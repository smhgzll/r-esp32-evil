// board.h - drivers of the r-esp32-evil board component.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mount point of the game data ("/sd"); the data root is "/sd/re1".
#define BOARD_MOUNT "/sd"
#define BOARD_DATA_ROOT "/sd/re1"

void board_init(void);            // everything below, in order

void board_display_init(void);
void board_display_present(const uint16_t* fb);

int  board_storage_init(void);    // SD card (board) or semihosting (QEMU); 1 = ok

int  board_audio_init(int rate, void (*pull)(int16_t*, int));

void     board_input_init(void);
uint32_t board_input_mask(void);  // Marni pad mask
int      board_input_present(void);
// Wiring check: a test tone of `ms`; returns the game mix's peak since the
// last call (audio_i2s.cpp).
int      board_audio_tone(int ms);
// A Bluetooth LE controller (gamepad_ble.cpp): Marni pad mask, 0 when none.
void     board_gamepad_init(void);
uint32_t board_gamepad_mask(void);
int      board_gamepad_connected(void);

void   board_remote_init(void);   // UART0 packet link
int    board_remote_enabled(void);
void   board_remote_write(const void* data, size_t size);
size_t board_remote_read(void* data, size_t max);

// QEMU only: register the simcall-backed VFS at BOARD_MOUNT.
int  board_semihost_vfs_register(const char* mount, const char* hostDir);

#ifdef __cplusplus
}
#endif
