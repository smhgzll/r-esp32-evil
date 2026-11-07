// Audio.cpp - DirectSound + software mixer for every r-esp32-evil target.
//
// Counterpart of decomp/src/platform/linux/audio.cpp (SDL2). The contract with
// src/game/SoundApi.cpp is identical:
//   - bank indices 1..80, one per loaded WAV (0 == none)
//   - PlaySound(bank, slot): slot != 0 loops forever, slot == 0 plays once
//   - volume in DirectSound millibels (0 full, -10000 silence) -> 10^(mB/2000)
//   - GetStatus is 1 while the bank plays
//
// Two things differ, both for an 8 MB-PSRAM board:
//   1. Integer mixing (Q16 positions, Q15 gains) instead of double/float.
//   2. Large banks are STREAMED from the SD card instead of loaded whole: the
//      PC tree's BGM and long voice clips are 0.5-2.9 MB of PCM each. A
//      streamed bank keeps a ring of decoded frames that a low-priority pump
//      thread refills from the file (re1_audio_pump_main); the mixer callback
//      itself never touches the filesystem.
// The FMV player (Video.cpp) uses the same stream machinery for movie audio,
// and its position is the movie clock, as on Linux.
#include "Globals.h"
#include "platform/platform.h"
#include "marni/MarniSound.h"
#include "system/AssetPath.h"
#include "system/AudioFile.h"

#include "re1_audio.h"
#include "re1_remote.h"
#include "re1_target.h"

#include <math.h>
#include <stdio.h>
#include <atomic>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>

#ifndef RE1_AUDIO_RATE
#define RE1_AUDIO_RATE 22050
#endif
#ifndef RE1_AUDIO_STREAM_THRESHOLD
#define RE1_AUDIO_STREAM_THRESHOLD (96 * 1024)   // bytes of PCM
#endif
#ifndef RE1_AUDIO_RING_FRAMES
#define RE1_AUDIO_RING_FRAMES 16384                // per streamed source, power of 2
#endif
#define MAX_BANKS 81                                // 1..80
#define STREAM_CHUNK 2048                           // frames per pump read

