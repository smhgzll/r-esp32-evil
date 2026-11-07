// gamepad_ble.cpp - a Bluetooth LE game controller as the board's pad.
//
// An Xbox Wireless Controller (firmware 5.x speaks Bluetooth LE; the ESP32-S3
// has no classic Bluetooth) or another BLE HID gamepad connects through
// NimBLE and ESP-IDF's HID host (esp_hid). Its input report becomes a Marni
// pad mask that target.cpp ORs into the board's own controls, mapped like the
// browser's standard-gamepad mapping (tools/web/index.html); the Xbox button
// opens the port menu.
//
// Pairing: hold the controller's pair button (top, next to the USB port)
// until the Xbox logo flashes fast. While no controller is connected the
// board scans; it connects to the first gamepad it hears, bonds (the bond is
// kept in NVS) and from then on takes that controller back whenever it is
// switched on. The console says what happens ([pad] lines).
#include "board.h"
#include "re1_target.h"
#include "sdkconfig.h"

#include <atomic>

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

#include "esp_hidh.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include <string.h>

extern "C" void ble_store_config_init(void);

static const char* TAG = "pad";

static std::atomic<uint32_t> s_mask{0};
static std::atomic<int>      s_connected{0};
static TaskHandle_t          s_task = nullptr;
static volatile bool         s_found = false;
static ble_addr_t            s_foundAddr;
static char                  s_foundName[40];

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------
static int U16(const uint8_t* d, int i) { return d[i] | d[i + 1] << 8; }

// Xbox Wireless Controller, firmware 5.x (BLE), input report 1, 16 bytes:
//   0-7   LX LY RX RY (0..65535, centre 32768, Y grows downwards)
//   8-11  LT RT (0..1023)
//   12    hat: 0 = none, 1 = up, then clockwise to 8 = up-left
//   13    A 0x01  B 0x02  X 0x08  Y 0x10  LB 0x40  RB 0x80
//   14    View 0x04  Menu 0x08  Xbox 0x10  LS 0x20  RS 0x40
static uint32_t XboxMask(const uint8_t* d, size_t n)
{
    if (n < 15) return 0;
    uint32_t m = 0;
    const int lx = U16(d, 0) - 32768, ly = U16(d, 2) - 32768;
    if (ly < -16384) m |= 1;
    if (ly > 16384) m |= 2;
    if (lx < -16384) m |= 4;
    if (lx > 16384) m |= 8;
    static const uint8_t kHat[9] = {0, 1, 1 | 8, 8, 2 | 8, 2, 2 | 4, 4, 1 | 4};
    if (d[12] <= 8) m |= kHat[d[12]];
    const uint8_t b0 = d[13], b1 = d[14];
    if (b0 & 0x01) m |= 0x00000100u;   // A: action
    if (b0 & 0x02) m |= 0x00000200u;   // B: run / cancel
    if (b0 & 0x08) m |= 0x00000400u;   // X: aim
    if (b0 & 0x10) m |= 0x00000800u;   // Y: inventory
    if (b0 & 0x40) m |= 0x00001000u;   // LB
    if (b0 & 0x80) m |= 0x00002000u;   // RB: aim
    if (b1 & 0x04) m |= 0x00004000u;   // View: options
    if (b1 & 0x08) m |= 0x00008000u;   // Menu: inventory
    if (b1 & 0x20) m |= 0x00010000u;   // LS
    if (b1 & 0x40) m |= 0x00020000u;   // RS
    if (b1 & 0x10) m |= RE1_PAD_PORT_MENU;   // Xbox button: the port menu
    if (U16(d, 8) > 512) m |= 0x00040000u;    // LT: run
    if (U16(d, 10) > 512) m |= 0x00080000u;   // RT: aim
    return m;
}

static void OnInput(const esp_hidh_event_data_t* p)
{
    const uint8_t* d = p->input.data;
    const size_t n = p->input.length;
    static int s_unknownLogged = 0;
    uint32_t m = 0;
    if (p->input.usage == ESP_HID_USAGE_GAMEPAD || p->input.usage == ESP_HID_USAGE_JOYSTICK ||
        n == 16) {
        m = XboxMask(d, n);
    } else if (s_unknownLogged < 3) {
        ++s_unknownLogged;
        ESP_LOGW(TAG, "report %u (%s, %u bytes) not understood", p->input.report_id,
                 esp_hid_usage_str(p->input.usage), (unsigned)n);
    }
    // The first changes are logged raw: the layout above is the Xbox one;
    // another pad's shows here.
    static int s_logged = 0;
    if (m != s_mask.load() && s_logged < 12) {
        ++s_logged;
        char hex[3 * 20 + 1] = "";
        for (size_t i = 0; i < n && i < 20; ++i) sprintf(hex + 3 * i, "%02x ", d[i]);
        ESP_LOGI(TAG, "report %u: %s-> pad 0x%08lx", p->input.report_id, hex, (unsigned long)m);
    }
    s_mask.store(m);
}

static void HidhEvent(void*, esp_event_base_t, int32_t id, void* data)
{
    const esp_hidh_event_data_t* p = (const esp_hidh_event_data_t*)data;
    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        if (p->open.status == ESP_OK) {
            s_connected.store(1);
            ESP_LOGI(TAG, "connected: %s", esp_hidh_dev_name_get(p->open.dev));
        } else {
            ESP_LOGW(TAG, "connect failed");
        }
        break;
    case ESP_HIDH_BATTERY_EVENT:
        ESP_LOGI(TAG, "battery %d%%", p->battery.level);
        break;
    case ESP_HIDH_INPUT_EVENT:
        OnInput(p);
        break;
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "disconnected");
        s_mask.store(0);
        s_connected.store(0);
        if (s_task != nullptr) xTaskNotifyGive(s_task);   // back to scanning
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------
static bool IsBonded(const ble_addr_t& a)
{
    struct ble_store_key_sec key = {};
    struct ble_store_value_sec val;
    key.peer_addr = a;
    return ble_store_read_peer_sec(&key, &val) == 0;
}

