// target.cpp - re1_target.h on ESP-IDF (board and QEMU).
#include "board.h"
#include "re1_target.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>

static const char* TAG = "re1";

// ---- time -----------------------------------------------------------------
extern "C" uint32_t re1t_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

extern "C" uint64_t re1t_time_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

// The game runs on one core (CONFIG_RE1_GAME_CORE), so its cycle counter is
// a consistent clock for the profiler - and a register read, where
// esp_timer_get_time() costs about a microsecond a call.
extern "C" uint32_t re1t_prof_ticks(void) { return (uint32_t)esp_cpu_get_cycle_count(); }
extern "C" uint32_t re1t_prof_ticks_per_us(void)
{
    return (uint32_t)(esp_clk_cpu_freq() / 1000000);
}

extern "C" void re1t_sleep_ms(uint32_t ms)
{
    vTaskDelay(ms ? pdMS_TO_TICKS(ms) : 1);
}

// ---- display ----------------------------------------------------------------
extern "C" void re1t_present(const uint16_t* fb, int w, int h)
{
    (void)w;
    (void)h;
#if CONFIG_RE1_TARGET_BOARD
    board_display_present(fb);
#else
    (void)fb;   // QEMU: the picture goes over the browser link only
#endif
}

// ---- input ------------------------------------------------------------------
extern "C" uint32_t re1t_pad_mask(void) { return board_input_mask() | board_gamepad_mask(); }
// Bluetooth starts only now: its controller and stacks take ~45 KB of internal
// RAM, and started first they left the depth buffer to PSRAM (~2 ms a frame).
extern "C" void re1t_renderer_ready(void) { board_gamepad_init(); }
extern "C" int re1t_audio_test_tone(int ms) { return board_audio_tone(ms); }

extern "C" int re1t_pad_present(void)
{
    return board_input_present() || board_gamepad_connected();
}

// ---- audio ------------------------------------------------------------------
static SemaphoreHandle_t s_audioMutex = nullptr;

extern "C" void re1t_audio_lock(void)
{
    if (s_audioMutex) xSemaphoreTakeRecursive(s_audioMutex, portMAX_DELAY);
}

extern "C" void re1t_audio_unlock(void)
{
    if (s_audioMutex) xSemaphoreGiveRecursive(s_audioMutex);
}

extern "C" int re1t_audio_open(int rate, re1t_audio_pull_fn pull)
{
    return board_audio_init(rate, pull);
}

// ---- threads ----------------------------------------------------------------
extern "C" void re1t_thread_start(const char* name, void (*fn)(void*), void* arg, int stack,
                                  int prio, int core)
{
    // prio 0..9 -> FreeRTOS 2..11, below the audio (8) / LCD (6) drivers'
    // tasks when low, and always below the game task's own priority (5) for
    // the stream pump/remote sender (prio <= 4).
    UBaseType_t p = (UBaseType_t)(prio < 0 ? 1 : prio + 1);
    xTaskCreatePinnedToCore(fn, name, stack, arg, p, nullptr, core < 0 ? tskNO_AFFINITY : core);
}

// ---- filesystem / memory ----------------------------------------------------
extern "C" const char* re1t_data_root(void) { return BOARD_DATA_ROOT; }

extern "C" int re1t_fs_case_mode(void)
{
#if CONFIG_RE1_TARGET_QEMU
    return 1;   // host directory (case-sensitive), tree written lower-case
#else
    return 0;   // FAT: case-insensitive already
#endif
}

extern "C" size_t re1t_readable_bytes(const void* p)
{
    // Only used to clamp a truncated TIM page read: any mapped RAM is
    // readable to its end; report up to 1 MB.
    if (esp_ptr_external_ram(p) || esp_ptr_internal(p)) return 1024 * 1024;
    return 0;
}

extern "C" void* re1t_alloc_large(size_t size)
{
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

extern "C" void re1t_free_large(void* p) { heap_caps_free(p); }

// Internal RAM that re1t_alloc_fast leaves alone: the pool IDF keeps for DMA
// and internal-only allocations (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL). The
// heap lets any internal allocation dip into that pool once the rest is gone;
// on 2026-11-07 the model scratch grew into it and every SD read failed with
// ESP_ERR_NO_MEM (the SD card now has its own bounce buffer, storage.cpp).
// Kept to the pool and no more: the framebuffers and depth buffer, allocated
// first, must still fit - with the depth buffer in PSRAM room 112 drew in
// 34 ms instead of 32. Past this line a fast allocation falls back to PSRAM.
static const size_t kInternalKeepFree = CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL;

extern "C" void* re1t_alloc_fast(size_t size)
{
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= size + kInternalKeepFree) {
        void* p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p != nullptr) return p;
    }
    return re1t_alloc_large(size);
}