namespace {

// ---------------------------------------------------------------------------
// Streamed PCM source
// ---------------------------------------------------------------------------
struct Stream {
    FILE*    f = nullptr;
    long     dataOffset = 0;      // file offset of frame 0
    int      channels = 1;
    int      bits = 16;
    uint32_t total = 0;           // frames in the file
    uint32_t loopBegin = 0;       // frames
    uint32_t loopEnd = 0;         // frames, exclusive (== total when unset)
    int16_t* ring = nullptr;      // RE1_AUDIO_RING_FRAMES * channels
    // Virtual frame indices: the mixer reads [ringStart, ringStart+ringCount).
    // Past loopEnd a looping stream keeps counting; FileFrame() wraps it.
    uint32_t ringStart = 0;
    uint32_t ringCount = 0;
    uint32_t gen = 0;             // bumped on every rewind; stale fills drop
    bool     loop = false;
    bool     wanted = false;      // the pump should keep this one full
    bool     busy = false;        // the pump is reading the file right now
};

uint32_t FileFrame(const Stream& s, uint32_t v)
{
    if (v < s.loopEnd || !s.loop) return v;
    uint32_t len = s.loopEnd - s.loopBegin;
    if (len == 0) return s.loopBegin;
    return s.loopBegin + (v - s.loopBegin) % len;
}

// Streams with an open file (each holds a file handle for as long as its bank
// lives; the board's FAT volume allows a fixed number) - the [RE1] line.
std::atomic<int> s_openStreams{0};

bool StreamOpen(Stream& s, const char* path, long dataOffset, int channels, int bits,
                uint32_t frames)
{
    s.f = fopen(path, "rb");
    if (s.f == nullptr) {
        fprintf(stderr, "[AUDIO] cannot open %s (%s, %d streams open)\n", path, strerror(errno),
                s_openStreams.load());
        return false;
    }
    s.ring = (int16_t*)re1t_alloc_large((size_t)RE1_AUDIO_RING_FRAMES * channels * 2);
    if (s.ring == nullptr) { fclose(s.f); s.f = nullptr; return false; }
    s_openStreams.fetch_add(1);
    s.dataOffset = dataOffset;
    s.channels = channels;
    s.bits = bits;
    s.total = frames;
    s.loopBegin = 0;
    s.loopEnd = frames;
    s.ringStart = s.ringCount = 0;
    s.gen++;
    return true;
}

// Caller holds the audio lock. Waits out an in-flight pump read first, so the
// FILE* is never closed under it.
void StreamClose(Stream& s)
{
    s.wanted = false;
    while (s.busy) {
        re1t_audio_unlock();
        re1t_sleep_ms(1);
        re1t_audio_lock();
    }
    if (s.f) {
        fclose(s.f);
        s_openStreams.fetch_sub(1);
    }
    s.f = nullptr;
    re1t_free_large(s.ring);
    s.ring = nullptr;
    s.wanted = false;
}

// Caller holds the audio lock.
void StreamRewind(Stream& s, uint32_t frame)
{
    s.ringStart = frame;
    s.ringCount = 0;
    s.gen++;
}

// One refill step for a stream (called WITHOUT the audio lock held).
// Returns true when it read something.
bool StreamPump(Stream& s)
{
    if (s.f == nullptr || !s.wanted) return false;

    re1t_audio_lock();
    if (s.f == nullptr || !s.wanted) { re1t_audio_unlock(); return false; }
    const uint32_t gen = s.gen;
    const uint32_t from = s.ringStart + s.ringCount;
    const uint32_t space = RE1_AUDIO_RING_FRAMES - s.ringCount;
    const bool loop = s.loop;
    if (space < STREAM_CHUNK / 2 || (!loop && from >= s.total)) {
        re1t_audio_unlock();
        return false;
    }
    s.busy = true;
    re1t_audio_unlock();

    uint32_t want = space < STREAM_CHUNK ? space : STREAM_CHUNK;
    // Do not read across the loop end / file end in one go.
    uint32_t ff = FileFrame(s, from);
    uint32_t limit = loop ? s.loopEnd : s.total;
    if (ff + want > limit) want = limit - ff;
    if (want == 0) { re1t_audio_lock(); s.busy = false; re1t_audio_unlock(); return false; }

    static uint8_t raw[STREAM_CHUNK * 4];
    static int16_t pcm[STREAM_CHUNK * 2];
    const int bps = s.bits / 8;
    const size_t bytes = (size_t)want * s.channels * bps;
    size_t got = 0;
    if (fseek(s.f, s.dataOffset + (long)ff * s.channels * bps, SEEK_SET) == 0)
        got = fread(raw, 1, bytes, s.f);
    uint32_t frames = (uint32_t)(got / ((size_t)s.channels * bps));
    if (frames == 0) { re1t_audio_lock(); s.busy = false; re1t_audio_unlock(); return false; }
    const uint32_t samples = frames * s.channels;
    if (bps == 1) {
        for (uint32_t i = 0; i < samples; ++i) pcm[i] = (int16_t)(((int)raw[i] - 128) << 8);
    } else {
        for (uint32_t i = 0; i < samples; ++i)
            pcm[i] = (int16_t)((unsigned)raw[i * 2] | ((unsigned)raw[i * 2 + 1] << 8));
    }

    re1t_audio_lock();
    if (s.gen == gen && s.ringStart + s.ringCount == from) {
        for (uint32_t i = 0; i < frames; ++i) {
            uint32_t slot = (from + i) & (RE1_AUDIO_RING_FRAMES - 1);
            for (int c = 0; c < s.channels; ++c)
                s.ring[slot * s.channels + c] = pcm[i * s.channels + c];
        }
        s.ringCount += frames;
    }
    s.busy = false;
    re1t_audio_unlock();
    return true;
}

// ---------------------------------------------------------------------------
// Banks
// ---------------------------------------------------------------------------
struct Bank {
    bool     active = false;
    bool     playing = false;
    bool     loop = false;
    BYTE*    wavData = nullptr;   // in-memory banks: owned buffer
    const BYTE* pcm = nullptr;
    uint32_t frames = 0;
    int      sampleRate = 22050;
    int      channels = 1;
    int      bitsPerSample = 16;
    int      vol = 0;             // millibels
    int      pan = 400;
    int      gainL = 0, gainR = 0;  // Q15
    uint32_t step = 1u << 16;     // Q16 source frames per output frame
    uint64_t pos = 0;             // Q16 frame position (virtual for streams)
    uint32_t loopBegin = 0, loopEnd = 0;   // frames; loopEnd 0 = whole buffer
    bool     streamed = false;
    Stream   stream;
    int      status = 0;
};

// Built at start-up rather than a static array: Bank's defaults are not zero,
// which would put 9.7 KB in .data - the target's internal RAM - while a heap
// block this size comes from PSRAM.
Bank* const s_banks = new Bank[MAX_BANKS];
Stream s_fmv;                       // movie audio
bool   s_fmvOn = false;
uint64_t s_fmvPos = 0;              // Q16 frames played
uint32_t s_fmvStep = 1u << 16;
int    s_fmvRate = 22050;
int    s_outRate = RE1_AUDIO_RATE;
bool   s_open = false;
int    s_suspended = 0;

int MillibelsToQ15(int mB)
{
    if (mB <= -10000) return 0;
    if (mB >= 0) return 32767;
    return (int)(32767.0 * pow(10.0, (double)mB / 2000.0));
}

void UpdateGains(Bank& b)
{
    int amp = MillibelsToQ15(b.vol);
    // Linux build: panL = (10000 - pan) / 20000, panR = (10000 + pan) / 20000.
    b.gainL = (int)((int64_t)amp * (10000 - b.pan) / 20000);
    b.gainR = (int)((int64_t)amp * (10000 + b.pan) / 20000);
}

inline int32_t MemSample(const Bank& b, uint32_t frame, int ch)
{
    size_t idx = (size_t)frame * b.channels + ch;
    if (b.bitsPerSample == 8) return ((int32_t)b.pcm[idx] - 128) << 8;
    return (int16_t)((unsigned)b.pcm[idx * 2] | ((unsigned)b.pcm[idx * 2 + 1] << 8));
}

int32_t s_acc[1024 * 2];

void MixBank(Bank& b, int frames)
{
    const bool st = b.streamed;
    const uint32_t end = st ? b.stream.total
                            : ((b.loop && b.loopEnd != 0) ? b.loopEnd : b.frames);
    const uint32_t begin = (b.loopEnd != 0) ? b.loopBegin : 0;
    const int ch1 = b.channels > 1 ? 1 : 0;

    for (int f = 0; f < frames; ++f) {
        uint32_t i = (uint32_t)(b.pos >> 16);
        int32_t l, r;
        if (st) {
            Stream& s = b.stream;
            if (!b.loop && i >= s.total) { b.playing = false; b.status = 0; b.pos = 0; break; }
            if (i < s.ringStart || i >= s.ringStart + s.ringCount) break;   // underrun: stall
            uint32_t slot = i & (RE1_AUDIO_RING_FRAMES - 1);
            l = s.ring[slot * s.channels];
            r = s.ring[slot * s.channels + ch1];
            // Release what has been consumed so the pump can refill it.
            uint32_t consumed = i - s.ringStart;
            s.ringStart += consumed;
            s.ringCount -= consumed;
        } else {
            if (i >= end) {
                if (b.loop) {
                    b.pos -= (uint64_t)(end - begin) << 16;
                    i = (uint32_t)(b.pos >> 16);
                    if (i >= end) { b.pos = (uint64_t)begin << 16; i = begin; }
                } else { b.playing = false; b.status = 0; b.pos = 0; break; }
            }
            l = MemSample(b, i, 0);
            r = MemSample(b, i, ch1);
        }
        s_acc[f * 2 + 0] += (l * b.gainL) >> 15;
        s_acc[f * 2 + 1] += (r * b.gainR) >> 15;
        b.pos += b.step;
    }
}

void MixFmv(int frames)
{
    Stream& s = s_fmv;
    const int ch1 = s.channels > 1 ? 1 : 0;
    for (int f = 0; f < frames; ++f) {
        uint32_t i = (uint32_t)(s_fmvPos >> 16);
        if (i >= s.total) { s_fmvOn = false; break; }
        if (i < s.ringStart || i >= s.ringStart + s.ringCount) break;
        uint32_t slot = i & (RE1_AUDIO_RING_FRAMES - 1);
        s_acc[f * 2 + 0] += s.ring[slot * s.channels];
        s_acc[f * 2 + 1] += s.ring[slot * s.channels + ch1];
        uint32_t consumed = i - s.ringStart;
        s.ringStart += consumed;
        s.ringCount -= consumed;
        s_fmvPos += s_fmvStep;
    }
}

void Pull(int16_t* out, int frames)
{
    while (frames > 0) {
        int n = frames > 1024 ? 1024 : frames;
        memset(s_acc, 0, (size_t)n * 2 * sizeof(int32_t));
        re1t_audio_lock();
        if (!s_suspended) {
            if (s_fmvOn && s_fmv.ring) MixFmv(n);
            for (int b = 1; b < MAX_BANKS; ++b) {
                Bank& bank = s_banks[b];
                if (bank.active && bank.playing) MixBank(bank, n);
            }
        }
        re1t_audio_unlock();
        for (int i = 0; i < n * 2; ++i) {
            int32_t v = s_acc[i];
            out[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
#if defined(RE1_REMOTE_AUDIO)
        re1_remote_offer_audio(out, n, s_outRate);
#endif
        out += n * 2;
        frames -= n;
    }
}

void PumpMain(void*)
{
    for (;;) {
        bool any = false;
        any |= StreamPump(s_fmv);
        for (int b = 1; b < MAX_BANKS; ++b) {
            if (s_banks[b].streamed) any |= StreamPump(s_banks[b].stream);
        }
        if (!any) re1t_sleep_ms(10);
    }
}

void FreeBank(Bank& b)
{
    b.playing = false;
    b.status = 0;
    b.pos = 0;
    free(b.wavData);
    b.wavData = nullptr;
    b.pcm = nullptr;
    if (b.streamed) StreamClose(b.stream);
    b.streamed = false;
    b.active = false;
}

// Minimal RIFF/WAVE reader for streamed banks: PCM 8/16-bit, 1-2 channels.
bool ParseWavHeader(FILE* f, long* dataOffset, uint32_t* dataBytes, int* rate,
                    int* channels, int* bits)
{
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
        return false;
    bool haveFmt = false;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) return false;
        uint32_t len = (uint32_t)c[4] | ((uint32_t)c[5] << 8) | ((uint32_t)c[6] << 16) |
                       ((uint32_t)c[7] << 24);
        if (memcmp(c, "fmt ", 4) == 0) {
            uint8_t fm[16];
            if (len < 16 || fread(fm, 1, 16, f) != 16) return false;
            int fmt = fm[0] | (fm[1] << 8);
            *channels = fm[2] | (fm[3] << 8);
            *rate = (int)((uint32_t)fm[4] | ((uint32_t)fm[5] << 8) | ((uint32_t)fm[6] << 16));
            *bits = fm[14] | (fm[15] << 8);
            if (fmt != 1 || (*bits != 8 && *bits != 16) || *channels < 1 || *channels > 2)
                return false;
            haveFmt = true;
            if (fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR) != 0) return false;
        } else if (memcmp(c, "data", 4) == 0) {
            if (!haveFmt) return false;
            *dataOffset = ftell(f);
            *dataBytes = len;
            return true;
        } else {
            if (fseek(f, (long)(len + (len & 1)), SEEK_CUR) != 0) return false;
        }
    }
}

