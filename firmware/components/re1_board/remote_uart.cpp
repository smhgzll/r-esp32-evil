// remote_uart.cpp - the browser link (port/include/re1_remote.h).
//
// The link shares its byte stream with the console, and a packet must reach
// the bridge in one piece. Three transports:
//
//  Board, UART0 ("COM" port): the interrupt-driven UART driver with the
//    console VFS routed through it - every write, packets and printf alike, is
//    one uart_write_bytes call holding the TX mutex, so text can only land
//    between packets.
//  Board, USB-Serial-JTAG ("USB" port): the USB driver with the console VFS
//    routed through it. Packets bigger than its TX ring go out in pieces, so a
//    log line can occasionally land inside one; the bridge then drops that
//    packet and asks for a key frame (tools/re1tool.py Demux).
//  QEMU: Espressif's QEMU raises an endless UART interrupt storm as soon as the
//    driver's ISR is enabled, so the link uses the console VFS itself
//    (/dev/uart/0, polled FIFO); each write() runs under the port's write lock,
//    and line-ending conversion is switched off.
#include "board.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"

#if CONFIG_RE1_REMOTE_PORT_USB
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#if CONFIG_RE1_REMOTE_LINK
static bool s_on = false;
#if CONFIG_RE1_TARGET_QEMU
static int s_fd = -1;
#endif
#endif

void board_remote_init(void)
{
#if CONFIG_RE1_REMOTE_LINK
#if CONFIG_RE1_TARGET_QEMU
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
    uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
    s_fd = open("/dev/uart/0", O_RDWR | O_NONBLOCK);
    if (s_fd < 0) return;
#elif CONFIG_RE1_REMOTE_PORT_USB
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.tx_buffer_size = 16384;
    cfg.rx_buffer_size = 2048;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return;
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
#else
    if (uart_driver_install(UART_NUM_0, 4096, 16384, 0, nullptr, 0) != ESP_OK) return;
    uart_vfs_dev_use_driver(UART_NUM_0);
#endif
    s_on = true;
#if CONFIG_RE1_REMOTE_PORT_USB
    ESP_LOGI("remote", "browser link on the USB port (USB-Serial-JTAG)");
#else
    ESP_LOGI("remote", "browser link on UART0 (%d baud)", CONFIG_ESP_CONSOLE_UART_BAUDRATE);
#endif
#endif
}

int board_remote_enabled(void)
{
#if CONFIG_RE1_REMOTE_LINK
    return s_on ? 1 : 0;
#else
    return 0;
#endif
}

void board_remote_write(const void* data, size_t size)
{
#if CONFIG_RE1_REMOTE_LINK
    if (!s_on) return;
#if CONFIG_RE1_TARGET_QEMU
    const char* p = (const char*)data;
    while (size > 0) {
        ssize_t n = write(s_fd, p, size);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        p += n;
        size -= (size_t)n;
    }
#elif CONFIG_RE1_REMOTE_PORT_USB
    // Pieces that fit the TX ring; give up on a packet when nobody reads the
    // port (no bridge connected) instead of stalling the sender.
    const uint8_t* p = (const uint8_t*)data;
    while (size > 0) {
        const size_t chunk = size > 4096 ? 4096 : size;
        const int n = usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(100));
        if (n <= 0) return;
        p += n;
        size -= (size_t)n;
    }
#else
    uart_write_bytes(UART_NUM_0, data, size);
#endif
#else
    (void)data;
    (void)size;
#endif
}

size_t board_remote_read(void* data, size_t max)
{
#if CONFIG_RE1_REMOTE_LINK
    if (!s_on) return 0;
#if CONFIG_RE1_TARGET_QEMU
    ssize_t n = read(s_fd, data, max);
    return n > 0 ? (size_t)n : 0;
#elif CONFIG_RE1_REMOTE_PORT_USB
    int n = usb_serial_jtag_read_bytes(data, max, 0);
    return n > 0 ? (size_t)n : 0;
#else
    int n = uart_read_bytes(UART_NUM_0, data, max, 0);
    return n > 0 ? (size_t)n : 0;
#endif
#else
    (void)data;
    (void)max;
    return 0;
#endif
}
