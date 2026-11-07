// Remote.cpp - the browser link: frame/audio sender and input receiver.
// Wire format: port/include/re1_remote.h.
#include "re1_remote.h"
#include "re1_target.h"

#include "platform/platform.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---- sender -------------------------------------------------------------
std::atomic<int> s_frameBusy{0};      // 1 while the sender owns s_frame
uint16_t* s_frame     = nullptr;      // frame being sent
uint16_t* s_prev      = nullptr;      // what the bridge currently shows
uint8_t*  s_packet    = nullptr;      // encode buffer
size_t    s_packetCap = 0;
int       s_w = 0, s_h = 0;
uint32_t  s_frameNo = 0;
std::atomic<int> s_keyframeWanted{1};
std::atomic<int> s_infoWanted{1};
bool      s_started = false;

// Audio ring (host only): mixed stereo S16, sent as mono S16 at the mix rate.
#if defined(RE1_REMOTE_AUDIO)
const int kAudioRing = 22050;          // 1 s of mono
#else
const int kAudioRing = 2;              // the board never streams its mix
#endif
int16_t   s_audioRing[kAudioRing];
std::atomic<int> s_audioWrite{0};
int       s_audioRead = 0;
std::atomic<int> s_audioRate{0};

inline void put16(uint8_t*& p, uint32_t v) { *p++ = (uint8_t)v; *p++ = (uint8_t)(v >> 8); }
inline void put32(uint8_t*& p, uint32_t v) { put16(p, v & 0xFFFF); put16(p, v >> 16); }

uint8_t* BeginPacket(uint8_t type, size_t payloadMax)
{
    size_t need = RVL_HEADER + payloadMax + 1;
    if (need > s_packetCap) {
        free(s_packet);
        s_packet = (uint8_t*)re1t_alloc_large(need);
        s_packetCap = s_packet ? need : 0;
        if (!s_packet) return nullptr;
    }
    s_packet[0] = RVL_MAGIC0;
    s_packet[1] = RVL_MAGIC1;
    s_packet[2] = type;
    return s_packet + RVL_HEADER;
}

void EndPacket(uint8_t* end)
{
    size_t len = (size_t)(end - (s_packet + RVL_HEADER));
    uint8_t* p = s_packet + 3;
    put32(p, (uint32_t)len);
    *end++ = RVL_TAIL;
    re1t_remote_write(s_packet, (size_t)(end - s_packet));
}

void SendFrame()
{
    const int n = s_w * s_h;
    // Worst case of the delta codec: one token per pixel pair, bounded by raw.
    uint8_t* body = BeginPacket('F', 12 + (size_t)n * 2 + (size_t)n * 2);
    if (body == nullptr) return;
    uint8_t* p = body;
    put16(p, (uint32_t)s_w);
    put16(p, (uint32_t)s_h);
    const bool key = s_keyframeWanted.exchange(0) != 0 || (s_frameNo % 120) == 0;
    *p++ = key ? RVL_FRAME_RAW : RVL_FRAME_DELTA;
    *p++ = 0;
    put32(p, s_frameNo++);

    if (key) {
        for (int i = 0; i < n; ++i) put16(p, s_frame[i]);
    } else {
        int i = 0;
        while (i < n) {
            int skip = 0;
            while (i + skip < n && skip < 0xFFFF && s_frame[i + skip] == s_prev[i + skip]) ++skip;
            i += skip;
            int count = 0;
            // A literal run ends at 3+ equal pixels in a row (cheaper to skip).
            while (i + count < n && count < 0xFFFF) {
                if (s_frame[i + count] == s_prev[i + count]) {
                    int same = 0;
                    while (same < 3 && i + count + same < n &&
                           s_frame[i + count + same] == s_prev[i + count + same]) ++same;
                    if (same >= 3 || i + count + same >= n) break;
                    count += same;
                    continue;
                }
                ++count;
            }
            put16(p, (uint32_t)skip);
            put16(p, (uint32_t)count);
            for (int k = 0; k < count; ++k) put16(p, s_frame[i + k]);
            i += count;
        }
    }
    memcpy(s_prev, s_frame, (size_t)n * 2);
    EndPacket(p);
}

void SendAudio()
{
    const int rate = s_audioRate.load();
    if (rate <= 0) return;
    int w = s_audioWrite.load();
    int avail = (w - s_audioRead + kAudioRing) % kAudioRing;
    if (avail < 512) return;
    uint8_t* body = BeginPacket('A', 4 + (size_t)avail * 2);
    if (body == nullptr) return;
    uint8_t* p = body;
    put16(p, (uint32_t)rate);
    *p++ = 1;
    *p++ = 0;
    for (int i = 0; i < avail; ++i) {
        put16(p, (uint16_t)s_audioRing[s_audioRead]);
        s_audioRead = (s_audioRead + 1) % kAudioRing;
    }
    EndPacket(p);
}