uint32_t StepFor(int srcRate)
{
    return (uint32_t)(((uint64_t)srcRate << 16) / (uint32_t)s_outRate);
}

}  // namespace

extern "C" int re1audio_open_streams(void) { return s_openStreams.load(); }

// ===========================================================================
// DirectSound
// ===========================================================================

DirectSound::DirectSound(HWND hwnd)
{
    (void)hwnd;
    memset(this, 0, sizeof(*this));
    // SoundApi.cpp gates on the DWORD at +0x10; +0x14 is the named flag.
    *(int*)((BYTE*)this + 0x10) = 1;
    m_bInitialized = 1;
}

void DirectSound::compact(void) {}

int DirectSound::GetStatus(int bank)
{
    if (bank <= 0 || bank >= MAX_BANKS) return 0;
    return s_banks[bank].status;
}

void DirectSound::Release(void)  { s_suspended = 1; }
void DirectSound::Reload(void)   { s_suspended = 0; }

void DirectSound::DestroySound(int bank)
{
    if (bank <= 0 || bank >= MAX_BANKS) return;
    re1t_audio_lock();
    FreeBank(s_banks[bank]);   // StreamClose waits out an in-flight pump read
    re1t_audio_unlock();
}

int DirectSound::NewDirectSoundBuffer(DWORD*)
{
    return 1;
}

