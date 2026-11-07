// GameMain.cpp - boot sequence and per-tick loop shared by every target.
//
// The body of decomp/src/platform/linux/main.cpp without SDL: read config.ini,
// bring up sound, the task stacks and the Marni system, then run main_loop()
// at the engine's 33 ms tick with the original frame limiter.
#include "Globals.h"
#include "platform/platform.h"
#include "system/AssetPath.h"
#include "system/ConfigFile.h"
#include "system/HdTextures.h"
#include "marni/MarniDX.h"
#include "marni/MarniSystem.h"

#include "MarniSW.h"
#include "re1_game.h"
#include "re1_remote.h"
#include "re1_prof.h"
#include "re1_fastmath.h"
#include "re1_target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" BOOL g_re1port_quit_requested;   // Platform.cpp

static const int kFrameIntervalMs = 33;    // 0x00441f26: 30 ticks/s

// ---------------------------------------------------------------------------
// Bits of the Linux platform layer that are not OS services
// ---------------------------------------------------------------------------
int ShowMessageBox(HWND, LPCSTR lpMsg, LPCSTR lpCaption, UINT)
{
    fprintf(stderr, "[RE1] %s: %s\n", lpCaption ? lpCaption : "RESIDENT EVIL", lpMsg ? lpMsg : "");
    return 1;   // IDOK
}

void CleanupVideoConfigAndSaveAllSettings(void)
{
    // 0x00497ea0: the exit hook that writes config.ini.
    if (g_bHasFinalizedSettings) return;
    g_bHasFinalizedSettings = TRUE;
    ConfigFile_Save();
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
// [Port] section of config.ini: settings of this port only (the decomp's own
// ConfigFile keeps unknown sections untouched when it rewrites the file).
static float ReadPortFloat(const char* key, float def)
{
    char path[512], norm[512];
    snprintf(path, sizeof(path), "%s/config.ini", re1t_data_root());
    FILE* f = fopen(plat_normalize_path(path, norm, sizeof(norm)), "r");
    if (f == NULL) return def;
    char line[256];
    bool inPort = false;
    float v = def;
    const size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f) != NULL) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '[') { inPort = strncasecmp(s, "[Port]", 6) == 0; continue; }
        if (inPort && strncasecmp(s, key, kl) == 0 && s[kl] == '=') v = (float)atof(s + kl + 1);
    }
    fclose(f);
    return v;
}

static DWORD s_lastStatsMs = 0;
static unsigned s_ticks = 0;
static unsigned s_statsFrames = 0;      // presented frames at the last stats line

extern "C" void re1port_big_init(void);   // BigAlloc.cpp
extern "C" void re1port_extras_tick(void); // Extras.cpp

extern "C" int re1_game_init(void)
{
    re1port_big_init();   // first, while the heap is still in one piece
    re1_remote_init();

    // The rasteriser's inline division (re1_fastmath.h) must give exactly the
    // library's quotients; a wrong one would show as shifted texels.
    {
        int checked = 0;
        const int bad = re1_div_selftest(&checked);
        fprintf(stderr, "[RE1] re1_div self-test: %d quotients, %s (inline: %d)\n", checked,
                bad == 0 ? "all bit-identical" : "SOME DIFFER", RE1_DIV_INLINE);
        if (bad != 0) fprintf(stderr, "[RE1] re1_div: %d of %d differ from a / b\n", bad, checked);
    }

    ConfigFile_EnsureExists();
    ConfigFile_Load();

    // The panel is fixed: whatever config.ini says, the game lays itself out
    // for the 320x240 logical back buffer the software renderer reports.
    g_dwScreenWidth = RE1_LOGICAL_W;
    g_dwScreenHeight = RE1_LOGICAL_H;
    g_bFullScreen = TRUE;
    g_bVSync = FALSE;
#if defined(RE1_NO_HD_TEXTURES)
    // HD packs need decoder threads and tens of MB; never on the board.
    HdTextures_Configure(false, false);
#endif

    g_hWnd = (HWND)(uintptr_t)1;   // opaque, never dereferenced

    StartSoundSystemAsync(g_hWnd);
    TaskScheduler_Init();

    InitializeMarniSystem();
    if (!IsGraphicsSystemReadyForOperation()) {
        fprintf(stderr, "[RE1] graphics system failed to initialize\n");
        return 0;
    }
    re1t_renderer_ready();   // after the framebuffers took their internal RAM

#if defined(RE1_PORT_TEXT_SCALE)
    {
        const float msg = ReadPortFloat("TextScale", 2.0f);
        const float menu = ReadPortFloat("MenuTextScale", 2.0f);
        MarniSW_SetTextScale(msg, menu);
        fprintf(stderr, "[RE1] text scale: messages %.2f, menus %.2f ([Port] in config.ini)\n",
                msg, menu);
    }
#endif
    MarniSW_SetShowFps(ReadPortFloat("ShowFps", 1.0f) >= 0.5f);

    g_bWindowFocused = TRUE;
    g_bWindowActive = TRUE;
    g_bQuitFlag = FALSE;
    g_dwGameTimer1 = plat_time_ms();
    s_lastStatsMs = g_dwGameTimer1;
    fprintf(stderr, "[RE1] boot complete, heap free %ld\n", re1t_heap_free());
    return 1;
}

