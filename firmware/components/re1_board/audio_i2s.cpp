// audio_i2s.cpp - PCM5102A over I2S (standard Philips, 16-bit stereo).
//
// PCM5102A wiring: SCK -> GND (the DAC derives its clock from BCK), FMT -> GND
// (I2S), XSMT -> 3V3 (un-mute), FLT/DEMP -> GND, VIN 3V3/5V per module.
// A task on core 0 pulls the port mixer (port/common/Audio.cpp) in 256-frame
// blocks and writes them to the DMA ring.
#include "board.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <math.h>

static const char* TAG = "audio";

// Wiring check (re1tool.py cmd tone): a 440 Hz tone replaces the game's mix
// for a while, and the loudest sample the game mixed is kept, so one command
// tells whether the game makes sound and whether the DAC plays it.
static std::atomic<int> s_toneFrames{0};
static std::atomic<int> s_mixPeak{0};

#if CONFIG_RE1_TARGET_BOARD
static i2s_chan_handle_t s_tx = nullptr;
static void (*s_pull)(int16_t*, int) = nullptr;
static int s_rate = 22050;

static void AudioTask(void*)
{
    const int frames = 256;
    static int16_t buf[256 * 2];
    const int vol = CONFIG_RE1_AUDIO_VOLUME * 256 / 100;
    float phase = 0.0f;
    for (;;) {
        s_pull(buf, frames);
        int peak = 0;
        for (int i = 0; i < frames * 2; ++i) {
            const int a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        if (peak > s_mixPeak.load(std::memory_order_relaxed)) s_mixPeak.store(peak);
        if (s_toneFrames.load(std::memory_order_relaxed) > 0) {
            const float step = 2.0f * 3.14159265f * 440.0f / (float)s_rate;
            for (int i = 0; i < frames; ++i) {
                const int16_t v = (int16_t)(sinf(phase) * 12000.0f);
                buf[2 * i] = buf[2 * i + 1] = v;
                phase += step;
                if (phase > 6.2831853f) phase -= 6.2831853f;
            }
            s_toneFrames.fetch_sub(frames);
        }
        if (vol != 256) {
            for (int i = 0; i < frames * 2; ++i) buf[i] = (int16_t)((buf[i] * vol) >> 8);
        }
        size_t written = 0;
        i2s_channel_write(s_tx, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}
#endif

#if CONFIG_RE1_TARGET_QEMU
// QEMU has no I2S. The mixer must still run in real time: the game polls each
// sound's "still playing" status (cutscenes wait for a voice line to end, the
// movie clock is the audio position), so a mixer that never advances stalls
// them. This task pulls the mix at the output rate; the browser link
// (RE1_REMOTE_AUDIO) forwards it to the page, which plays it.
static void (*s_qpull)(int16_t*, int) = nullptr;
static int s_qrate = 22050;

static void ClockTask(void*)
{
    const int chunk = 256;
    static int16_t buf[256 * 2];
    const int64_t start = esp_timer_get_time();
    uint64_t produced = 0;
    for (;;) {
        const uint64_t due = (uint64_t)(esp_timer_get_time() - start) * (uint64_t)s_qrate / 1000000u;
        while (produced + chunk <= due) {
            s_qpull(buf, chunk);
            produced += chunk;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif

int board_audio_init(int rate, void (*pull)(int16_t*, int))
{
#if CONFIG_RE1_TARGET_QEMU
    s_qpull = pull;
    s_qrate = rate;
    xTaskCreatePinnedToCore(ClockTask, "re1_aclock", 4096, nullptr, 8, nullptr, 0);
    ESP_LOGI(TAG, "QEMU: mixer clocked in software, %d Hz, sound goes to the browser", rate);
    return 1;
#else
    s_pull = pull;
    s_rate = rate;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 256;
    chan.auto_clear = true;
    if (i2s_new_channel(&chan, &s_tx, nullptr) != ESP_OK) return 0;

    i2s_std_config_t std = {};
    std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate);
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std.gpio_cfg.bclk = (gpio_num_t)CONFIG_RE1_I2S_BCK;
    std.gpio_cfg.ws = (gpio_num_t)CONFIG_RE1_I2S_WS;
    std.gpio_cfg.dout = (gpio_num_t)CONFIG_RE1_I2S_DOUT;
    std.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) return 0;
    if (i2s_channel_enable(s_tx) != ESP_OK) return 0;

    xTaskCreatePinnedToCore(AudioTask, "re1_audio", 4096, nullptr, 8, nullptr, 0);
    ESP_LOGI(TAG, "I2S up: %d Hz stereo, BCK=%d WS=%d DOUT=%d", rate, CONFIG_RE1_I2S_BCK,
             CONFIG_RE1_I2S_WS, CONFIG_RE1_I2S_DOUT);
    return 1;
#endif
}

// Starts a test tone of `ms` on the DAC and returns the loudest sample the
// game's mixer produced since the last call (0..32767; 0 = it was silent).
int board_audio_tone(int ms)
{
#if CONFIG_RE1_TARGET_BOARD
    s_toneFrames.store(s_rate * ms / 1000);
    ESP_LOGI(TAG, "test tone: %d ms of 440 Hz on BCK=%d WS=%d DOUT=%d", ms, CONFIG_RE1_I2S_BCK,
             CONFIG_RE1_I2S_WS, CONFIG_RE1_I2S_DOUT);
#else
    (void)ms;
#endif
    return s_mixPeak.exchange(0);
}
