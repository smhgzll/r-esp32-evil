// Video.cpp - plat_video_* backend for the ".rv1" movies (re1_rv1.h).
//
// Counterpart of decomp/src/platform/linux/video.cpp (ffmpeg). Same state
// machine contract with src/video/VideoPlayback.cpp: open/play to a cut point,
// play_from, one-shot end event, per-tick pump that presents the frame due now.
// The clock is the movie's audio stream position while the mixer runs, the
// wall clock otherwise - exactly the rule the Linux backend applies.
#include "Globals.h"
#include "platform/platform.h"
#include "marni/MarniDX.h"

#include "MarniSW.h"
#include "re1_audio.h"
#include "re1_rv1.h"
#include "re1_target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

PlatVideoOverlayCallback s_overlay = NULL;

FILE*      s_file = NULL;
Rv1Header  s_hdr;
uint32_t*  s_index = NULL;          // frameCount x {offset, size}
uint8_t*   s_frameBuf = NULL;       // one encoded frame
uint16_t*  s_pixels = NULL;         // decoded RGB565
MarniHandle s_tex = MARNI_NULL_HANDLE;
char       s_path[300];

int   s_frameIndex = -1;            // frame currently in s_pixels
int   s_baseFrame = 0;              // frame the current segment started at
int   s_playToFrame = 0;            // 0 = to the end
BOOL  s_active = FALSE;
BOOL  s_endEvent = FALSE;
DWORD s_startTicks = 0;
uint32_t s_audioBase = 0;           // audio frame the segment started at

double Fps()
{
    return (s_hdr.fpsDen != 0 && s_hdr.fpsNum != 0) ? (double)s_hdr.fpsNum / s_hdr.fpsDen : 10.0;
}

// "<dir>/capcom.avi" -> "<dir>/capcom.rv1", normalised for the filesystem.
void Rv1PathFor(const char* path, char* out, size_t size)
{
    char tmp[300];
    strncpy(tmp, path, sizeof(tmp) - 5);
    tmp[sizeof(tmp) - 5] = '\0';
    char* dot = strrchr(tmp, '.');
    char* sep = strrchr(tmp, '/');
    char* bsep = strrchr(tmp, '\\');
    if (bsep > sep) sep = bsep;
    if (dot != NULL && (sep == NULL || dot > sep)) *dot = '\0';
    strcat(tmp, ".rv1");
    plat_normalize_path(tmp, out, size);
}

void FreeAll()
{
    re1_audio_fmv_close();
    if (s_file) fclose(s_file);
    s_file = NULL;
    free(s_index);       s_index = NULL;
    re1t_free_large(s_frameBuf); s_frameBuf = NULL;
    re1t_free_large(s_pixels);   s_pixels = NULL;
    if (s_tex != MARNI_NULL_HANDLE && Marni_DX() != NULL) Marni_DX()->DestroyTexture(s_tex);
    s_tex = MARNI_NULL_HANDLE;
    s_active = FALSE;
    s_frameIndex = -1;
}

bool ReadHeader(FILE* f, Rv1Header* h)
{
    if (fread(h, 1, sizeof(*h), f) != sizeof(*h)) return false;
    return memcmp(h->magic, "RV1", 4) == 0 && h->version == 1 && h->width > 0 &&
           h->height > 0 && h->frameCount > 0;
}

bool DecodeFrame(int frame)
{
    if (frame < 0 || (uint32_t)frame >= s_hdr.frameCount) return false;
    if (frame == s_frameIndex) return true;
    const uint32_t off = s_index[frame * 2];
    const uint32_t size = s_index[frame * 2 + 1];
    const size_t px = (size_t)s_hdr.width * s_hdr.height;
    if (fseek(s_file, (long)off, SEEK_SET) != 0) return false;

    if (s_hdr.codec == RV1_CODEC_RGB565) {
        if (size < px * 2) return false;
        if (fread(s_pixels, 1, px * 2, s_file) != px * 2) return false;
    } else if (s_hdr.codec == RV1_CODEC_PAL8) {
        if (size < 512 + px) return false;
        if (fread(s_frameBuf, 1, 512 + px, s_file) != 512 + px) return false;
        const uint16_t* pal = (const uint16_t*)s_frameBuf;
        const uint8_t* idx = s_frameBuf + 512;
        for (size_t i = 0; i < px; ++i) s_pixels[i] = pal[idx[i]];
    } else {
        return false;
    }
    s_frameIndex = frame;
    return true;
}

void Present()
{
    MarniDX* dx = Marni_DX();
    if (dx == NULL || s_pixels == NULL || s_frameIndex < 0) return;
    if (s_tex == MARNI_NULL_HANDLE) {
        s_tex = MarniSW_CreateTexture565(s_hdr.width, s_hdr.height, s_pixels);
        if (s_tex == MARNI_NULL_HANDLE) return;
    } else {
        MarniSW_UpdateTexture565(s_tex, s_hdr.width, s_hdr.height, s_pixels);
    }
    DWORD bw = 0, bh = 0;
    dx->GetBackBufferSize(&bw, &bh);
    dx->Clear(0.0f, 0.0f, 0.0f, 1.0f);
    const int frameMs = (int)((double)s_frameIndex * 1000.0 / Fps() + 0.5);
    if (s_overlay == NULL || !s_overlay(s_tex, frameMs)) {
        dx->DrawSprite(0.0f, 0.0f, (float)bw, (float)bh, 0.0f, 0.0f, 1.0f, 1.0f,
                       0xFFFFFFFFu, s_tex, MARNI_SAMPLER_POINT, MARNI_BLEND_DISABLE);
    }
    dx->Present();
}