static int GapEvent(struct ble_gap_event* ev, void*)
{
    if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (s_task != nullptr) xTaskNotifyGive(s_task);
        return 0;
    }
    if (ev->type != BLE_GAP_EVENT_DISC || s_found) return 0;
    const struct ble_gap_disc_desc& d = ev->disc;
    // A game controller only: a HID service alone is any keyboard or mouse
    // nearby. The Xbox one says "gamepad" in its advertisement (appearance)
    // and its name in the scan response.
    bool want = false;
    int appearance = -1;
    char name[sizeof(s_foundName)] = "";
    if (d.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {
        want = true;                  // a bonded controller calling this board
    } else {
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, d.data, d.length_data) == 0) {
            if (f.name != nullptr) {
                const size_t len = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
                memcpy(name, f.name, len);
                name[len] = '\0';
            }
            if (f.appearance_is_present) {
                appearance = f.appearance;
                if (appearance == 0x03C4 || appearance == 0x03C3) want = true;  // gamepad, joystick
            }
            if (strstr(name, "Xbox") || strstr(name, "Controller") || strstr(name, "controller") ||
                strstr(name, "Gamepad") || strstr(name, "gamepad"))
                want = true;
        }
        if (!want && IsBonded(d.addr)) want = true;
    }
    if (want) {
        s_foundAddr = d.addr;
        snprintf(s_foundName, sizeof(s_foundName), "%s", name[0] ? name : "(no name yet)");
        ESP_LOGI(TAG, "seen %s, appearance 0x%04x, address %02x:%02x:%02x:%02x:%02x:%02x%s",
                 s_foundName, appearance & 0xFFFF, d.addr.val[5], d.addr.val[4], d.addr.val[3],
                 d.addr.val[2], d.addr.val[1], d.addr.val[0],
                 d.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND ? " (calling us)" : "");
        s_found = true;
        if (s_task != nullptr) xTaskNotifyGive(s_task);
    }
    return 0;
}

static void HostTask(void*)
{
    nimble_port_run();              // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

// NVS, the controller and the host come up on the caller's stack, which is in
// internal RAM: they may write flash (the radio's calibration, the bond
// store), and a task whose stack is in PSRAM must not.
static bool StartBluetooth(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "NVS: %s - pairing will not be remembered", esp_err_to_name(err));

    if ((err = nimble_port_init()) != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth did not start: %s", esp_err_to_name(err));
        return false;
    }
    // Just Works pairing with bonding: a controller has no keyboard or screen.
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    esp_hidh_config_t cfg = {};
    cfg.callback = HidhEvent;
    cfg.event_stack_size = 4096;
    cfg.callback_arg = nullptr;
    if ((err = esp_hidh_init(&cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "HID host did not start: %s", esp_err_to_name(err));
        return false;
    }
    ble_store_config_init();
    nimble_port_freertos_init(HostTask);
    return true;
}

// Scans and connects. esp_hidh_dev_open walks the controller's whole GATT
// database on this task's stack - several KB; 3 KB overflowed - so it gets
// 8 KB, in PSRAM (see board_gamepad_init).
static void PadTask(void*)
{
    while (!ble_hs_synced()) vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t ownAddrType = BLE_OWN_ADDR_PUBLIC;
    ble_hs_id_infer_auto(0, &ownAddrType);
    ESP_LOGI(TAG, "Bluetooth up - hold the controller's pair button to connect it");

    bool announced = false;
    for (;;) {
        if (s_connected.load()) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // until it disconnects
            announced = false;
            continue;
        }
        s_found = false;
        struct ble_gap_disc_params dp = {};
        dp.passive = 0;               // active: the name is in the scan response
        dp.filter_duplicates = 1;
        dp.itvl = 0x50;
        dp.window = 0x30;
        if (!announced) {
            ESP_LOGI(TAG, "looking for a controller");
            announced = true;
        }
        int rc = ble_gap_disc(ownAddrType, 30000, &dp, GapEvent, nullptr);
        if (rc != 0) {
            ESP_LOGW(TAG, "scan did not start (%d)", rc);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);       // found one, or the scan ended
        if (!s_found) continue;
        ble_gap_disc_cancel();
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_LOGI(TAG, "found %s, connecting", s_foundName);
        esp_hidh_dev_t* dev =
            esp_hidh_dev_open(s_foundAddr.val, ESP_HID_TRANSPORT_BLE, s_foundAddr.type);
        if (dev == nullptr) {
            ESP_LOGW(TAG, "could not connect to %s", s_foundName);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        s_connected.store(1);
    }
}

extern "C" void board_gamepad_init(void)
{
    if (!StartBluetooth()) return;
    // Core 0, beside the panel and audio tasks; the game owns core 1. The
    // stack is in PSRAM: this task never touches flash.
    if (xTaskCreatePinnedToCoreWithCaps(PadTask, "re1_pad", 8192, nullptr, 4, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE(TAG, "no memory for the controller task");
}

extern "C" uint32_t board_gamepad_mask(void) { return s_mask.load(); }
extern "C" int board_gamepad_connected(void) { return s_connected.load(); }

#else   // no Bluetooth (QEMU)

extern "C" void board_gamepad_init(void) {}
extern "C" uint32_t board_gamepad_mask(void) { return 0; }
extern "C" int board_gamepad_connected(void) { return 0; }

#endif