void DirectSound::StopSound(int bank)
{
    if (bank <= 0 || bank >= MAX_BANKS) return;
    re1t_audio_lock();
    s_banks[bank].playing = false;   // cursor stays: a stopped bank resumes
    s_banks[bank].status = 0;
    re1t_audio_unlock();
}

void DirectSound::PlaySound(int bank, unsigned int slot)
{
    if (bank <= 0 || bank >= MAX_BANKS) return;
    re1t_audio_lock();
    Bank& b = s_banks[bank];
    if (b.active) {
        // 0x0041ee60: SetCurrentPosition(0) only while the buffer is playing.
        if (b.playing) {
            b.pos = 0;
            if (b.streamed) StreamRewind(b.stream, 0);
        }
        b.loop = (slot != 0);
        if (b.streamed) {
            b.stream.loop = b.loop;
            b.stream.wanted = true;
        }
        b.playing = true;
        b.status = 1;
    }
    re1t_audio_unlock();
}

void DirectSound::SetVol(int bank, int vol)
{
    if (bank <= 0 || bank >= MAX_BANKS) return;
    if (vol > -1) vol = -1;
    if (vol < -9999) vol = -9999;
    re1t_audio_lock();
    s_banks[bank].vol = vol;
    UpdateGains(s_banks[bank]);
    re1t_audio_unlock();
}