// ---- diagnostics ------------------------------------------------------------
extern "C" void re1t_log(const char* s) { fputs(s, stderr); }

extern "C" void re1t_fatal(const char* s)
{
    ESP_LOGE(TAG, "FATAL: %s (PSRAM free %u, internal free %u)", s,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    vTaskDelay(pdMS_TO_TICKS(500));
    abort();
}

extern "C" long re1t_heap_largest(void)
{
    return (long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

extern "C" long re1t_heap_free(void)
{
    return (long)heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

extern "C" long re1t_heap_internal_free(void)
{
    return (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

// ---- remote link ------------------------------------------------------------
extern "C" int re1t_remote_enabled(void) { return board_remote_enabled(); }
extern "C" void re1t_remote_write(const void* d, size_t n) { board_remote_write(d, n); }
extern "C" size_t re1t_remote_read(void* d, size_t m) { return board_remote_read(d, m); }

// ---- memory speed -----------------------------------------------------------
// One look at the memory a frame lives in, logged at boot: sequential copy /
// fill / read rates of PSRAM through the data cache (over 256 KB, four times
// the cache, so it is the PSRAM that is measured) and of internal RAM, and
// the cost of a PSRAM access that misses the cache (one per 64-byte line).
//
// Tried with it (2026-11-07): the data cache's autoload (prefetch of the
// next line after a miss, which IDF leaves off) lifts sequential PSRAM reads
// from 75 to 82 MB/s but doubles a scattered miss, 708 -> 1420 ns, and a
// frame is mostly scattered reads (texels, the depth-sorted triangle pool).
// Off it stays.
static void MeasureMemory(const char* what)
{
    const size_t kExt = 128 * 1024, kIn = 16 * 1024;
    uint8_t* ext = (uint8_t*)heap_caps_malloc(2 * kExt, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t* in = (uint8_t*)heap_caps_malloc(2 * kIn, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ext == nullptr || in == nullptr) {
        heap_caps_free(ext);
        heap_caps_free(in);
        return;
    }
    auto mbps = [](size_t bytes, int64_t us) { return us > 0 ? (unsigned)(bytes / us) : 0u; };
    volatile uint32_t sink = 0;

    memset(ext, 1, 2 * kExt);
    int64_t t = esp_timer_get_time();
    for (int r = 0; r < 4; ++r) memcpy(ext + kExt, ext, kExt);
    const unsigned extCopy = mbps(4 * kExt, esp_timer_get_time() - t);
    t = esp_timer_get_time();
    for (int r = 0; r < 4; ++r) memset(ext, r, 2 * kExt);
    const unsigned extFill = mbps(8 * kExt, esp_timer_get_time() - t);
    t = esp_timer_get_time();
    uint32_t sum = 0;
    for (int r = 0; r < 4; ++r) {
        const uint32_t* w = (const uint32_t*)ext;
        for (size_t i = 0; i < 2 * kExt / 4; ++i) sum += w[i];
    }
    sink = sum;
    const unsigned extRead = mbps(8 * kExt, esp_timer_get_time() - t);
    // One read per cache line, lines visited in a scattered order.
    const size_t lines = 2 * kExt / 64;
    t = esp_timer_get_time();
    sum = 0;
    for (int r = 0; r < 4; ++r)
        for (size_t i = 0; i < lines; ++i) sum += ext[((i * 1543u) % lines) * 64];
    sink = sink + sum;
    const unsigned missNs = (unsigned)((esp_timer_get_time() - t) * 1000 / (int64_t)(4 * lines));

    memset(in, 1, 2 * kIn);
    t = esp_timer_get_time();
    for (int r = 0; r < 64; ++r) memcpy(in + kIn, in, kIn);
    const unsigned inCopy = mbps(64 * kIn, esp_timer_get_time() - t);
    (void)sink;
    heap_caps_free(ext);
    heap_caps_free(in);
    ESP_LOGI(TAG, "memory (%s): PSRAM copy %u MB/s, fill %u MB/s, read %u MB/s, cache miss "
                  "%u ns; internal copy %u MB/s", what, extCopy, extFill, extRead, missNs, inCopy);
}

// ---- bring-up ---------------------------------------------------------------
extern "C" void board_init(void)
{
    s_audioMutex = xSemaphoreCreateRecursiveMutex();
    board_remote_init();
    ESP_LOGI(TAG, "r-esp32-evil (%s) PSRAM free %u KB, internal free %u KB",
#if CONFIG_RE1_TARGET_QEMU
             "QEMU",
#else
             "board",
#endif
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
#if CONFIG_RE1_TARGET_BOARD
    MeasureMemory("data cache");
    board_display_init();
    board_input_init();
#endif
}
