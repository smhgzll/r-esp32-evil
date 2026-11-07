// re1_remote.h - wire format of the browser link ("RVL").
//
// The game target and tools/re1tool.py's bridge exchange packets over any
// byte stream (UART, USB-CDC, the QEMU serial socket, a TCP socket):
//
//   0xA5 0x5A <type:u8> <len:u32 LE> <payload: len bytes> 0x5A
//
// Bytes outside a packet are console text (ESP-IDF logs, printf) and are shown
// in the bridge's console panel, so the link can share the console UART.
// A packet is always written with a single write call, so console output from
// other tasks can only land *between* packets (uart_write_bytes holds the TX
// mutex for the whole call).
//
// target -> bridge
//   'F' frame   u16 w, u16 h, u8 codec, u8 flags, u32 frameNo, data
//               codec 0: raw RGB565 LE, w*h pixels
//               codec 1: delta against the previous frame, a sequence of
//                        { u16 skip, u16 count, count x u16 pixel }
//                        covering w*h pixels
//   'A' audio   u16 rate, u8 channels, u8 reserved, S16 LE PCM
//   'I' info    UTF-8 JSON object (target name, fb size, heap) - sent once
// bridge -> target
//   'K' key     u8 vk (Win32 virtual-key code), u8 down
//   'P' pad     u32 Marni pad mask (browser Gamepad API / on-screen pad)
//   'R' refresh request a codec-0 key frame
//   'C' command UTF-8 text, at most 56 bytes (re1tool.py room / cmd):
//               "room 112"  go to a room (hex id) on the room tour - from the
//                           title a new game starts for it, movies skipped
//               "room 112 2" ...and its second camera (1-based, as the
//                           tour band counts them)
//               "cam 2"     another camera of the room on screen
//               "auto [112]" the auto tour, from that room or the first
//               "stop"      end the tour
//               "save 1"    save the game in progress to savedat1.dat (1-8),
//                           the typewriter's file image
//               "load 1"    load it back (fades out, restarts from the file)
//               "saves"     list the save folder and the slot files' sizes
//               "sdtest"    write, read back and compare test files on the
//                           card (512 B - 256 KB), then remove them
//               "tone"      2 s of 440 Hz on the sound output (wiring check);
//                           also says whether the game's mix was silent
#pragma once

#include <stddef.h>
#include <stdint.h>

#define RVL_MAGIC0 0xA5
#define RVL_MAGIC1 0x5A
#define RVL_TAIL   0x5A
#define RVL_HEADER 7          // magic(2) + type(1) + len(4)

#define RVL_FRAME_RAW   0
#define RVL_FRAME_DELTA 1

#ifdef __cplusplus
extern "C" {
#endif

// Start the sender thread (no-op when the target has no remote link).
void re1_remote_init(void);
// Offer a finished frame. Copied only if the sender is idle, so this never
// blocks the game; frames are dropped while a slow link is still busy.
void re1_remote_offer_frame(const uint16_t* fb, int w, int h);
// Offer mixed audio (host only: a UART is too slow for it).
void re1_remote_offer_audio(const int16_t* stereo, int frames, int rate);
// Drain and act on input packets: keys -> plat_key_event, pad -> mask.
void re1_remote_poll(void);
// Pad mask most recently sent by the bridge (0 when none).
uint32_t re1_remote_pad_mask(void);

#ifdef __cplusplus
}
#endif
