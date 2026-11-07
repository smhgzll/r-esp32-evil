// input.cpp - analog joystick(s) + push buttons -> Marni pad mask.
//
// Joystick modules (KY-023 style): VRx/VRy to ADC1 pins (GPIO 1-10), SW to a
// GPIO, +5V pin to 3V3 (NOT 5 V: the ADC tops out at ~3.1 V). Buttons are
// wired between the GPIO and GND; the internal pull-ups are enabled.
//
// The resting position of each axis is sampled at boot, so keep the sticks
// centred while the board starts.
//
// Mask bits (decomp/src/marni/MarniXInput.h, same table as an XInput pad):
//   1/2/4/8 = up/down/left/right
//   A = action, B = run/cancel, RB = aim, Y = inventory, BACK = options
#include "board.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#if CONFIG_RE1_TARGET_BOARD

static const char* TAG = "input";

#define BTN_A     0x00000100u
#define BTN_B     0x00000200u
#define BTN_Y     0x00000800u
#define BTN_RB    0x00002000u
#define BTN_BACK  0x00004000u
#define BTN_START 0x00008000u

namespace {

adc_oneshot_unit_handle_t s_adc = nullptr;

struct Axis {
    int gpio = -1;
    adc_channel_t ch = ADC_CHANNEL_0;
    int center = 2048;
    bool invert = false;
    bool ok = false;
};

Axis s_axes[4];   // stick1 x, y, stick2 x, y

struct Button { int gpio; uint32_t bit; };
Button s_buttons[] = {
    {CONFIG_RE1_BTN_ACTION, BTN_A},
    {CONFIG_RE1_BTN_CANCEL, BTN_B},
    {CONFIG_RE1_BTN_AIM, BTN_RB},
    {CONFIG_RE1_BTN_INVENTORY, BTN_Y},
    {CONFIG_RE1_BTN_OPTIONS, BTN_BACK},
    {CONFIG_RE1_STICK1_SW, BTN_Y},
#if CONFIG_RE1_STICK2_ENABLE
    {CONFIG_RE1_STICK2_SW, BTN_BACK},
#endif
    {CONFIG_RE1_BTN_UP, 1},
    {CONFIG_RE1_BTN_DOWN, 2},
    {CONFIG_RE1_BTN_LEFT, 4},
    {CONFIG_RE1_BTN_RIGHT, 8},
};

int ReadRaw(const Axis& a)
{
    int v = 0;
    if (adc_oneshot_read(s_adc, a.ch, &v) != ESP_OK) return a.center;
    return v;
}

void SetupAxis(Axis& a, int gpio, bool invert)
{
    a.gpio = gpio;
    a.invert = invert;
    if (gpio < 0) return;
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(gpio, &unit, &a.ch) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin (use GPIO 1-10)", gpio);
        return;
    }
    adc_oneshot_chan_cfg_t cfg = {};
    cfg.atten = ADC_ATTEN_DB_12;
    cfg.bitwidth = ADC_BITWIDTH_12;
    adc_oneshot_config_channel(s_adc, a.ch, &cfg);
    int sum = 0;
    for (int i = 0; i < 16; ++i) {
        int v = 0;
        adc_oneshot_read(s_adc, a.ch, &v);
        sum += v;
    }
    a.center = sum / 16;
    a.ok = true;
    ESP_LOGI(TAG, "axis GPIO%d centre %d", gpio, a.center);
}

// -1 / 0 / +1 with the dead zone applied.
int Direction(const Axis& a)
{
    if (!a.ok) return 0;
    int v = ReadRaw(a) - a.center;
    int span = v > 0 ? 4095 - a.center : a.center;
    if (span <= 0) return 0;
    int pct = v * 100 / span;
    if (a.invert) pct = -pct;
    if (pct > CONFIG_RE1_STICK_DEADZONE) return 1;
    if (pct < -CONFIG_RE1_STICK_DEADZONE) return -1;
    return 0;
}

}  // namespace

void board_input_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = {};
    unit.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &s_adc));

#if CONFIG_RE1_STICK1_INVERT_X
    const bool ix = true;
#else
    const bool ix = false;
#endif
#if CONFIG_RE1_STICK1_INVERT_Y
    const bool iy = true;
#else
    const bool iy = false;
#endif
    SetupAxis(s_axes[0], CONFIG_RE1_STICK1_X, ix);
    SetupAxis(s_axes[1], CONFIG_RE1_STICK1_Y, iy);
#if CONFIG_RE1_STICK2_ENABLE
    SetupAxis(s_axes[2], CONFIG_RE1_STICK2_X, false);
    SetupAxis(s_axes[3], CONFIG_RE1_STICK2_Y, false);
#endif

    for (const Button& b : s_buttons) {
        if (b.gpio < 0) continue;
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << b.gpio;
        c.mode = GPIO_MODE_INPUT;
        c.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&c);
    }
}

uint32_t board_input_mask(void)
{
    uint32_t m = 0;
    // Stick 1: screen-up is the ADC's low end on most modules (Y decreases
    // when pushed away from the pins); flip with RE1_STICK1_INVERT_Y.
    int x = Direction(s_axes[0]);
    int y = Direction(s_axes[1]);
    if (y < 0) m |= 1;
    if (y > 0) m |= 2;
    if (x < 0) m |= 4;
    if (x > 0) m |= 8;
#if CONFIG_RE1_STICK2_ENABLE
    int x2 = Direction(s_axes[2]);
    int y2 = Direction(s_axes[3]);
#if CONFIG_RE1_STICK2_FACE
    if (y2 < 0) m |= BTN_RB;
    if (y2 > 0) m |= BTN_B;
    if (x2 < 0) m |= BTN_Y;
    if (x2 > 0) m |= BTN_A;
#else
    if (y2 < 0) m |= 1;
    if (y2 > 0) m |= 2;
    if (x2 < 0) m |= 4;
    if (x2 > 0) m |= 8;
#endif
#endif
    for (const Button& b : s_buttons) {
        if (b.gpio >= 0 && gpio_get_level((gpio_num_t)b.gpio) == 0) m |= b.bit;
    }
    return m;
}

int board_input_present(void)
{
    return 1;
}

#else  // QEMU: no controls on the machine, the browser bridge drives input

void board_input_init(void) {}
uint32_t board_input_mask(void) { return 0; }
int board_input_present(void) { return 1; }

#endif