// ---------------------------------------------------------------------------
// One loop iteration. Returns 0 when the game wants to exit, 1 otherwise;
// *ranTick tells the caller whether a game tick ran (it sleeps otherwise).
// ---------------------------------------------------------------------------
extern "C" int re1port_menu_tick(void);   // PortMenu.cpp
extern "C" int re1audio_open_streams(void);   // Audio.cpp

extern "C" int re1_game_step(int* ranTick)
{
    if (ranTick) *ranTick = 0;
    re1_remote_poll();
    if (g_re1port_quit_requested) g_bQuitFlag = TRUE;
    if (g_bQuitFlag) return 0;

    DWORD now = plat_time_ms();

    // 0x00441f0c: the frame limiter. Skip a tick that arrives early.
    BOOL bSkipFrame = FALSE;
    if (g_bUseFrameSkip == 1) {
        if (!g_bFrameSkipDetected) {
            if ((int)(kFrameIntervalMs + g_dwGameTimer1) > (int)now &&
                (int)(g_dwGameTimer1 - 0x10000) < (int)now) {
                bSkipFrame = TRUE;
            }
        }
    } else {
        if ((int)(g_dwGameTimer1 + 16) > (int)now && (int)(g_dwGameTimer1 - 0x10000) < (int)now) {
            bSkipFrame = TRUE;
        }
    }
    if (bSkipFrame) return 1;
    g_dwGameTimer1 = now;
    if (ranTick) *ranTick = 1;
    s_ticks++;

    // The port menu (PortMenu.cpp) freezes everything while it is up,
    // movies included.
    if (re1port_menu_tick()) return 1;

    // 0x00441dfe: while a movie is up the loop runs the video state machine
    // instead of the game.
    if (g_bMCINotifyEnabled) {
        if (g_bMCINotifyFlag) {
            UpdateVideoPlayback();
            return 1;
        }
        g_bMCINotifyFlag = TRUE;
    }

    {
        RE1_PROF_SCOPE(RE1_PROF_GAME);
        if (main_loop() == 0) return 0;
    }
    re1port_extras_tick();

    if ((int)(g_dwSystemTimer1 + 1000) < (int)now) g_dwSystemTimer1 = now;

#if !defined(RE1_QUIET)
    if (now - s_lastStatsMs >= 5000) {
        MarniSWStats st;
        MarniSW_GetStats(&st);
        char internal[48] = "";
        const long internalFree = re1t_heap_internal_free();
        if (internalFree >= 0) snprintf(internal, sizeof(internal), " internal=%ld", internalFree);
        // Sound streams each hold a file open (the board's FAT allows a fixed
        // number, CONFIG_RE1 storage.cpp max_files).
        snprintf(internal + strlen(internal), sizeof(internal) - strlen(internal), " streams=%d",
                 re1audio_open_streams());
        fprintf(stderr, "[RE1] t=%us ticks=%u frames=%u tris=%u sprites=%u tex=%u (%u KB) "
                        "heap=%ld largest=%ld%s\n",
                (unsigned)(now / 1000), s_ticks, st.frames, st.trianglesLastFrame,
                st.spritesLastFrame, st.textureCount, st.textureBytes / 1024, re1t_heap_free(),
                re1t_heap_largest(), internal);
        // Where a frame's time went (per presented frame, averaged over the
        // window), by exclusive bucket (re1_prof.h).
        uint64_t prof[RE1_PROF_COUNT];
        re1prof_take(prof);
        const unsigned nf = st.frames - s_statsFrames;
        const unsigned tris = MarniSW_TakeTriangleCount();
        if (nf > 0) {
            char line[384];
            uint64_t total = 0;
            for (int b = RE1_PROF_GAME; b < RE1_PROF_COUNT; ++b) total += prof[b];
            int n = snprintf(line, sizeof(line), "[PERF] %u frames, %u tris/frame, %.1f ms/frame:",
                             nf, tris / nf, total / 1000.0 / nf);
            for (int b = RE1_PROF_GAME; b < RE1_PROF_COUNT && n < (int)sizeof(line) - 24; ++b) {
                if (prof[b] / nf < 50) continue;             // under 0.05 ms
                n += snprintf(line + n, sizeof(line) - n, " %s %.1f", re1prof_name(b),
                              prof[b] / 1000.0 / nf);
            }
            fprintf(stderr, "%s\n", line);
            // How much each stage handled, per frame, to read the buckets
            // above per unit.
            uint32_t work[RE1_WORK_COUNT];
            re1prof_take_work(work);
            n = snprintf(line, sizeof(line), "[WORK] per frame:");
            for (int c = 0; c < RE1_WORK_COUNT && n < (int)sizeof(line) - 24; ++c)
                n += snprintf(line + n, sizeof(line) - n, " %s %u", re1prof_work_name(c),
                              (unsigned)(work[c] / nf));
            fprintf(stderr, "%s\n", line);
        }
        s_statsFrames = st.frames;
        s_lastStatsMs = now;
    }
#endif
    return 1;
}

extern "C" void re1_game_shutdown(void)
{
    CleanupVideoConfigAndSaveAllSettings();
    MarniDX_DestroyGlobal();
}

// Run until the game exits (targets that need nothing per frame).
extern "C" void re1_game_run(void)
{
    if (!re1_game_init()) return;
    for (;;) {
        int ran = 0;
        if (!re1_game_step(&ran)) break;
        if (!ran) re1t_sleep_ms(1);
    }
    re1_game_shutdown();
}
