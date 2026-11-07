// re1_target.h - the services a *target* (ESP32-S3 board, QEMU, PC host
// simulator) provides to the shared r-esp32-evil port layer.
//
// Layering:
//
//   decomp/src/game, marni, system  (unchanged game code, GPL-3.0 upstream)
//        |  platform.h / MarniDX.h / DirectSound / CMarniDirectInput
//   port/common + port/marni        (this port: one implementation for all
//        |                           targets of what the Linux build does
//        |                           with SDL2 / OpenGL / ffmpeg)
//   re1_target.h                    (this file)
//        |
//   port/host  or  firmware/components/re1_board   (per-target drivers)
//
// Everything here is plain C so the ESP-IDF C drivers can implement it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Framebuffer geometry.
//
// The game lays itself out for a 320x240 back buffer (the smallest mode the
// PC release supports). The software renderer rasterises that logical space
// at RE1_FB_W x RE1_FB_H - 160x120 on the 1.8" ST7735 - so every coordinate is
// scaled by RE1_FB_W/320 and every texture is box-filtered by the same factor
// when it is uploaded (see port/marni/MarniDX_SW.cpp).
// ---------------------------------------------------------------------------
#ifndef RE1_FB_W
#define RE1_FB_W 160
#endif
#ifndef RE1_FB_H
#define RE1_FB_H 120
#endif
#define RE1_LOGICAL_W 320
#define RE1_LOGICAL_H 240

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
uint32_t re1t_time_ms(void);
// RE1_HOT_FN: a function the renderer runs per vertex / pixel, kept in
// internal RAM (IRAM) on the board so the instruction cache never misses on
// it. Only those: internal RAM is shared with data and Bluetooth.
#ifndef RE1_HOT_FN
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define RE1_HOT_FN IRAM_ATTR
#else
#define RE1_HOT_FN
#endif
#endif

uint64_t re1t_time_us(void);   // for profiling
// The profiler's clock (re1_prof.h): cheap to read, fine-grained, only ever
// differenced. The board counts the game core's CPU cycles (wraps every ~17 s,
// fine for differences); re1t_prof_ticks_per_us() of them make a microsecond.
uint32_t re1t_prof_ticks(void);
uint32_t re1t_prof_ticks_per_us(void);
void     re1t_sleep_ms(uint32_t ms);

// ---------------------------------------------------------------------------
// Display: hand one finished frame (RGB565, native endianness, row-major,
// RE1_FB_W x RE1_FB_H) to the target. The target may keep the pointer until
// the next call; the renderer double-buffers so it never draws into a frame
// that is being presented.
// ---------------------------------------------------------------------------
void re1t_present(const uint16_t* fb, int w, int h);

// ---------------------------------------------------------------------------
// Input
//
// re1t_pad_mask: the physical controls of the target as a Marni pad mask
// (bits 0-3 = up/down/left/right, MARNI_XI_BTN_* for buttons - see
// decomp/src/marni/MarniXInput.h). 0 when the target has no pad.
// Keyboard keys (browser bridge) arrive through plat_key_event() instead.
// ---------------------------------------------------------------------------
uint32_t re1t_pad_mask(void);
// Called once the renderer has its framebuffers and depth buffer: the target
// may now start what competes with them for internal RAM (the board's
// Bluetooth controller), so the renderer is served first.
void     re1t_renderer_ready(void);
// Wiring check of the sound output: plays a test tone of `ms` and returns the
// loudest sample the game's mixer produced since the last call (0 = silent;
// -1 = the target has no such check).
int      re1t_audio_test_tone(int ms);
// A port-only pad bit (above the game's buttons, bits 8-19): opens the port
// menu (PortMenu.cpp). The browser's Menu button sends it.
#define RE1_PAD_PORT_MENU 0x01000000u
int      re1t_pad_present(void);

// ---------------------------------------------------------------------------
// Audio
//
// The target owns the output device and a thread that calls `pull` whenever
// it needs `frames` stereo S16 frames at `rate` Hz. `pull` must be cheap and
// never block on I/O - streamed sources are refilled by re1t_thread_start'd
// pump (see port/common/Mixer.cpp).
// ---------------------------------------------------------------------------
typedef void (*re1t_audio_pull_fn)(int16_t* stereo, int frames);
int  re1t_audio_open(int rate, re1t_audio_pull_fn pull);   // 1 = running
void re1t_audio_lock(void);
void re1t_audio_unlock(void);

// ---------------------------------------------------------------------------
// Threads (used for the audio stream pump). `stack` is in bytes; prio 0..9
// relative (the target maps it), core -1 = any.
// ---------------------------------------------------------------------------
void re1t_thread_start(const char* name, void (*fn)(void*), void* arg,
                       int stack, int prio, int core);

// ---------------------------------------------------------------------------
// Filesystem roots and path policy
//
// re1t_data_root: absolute directory that holds config.ini, USA/, SAVE/ -
//   "/sd/re1" on the board, the --data directory on the host.
// re1t_fs_case_mode:
//   0 = the filesystem is case-insensitive (FAT): only fix separators
//   1 = lower-case every component below the data root (QEMU semihosting
//       over a tree written by tools/re1tool.py, which lower-cases names)
//   2 = resolve every existing component case-insensitively (host, so an
//       original mixed-case install can be used as-is)
// ---------------------------------------------------------------------------
const char* re1t_data_root(void);
int         re1t_fs_case_mode(void);

// Bytes readable from p to the end of the RAM region holding it (0 if p is
// not readable memory). Backs plat_readable_bytes.
size_t re1t_readable_bytes(const void* p);

// ---------------------------------------------------------------------------
// Memory: large allocations prefer external RAM (PSRAM) on the board.
// ---------------------------------------------------------------------------
void* re1t_alloc_large(size_t size);
void  re1t_free_large(void* p);
// Small, hot allocations (framebuffer, depth buffer, span tables) prefer
// internal SRAM; falls back to re1t_alloc_large.
void* re1t_alloc_fast(size_t size);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
void re1t_log(const char* s);
void re1t_fatal(const char* s) __attribute__((noreturn));
// Remaining free heap (bytes) for the HUD / logs; -1 when unknown.
long re1t_heap_free(void);
// Largest single allocation that would currently succeed; -1 when unknown.
long re1t_heap_largest(void);
// Free internal SRAM (the drivers' DMA buffers come from it); -1 when the
// target has no such split.
long re1t_heap_internal_free(void);

// ---------------------------------------------------------------------------
// Remote display link (browser bridge)
//
// When the target has a byte link to tools/re1tool.py's bridge (UART /
// USB-CDC on the board and in QEMU, TCP on the host) it implements these.
// re1t_remote_write must accept the whole buffer (blocking is fine);
// re1t_remote_read returns 0..max bytes without blocking.
// ---------------------------------------------------------------------------
int    re1t_remote_enabled(void);
void   re1t_remote_write(const void* data, size_t size);
size_t re1t_remote_read(void* data, size_t max);

// ---------------------------------------------------------------------------
// Task switching for the game's cooperative scheduler
// (decomp/src/game/TaskScheduler.cpp, RE1_PORT_TASKSWITCH_EXTERNAL).
// Implemented by the board (FreeRTOS); the host uses upstream's ucontext.
// ---------------------------------------------------------------------------
void re1port_task_start(int id, void (*entry)(void));
void re1port_task_resume(int id);
void re1port_task_yield(int id);

#ifdef __cplusplus
}
#endif