void DirectSound::SetPan(int bank, int pan)
{
    if (bank <= 0 || bank >= MAX_BANKS) return;
    if (pan > 10000) pan = 10000;
    if (pan < -10000) pan = -10000;
    re1t_audio_lock();
    s_banks[bank].pan = pan;
    UpdateGains(s_banks[bank]);
    re1t_audio_unlock();
}

int DirectSound::GetVol(int bank)
{
    if (bank <= 0 || bank >= MAX_BANKS) return 0;
    return s_banks[bank].vol;
}

int DirectSound::CreateSound(const char* wavName)
{
    if (wavName == NULL) return 0;

    char resolved[260];
    const char* actualPath = ResolveAssetRoot(wavName, resolved, sizeof(resolved));
    char found[260];
    if (!AudioFile_Find(actualPath, found, sizeof(found))) {
        static int s_failLogged = 0;
        if (s_failLogged++ < 4) fprintf(stderr, "[AUDIO] could not find bank: %s\n", actualPath);
        return 0;
    }

    int bank = 0;
    for (int i = 1; i < MAX_BANKS; ++i) {
        if (!s_banks[i].active) { bank = i; break; }
    }
    if (bank == 0) return 0;

    Bank nb;
    nb.vol = 0;
    nb.pan = 400;   // same default as the Windows backend

    // Large plain WAVs stream; everything else (small WAVs, the migrator's
    // .ogg files, files carrying a PS1 loop region) loads whole.
    bool streamed = false;
    const char* ext = strrchr(found, '.');
    if (ext != NULL && strcasecmp(ext, ".wav") == 0) {
        FILE* f = fopen(found, "rb");
        if (f != NULL) {
            long off = 0; uint32_t bytes = 0; int rate = 0, ch = 0, bits = 0;
            if (ParseWavHeader(f, &off, &bytes, &rate, &ch, &bits) &&
                bytes >= (uint32_t)RE1_AUDIO_STREAM_THRESHOLD) {
                fclose(f);
                f = NULL;
                uint32_t frames = bytes / (uint32_t)(ch * (bits / 8));
                if (StreamOpen(nb.stream, found, off, ch, bits, frames)) {
                    streamed = true;
                    nb.stream.wanted = true;   // pre-fill the start of the bank
                    nb.sampleRate = rate;
                    nb.channels = ch;
                    nb.bitsPerSample = bits;
                    nb.frames = frames;
                }
            }
            if (f) fclose(f);
        }
    }

    if (!streamed) {
        AudioFileData af;
        if (!AudioFile_Load(found, &af)) {
            fprintf(stderr, "[AUDIO] could not load bank: %s\n", found);
            return 0;
        }
        nb.wavData = af.buffer;
        nb.pcm = af.pcm;
        nb.sampleRate = af.sampleRate;
        nb.channels = af.channels;
        nb.bitsPerSample = af.bitsPerSample;
        const int bps = af.bitsPerSample / 8;
        nb.frames = (bps > 0 && af.channels > 0) ? af.pcmSize / (uint32_t)(bps * af.channels) : 0;
        nb.loopBegin = af.loopBegin;
        nb.loopEnd = af.loopEnd > af.loopBegin ? af.loopEnd : 0;
    }
    nb.streamed = streamed;
    nb.step = StepFor(nb.sampleRate);
    nb.active = true;
    UpdateGains(nb);

    re1t_audio_lock();
    s_banks[bank] = nb;
    re1t_audio_unlock();
    return bank;
}