void StartSegment(int fromFrame)
{
    s_baseFrame = fromFrame;
    s_startTicks = plat_time_ms();
    s_audioBase = (uint32_t)((double)fromFrame / Fps() * s_hdr.audioRate + 0.5);
    if (s_hdr.audioFrames > 0) re1_audio_fmv_play(s_audioBase);
    s_frameIndex = -1;
    DecodeFrame(fromFrame);
}

}  // namespace

void plat_video_set_overlay_callback(PlatVideoOverlayCallback callback)
{
    s_overlay = callback;
}

BOOL plat_video_init(void)
{
    return TRUE;
}

BOOL plat_video_open_and_play(const char* path, int playToMs)
{
    FreeAll();
    Rv1PathFor(path, s_path, sizeof(s_path));
    s_file = fopen(s_path, "rb");
    if (s_file == NULL) {
        fprintf(stderr, "[VIDEO] no movie %s (run tools/re1tool.py prepare)\n", s_path);
        return FALSE;
    }
    if (!ReadHeader(s_file, &s_hdr)) {
        const uint8_t* b = (const uint8_t*)&s_hdr;
        fprintf(stderr, "[VIDEO] bad rv1 header: %s (%02x %02x %02x %02x v%u %ux%u n=%u)\n",
                s_path, b[0], b[1], b[2], b[3], (unsigned)s_hdr.version, (unsigned)s_hdr.width,
                (unsigned)s_hdr.height, (unsigned)s_hdr.frameCount);
        FreeAll();
        return FALSE;
    }
    const size_t px = (size_t)s_hdr.width * s_hdr.height;
    s_index = (uint32_t*)malloc((size_t)s_hdr.frameCount * 8);
    s_pixels = (uint16_t*)re1t_alloc_large(px * 2);
    s_frameBuf = (uint8_t*)re1t_alloc_large(512 + px);
    if (s_index == NULL || s_pixels == NULL || s_frameBuf == NULL ||
        fseek(s_file, (long)s_hdr.indexOffset, SEEK_SET) != 0 ||
        fread(s_index, 8, s_hdr.frameCount, s_file) != s_hdr.frameCount) {
        FreeAll();
        return FALSE;
    }
    if (s_hdr.audioFrames > 0 && s_hdr.audioBits == 16) {
        re1_audio_fmv_open(s_path, (long)s_hdr.audioOffset, (int)s_hdr.audioRate,
                           s_hdr.audioChannels, s_hdr.audioFrames);
    }
    s_playToFrame = playToMs > 0 ? (int)((double)playToMs / 1000.0 * Fps() + 0.5) : 0;
    s_active = TRUE;
    s_endEvent = FALSE;
    StartSegment(0);
    Present();
    fprintf(stderr, "[VIDEO] playing %s (%ux%u, %.2f fps, %u frames)\n", s_path,
            s_hdr.width, s_hdr.height, Fps(), (unsigned)s_hdr.frameCount);
    return TRUE;
}

int plat_video_probe_duration_ms(const char* path)
{
    char p[300];
    Rv1PathFor(path, p, sizeof(p));
    FILE* f = fopen(p, "rb");
    if (f == NULL) return 0;
    Rv1Header h;
    int ms = ReadHeader(f, &h) ? (int)h.durationMs : 0;
    fclose(f);
    return ms;
}

void plat_video_play_from(int fromMs)
{
    if (s_file == NULL) return;
    s_playToFrame = 0;
    s_endEvent = FALSE;
    s_active = TRUE;
    StartSegment((int)((double)fromMs / 1000.0 * Fps() + 0.5));
    Present();
}

void plat_video_stop(void)
{
    s_active = FALSE;
    re1_audio_fmv_close();
}

void plat_video_close(void)
{
    FreeAll();
}

BOOL plat_video_is_active(void)
{
    return s_active;
}

BOOL plat_video_take_end_event(void)
{
    if (!s_endEvent) return FALSE;
    s_endEvent = FALSE;
    return TRUE;
}

#if defined(RE1_PORT_BENCH)
extern int g_re1benchRequest;   // DebugMenu.cpp: a room tour waits for a game
#endif

void plat_video_tick(void)
{
    if (!s_active || s_file == NULL) return;
#if defined(RE1_PORT_BENCH)
    // A room tour was asked for (re1tool.py room ...): the movie ends here, as
    // if it had played out, and the title starts the game for it.
    if (g_re1benchRequest != 0) {
        re1_audio_fmv_close();
        s_active = FALSE;
        s_endEvent = TRUE;
        return;
    }
#endif

    const double wall = (double)(plat_time_ms() - s_startTicks) / 1000.0;
    double seconds = wall;
    const long pos = re1_audio_fmv_pos();
    if (pos >= 0 && s_hdr.audioRate > 0) {
        const double a = (double)((uint32_t)pos - s_audioBase) / (double)s_hdr.audioRate;
        if (a >= wall - 0.5 && a <= wall + 0.25) seconds = a;
    }
    int target = s_baseFrame + (int)(seconds * Fps());
    const int last = (int)s_hdr.frameCount - 1;
    if (target > last) target = last;

    // Frames are independent, so seek straight to the one due now.
    DecodeFrame(target);
    Present();

    if (s_playToFrame > 0 && s_frameIndex >= s_playToFrame) {
        s_active = FALSE;
        s_endEvent = TRUE;
        return;
    }
    if (s_baseFrame + (int)(seconds * Fps()) > last) {
        s_active = FALSE;
        s_endEvent = TRUE;
    }
}

void plat_video_set_window_mode(BOOL) {}