void SendInfo()
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"target\":\"%s\",\"fbW\":%d,\"fbH\":%d,\"heapFree\":%ld}",
#if defined(RE1_TARGET_NAME)
             RE1_TARGET_NAME,
#else
             "unknown",
#endif
             RE1_FB_W, RE1_FB_H, re1t_heap_free());
    size_t n = strlen(json);
    uint8_t* body = BeginPacket('I', n);
    if (body == nullptr) return;
    memcpy(body, json, n);
    EndPacket(body + n);
}

void SenderMain(void*)
{
    for (;;) {
        bool did = false;
        if (s_infoWanted.exchange(0)) SendInfo();
        if (s_frameBusy.load(std::memory_order_acquire) == 1) {
            SendFrame();
            s_frameBusy.store(0, std::memory_order_release);
            did = true;
        }
        SendAudio();
        if (!did) re1t_sleep_ms(4);
    }
}

// ---- receiver -----------------------------------------------------------
uint8_t  s_rx[64];
int      s_rxLen = 0;
std::atomic<uint32_t> s_padMask{0};

}  // namespace
extern "C" void re1port_command(const char* line);   // PortMenu.cpp
namespace {

void HandlePacket(uint8_t type, const uint8_t* d, uint32_t len)
{
    switch (type) {
    case 'K':
        if (len >= 2) plat_key_event(d[0], d[1] ? TRUE : FALSE);
        break;
    case 'P':
        if (len >= 4) s_padMask.store((uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                                      ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24));
        break;
    case 'R':
        s_keyframeWanted.store(1);
        s_infoWanted.store(1);
        break;
    case 'C': {                         // a text command (re1tool.py room / cmd)
        char line[64];
        const uint32_t n = len < sizeof(line) - 1 ? len : (uint32_t)sizeof(line) - 1;
        memcpy(line, d, n);
        line[n] = 0;
        re1port_command(line);
        break;
    }
    default:
        break;
    }
}

}  // namespace

extern "C" void re1_remote_init(void)
{
    if (s_started || !re1t_remote_enabled()) return;
    s_w = RE1_FB_W;
    s_h = RE1_FB_H;
    s_frame = (uint16_t*)re1t_alloc_large((size_t)s_w * s_h * 2);
    s_prev  = (uint16_t*)re1t_alloc_large((size_t)s_w * s_h * 2);
    if (s_frame == nullptr || s_prev == nullptr) return;
    memset(s_prev, 0, (size_t)s_w * s_h * 2);
    s_started = true;
    re1t_thread_start("re1_remote", SenderMain, nullptr, 6144, 2, 0);
}

extern "C" void re1_remote_offer_frame(const uint16_t* fb, int w, int h)
{
    if (!s_started || w != s_w || h != s_h) return;
    if (s_frameBusy.load(std::memory_order_acquire) != 0) return;   // link busy: drop
    memcpy(s_frame, fb, (size_t)w * h * 2);
    s_frameBusy.store(1, std::memory_order_release);
}

extern "C" void re1_remote_offer_audio(const int16_t* stereo, int frames, int rate)
{
    if (!s_started) return;
    s_audioRate.store(rate);
    int w = s_audioWrite.load();
    for (int i = 0; i < frames; ++i) {
        s_audioRing[w] = (int16_t)(((int)stereo[i * 2] + (int)stereo[i * 2 + 1]) / 2);
        w = (w + 1) % kAudioRing;
    }
    s_audioWrite.store(w);
}

extern "C" void re1_remote_poll(void)
{
    if (!s_started) return;
    uint8_t buf[256];
    size_t n;
    while ((n = re1t_remote_read(buf, sizeof(buf))) > 0) {
        for (size_t i = 0; i < n; ++i) {
            uint8_t c = buf[i];
            // Resynchronise on the magic; payloads are small (commands <= 56 bytes).
            if (s_rxLen == 0 && c != RVL_MAGIC0) continue;
            if (s_rxLen == 1 && c != RVL_MAGIC1) { s_rxLen = (c == RVL_MAGIC0) ? 1 : 0; continue; }
            s_rx[s_rxLen++] = c;
            if (s_rxLen >= RVL_HEADER) {
                uint32_t len = (uint32_t)s_rx[3] | ((uint32_t)s_rx[4] << 8) |
                               ((uint32_t)s_rx[5] << 16) | ((uint32_t)s_rx[6] << 24);
                if (len > sizeof(s_rx) - RVL_HEADER - 1) { s_rxLen = 0; continue; }
                if ((uint32_t)s_rxLen == RVL_HEADER + len + 1) {
                    if (s_rx[s_rxLen - 1] == RVL_TAIL) HandlePacket(s_rx[2], s_rx + RVL_HEADER, len);
                    s_rxLen = 0;
                }
            }
        }
    }
}

extern "C" uint32_t re1_remote_pad_mask(void)
{
    return s_padMask.load();
}
