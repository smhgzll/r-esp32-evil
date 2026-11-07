// display_st7735.cpp - 1.8" 128x160 ST7735(S) panel over SPI, landscape.
//
// The renderer produces RE1_FB_W x RE1_FB_H (160x120) RGB565 frames; the
// panel is 160x128 in landscape, so the picture is centred with 4-line black
// bars. A display task on core 0 converts each frame to the panel's
// big-endian byte order in small DMA chunks and streams it with
// esp_lcd_panel_io (which drives A0/DC per transaction), while the game draws
// the next frame on core 1.
#include "board.h"
#include "re1_target.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

#if CONFIG_RE1_TARGET_BOARD

static const char* TAG = "st7735";

#define PANEL_W 160
#define PANEL_H 128
#define CHUNK_LINES 16

static esp_lcd_panel_io_handle_t s_io = nullptr;
static SemaphoreHandle_t s_idle = nullptr;       // display finished the last frame
static SemaphoreHandle_t s_ready = nullptr;      // a new frame is waiting
static const uint16_t* s_frame = nullptr;
static uint16_t* s_dma[2] = {nullptr, nullptr};

static void Cmd(uint8_t cmd, const uint8_t* data = nullptr, size_t len = 0)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, data, len);
}

static void SetWindow(int x0, int y0, int x1, int y1)
{
    const int xo = CONFIG_RE1_LCD_X_OFFSET, yo = CONFIG_RE1_LCD_Y_OFFSET;
    uint8_t ca[4] = {(uint8_t)((x0 + xo) >> 8), (uint8_t)(x0 + xo), (uint8_t)((x1 + xo) >> 8),
                     (uint8_t)(x1 + xo)};
    uint8_t ra[4] = {(uint8_t)((y0 + yo) >> 8), (uint8_t)(y0 + yo), (uint8_t)((y1 + yo) >> 8),
                     (uint8_t)(y1 + yo)};
    Cmd(0x2A, ca, 4);   // CASET
    Cmd(0x2B, ra, 4);   // RASET
}

static void InitPanel()
{
    // ST7735S power-up sequence (datasheet defaults + the common "R" tuning).
    Cmd(0x01);                                  // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));
    Cmd(0x11);                                  // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(255));
    { const uint8_t d[] = {0x01, 0x2C, 0x2D};             Cmd(0xB1, d, 3); }  // FRMCTR1
    { const uint8_t d[] = {0x01, 0x2C, 0x2D};             Cmd(0xB2, d, 3); }  // FRMCTR2
    { const uint8_t d[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}; Cmd(0xB3, d, 6); }
    { const uint8_t d[] = {0x07};                         Cmd(0xB4, d, 1); }  // INVCTR
    { const uint8_t d[] = {0xA2, 0x02, 0x84};             Cmd(0xC0, d, 3); }  // PWCTR1
    { const uint8_t d[] = {0xC5};                         Cmd(0xC1, d, 1); }
    { const uint8_t d[] = {0x0A, 0x00};                   Cmd(0xC2, d, 2); }
    { const uint8_t d[] = {0x8A, 0x2A};                   Cmd(0xC3, d, 2); }
    { const uint8_t d[] = {0x8A, 0xEE};                   Cmd(0xC4, d, 2); }
    { const uint8_t d[] = {0x0E};                         Cmd(0xC5, d, 1); }  // VMCTR1
#if CONFIG_RE1_LCD_INVERT
    Cmd(0x21);                                  // INVON
#else
    Cmd(0x20);                                  // INVOFF
#endif
    {
        uint8_t mad = (uint8_t)CONFIG_RE1_LCD_MADCTL;
#if CONFIG_RE1_LCD_BGR
        mad |= 0x08;
#endif
        Cmd(0x36, &mad, 1);                     // MADCTL
    }
    { const uint8_t d[] = {0x05};                         Cmd(0x3A, d, 1); }  // COLMOD 16 bpp
    { const uint8_t d[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                           0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
      Cmd(0xE0, d, 16); }                                                       // GMCTRP1
    { const uint8_t d[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                           0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};
      Cmd(0xE1, d, 16); }                                                       // GMCTRN1
    Cmd(0x13);                                  // NORON
    vTaskDelay(pdMS_TO_TICKS(10));
    Cmd(0x29);                                  // DISPON
    vTaskDelay(pdMS_TO_TICKS(100));
}

// One chunk = its own window + RAMWR (the ST7735 has no "write continue").
// tx_param blocks until the previous colour DMA has drained, so converting
// chunk k+1 overlaps the DMA of chunk k, and two chunk buffers suffice as long
// as they keep alternating across frames (s_which is never reset).
static int s_which = 0;

static void SendRows(int x0, int y0, int w, int rows, const uint16_t* src, int srcStride)
{
    uint16_t* out = s_dma[s_which];
    for (int r = 0; r < rows; ++r) {
        const uint16_t* in = src ? src + (size_t)r * srcStride : nullptr;
        uint16_t* o = out + (size_t)r * w;
        if (in) for (int i = 0; i < w; ++i) o[i] = __builtin_bswap16(in[i]);
        else memset(o, 0, (size_t)w * 2);
    }
    SetWindow(x0, y0, x0 + w - 1, y0 + rows - 1);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, out, (size_t)w * rows * 2);   // RAMWR
    s_which ^= 1;
}