void DirectSound::ErrorRoutine(int) {}

// ===========================================================================
// Engine lifecycle
// ===========================================================================

void InitializeSoundSystem(void)
{
    // The class keeps the original 0x32DDC-byte layout (203 KB), but nothing
    // outside this file reads past the +0x10/+0x14 flags (SoundApi.cpp gates
    // on +0x10), and these methods keep their state in s_banks. A 64-byte
    // block is enough - and on the board 200 KB of PSRAM is worth having.
    DirectSound* mgr = (DirectSound*)calloc(1, 64);
    *(int*)((BYTE*)mgr + 0x10) = 1;
    *(int*)((BYTE*)mgr + 0x14) = 1;   // m_bInitialized
    g_pDirectSound = mgr;
    g_SoundManager = (void*)mgr;

    s_outRate = RE1_AUDIO_RATE;
    s_open = re1t_audio_open(s_outRate, Pull) != 0;
    if (!s_open) {
        fprintf(stderr, "[AUDIO] no output device; banks load but are silent\n");
    }
    re1t_thread_start("re1_apump", PumpMain, nullptr, 6144, 4, 0);
}

void CleanupSoundManagerResources(void)
{
    for (int i = 1; i < MAX_BANKS; ++i) {
        re1t_audio_lock();
        FreeBank(s_banks[i]);
        re1t_audio_unlock();
    }
}

// ===========================================================================
// Movie audio (re1_audio.h)
// ===========================================================================

extern "C" int re1_audio_fmv_open(const char* path, long dataOffset, int rate,
                                  int channels, uint32_t frames)
{
    re1_audio_fmv_close();
    Stream s;
    if (!StreamOpen(s, path, dataOffset, channels, 16, frames)) return 0;
    re1t_audio_lock();
    s_fmv = s;
    s_fmv.loop = false;
    s_fmv.wanted = true;
    s_fmvRate = rate;
    s_fmvStep = StepFor(rate);
    s_fmvPos = 0;
    s_fmvOn = false;
    re1t_audio_unlock();
    return 1;
}

extern "C" void re1_audio_fmv_play(uint32_t fromFrame)
{
    re1t_audio_lock();
    if (s_fmv.ring != nullptr) {
        StreamRewind(s_fmv, fromFrame);
        s_fmvPos = (uint64_t)fromFrame << 16;
        s_fmvOn = s_open;
    }
    re1t_audio_unlock();
}

extern "C" void re1_audio_fmv_close(void)
{
    re1t_audio_lock();
    s_fmvOn = false;
    s_fmv.wanted = false;
    StreamClose(s_fmv);
    re1t_audio_unlock();
}

extern "C" long re1_audio_fmv_pos(void)
{
    if (!s_fmvOn) return -1;
    return (long)(s_fmvPos >> 16);
}

extern "C" int re1_audio_fmv_rate(void)
{
    return s_fmvRate;
}

extern "C" int re1_audio_running(void)
{
    return s_open ? 1 : 0;
}