static void ClearPanel()
{
    for (int y = 0; y < PANEL_H; y += CHUNK_LINES) {
        int n = (PANEL_H - y) < CHUNK_LINES ? (PANEL_H - y) : CHUNK_LINES;
        SendRows(0, y, PANEL_W, n, nullptr, 0);
    }
}

static void DisplayTask(void*)
{
    const int top = (PANEL_H - RE1_FB_H) / 2;
    const int left = (PANEL_W - RE1_FB_W) / 2;
    for (;;) {
        xSemaphoreTake(s_ready, portMAX_DELAY);
        const uint16_t* fb = s_frame;
        for (int y = 0; y < RE1_FB_H; y += CHUNK_LINES) {
            int n = (RE1_FB_H - y) < CHUNK_LINES ? (RE1_FB_H - y) : CHUNK_LINES;
            SendRows(left, top + y, RE1_FB_W, n, fb + (size_t)y * RE1_FB_W, RE1_FB_W);
        }
        // The frame buffer is only read by the conversion above (the DMA reads
        // the chunk buffers), so the renderer may reuse it already.
        xSemaphoreGive(s_idle);
    }
}

void board_display_init(void)
{
    // Backlight: full on (PWM left available for a brightness option).
    if (CONFIG_RE1_LCD_BL >= 0) {
        gpio_config_t bl = {};
        bl.pin_bit_mask = 1ULL << CONFIG_RE1_LCD_BL;
        bl.mode = GPIO_MODE_OUTPUT;
        gpio_config(&bl);
        gpio_set_level((gpio_num_t)CONFIG_RE1_LCD_BL, 1);
    }
    if (CONFIG_RE1_LCD_RST >= 0) {
        gpio_config_t rst = {};
        rst.pin_bit_mask = 1ULL << CONFIG_RE1_LCD_RST;
        rst.mode = GPIO_MODE_OUTPUT;
        gpio_config(&rst);
        gpio_set_level((gpio_num_t)CONFIG_RE1_LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level((gpio_num_t)CONFIG_RE1_LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    spi_bus_config_t bus = {};
    bus.sclk_io_num = CONFIG_RE1_LCD_SCK;
    bus.mosi_io_num = CONFIG_RE1_LCD_MOSI;
    bus.miso_io_num = -1;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = PANEL_W * CHUNK_LINES * 2 + 64;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io = {};
    io.dc_gpio_num = CONFIG_RE1_LCD_DC;
    io.cs_gpio_num = CONFIG_RE1_LCD_CS;
    io.pclk_hz = (uint32_t)CONFIG_RE1_LCD_SPI_MHZ * 1000 * 1000;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    io.spi_mode = 0;
    io.trans_queue_depth = 4;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &s_io));

    for (int i = 0; i < 2; ++i) {
        s_dma[i] = (uint16_t*)heap_caps_malloc(PANEL_W * CHUNK_LINES * 2,
                                               MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(s_dma[i] != nullptr);
    }

    InitPanel();
    ClearPanel();

    s_idle = xSemaphoreCreateBinary();
    s_ready = xSemaphoreCreateBinary();
    xSemaphoreGive(s_idle);
    xTaskCreatePinnedToCore(DisplayTask, "re1_lcd", 3072, nullptr, 6, nullptr, 0);
    ESP_LOGI(TAG, "panel up: %dx%d @ %d MHz, picture %dx%d", PANEL_W, PANEL_H,
             CONFIG_RE1_LCD_SPI_MHZ, RE1_FB_W, RE1_FB_H);
}

void board_display_present(const uint16_t* fb)
{
    // Wait until the panel has taken the previous frame: the renderer is about
    // to reuse that buffer (see MarniDX::Present's double buffering).
    xSemaphoreTake(s_idle, portMAX_DELAY);
    s_frame = fb;
    xSemaphoreGive(s_ready);
}

#endif  // CONFIG_RE1_TARGET_BOARD
