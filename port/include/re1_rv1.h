// re1_rv1.h - the ".rv1" movie container written by tools/re1tool.py.
//
// The PC movies are 320x240 Cinepak AVIs at 10 fps. Decoding Cinepak on the
// ESP32-S3 is possible but pointless: the panel shows 160x120. re1tool.py
// re-encodes every movie once into frames the board can blit straight out of
// the SD card, plus one contiguous PCM track the mixer streams like a bank.
//
//   offset size
//   0      4    magic "RV1\0"
//   4      2    version (1)
//   6      2    width
//   8      2    height
//   10     1    codec   0 = RGB565 LE, width*height*2 bytes
//                       1 = 256-colour: 256 x u16 RGB565 palette + w*h indices
//   11     1    reserved
//   12     4    fps numerator
//   16     4    fps denominator
//   20     4    frame count
//   24     4    index offset: frame count x { u32 offset, u32 size }
//   28     4    audio offset (S16 LE PCM, interleaved)
//   32     4    audio frames
//   36     4    audio rate (Hz)
//   40     2    audio channels
//   42     2    audio bits (16)
//   44     4    duration in milliseconds
//   48     16   reserved (0)
//
// All integers little-endian.
#pragma once

#include <stdint.h>

#define RV1_HEADER_SIZE 64
#define RV1_CODEC_RGB565 0
#define RV1_CODEC_PAL8   1

typedef struct Rv1Header {
    char     magic[4];
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint8_t  codec;
    uint8_t  reserved0;
    uint32_t fpsNum;
    uint32_t fpsDen;
    uint32_t frameCount;
    uint32_t indexOffset;
    uint32_t audioOffset;
    uint32_t audioFrames;
    uint32_t audioRate;
    uint16_t audioChannels;
    uint16_t audioBits;
    uint32_t durationMs;
    uint8_t  reserved1[16];
} Rv1Header;

#ifdef __cplusplus
static_assert(sizeof(Rv1Header) == RV1_HEADER_SIZE, "rv1 header is 64 bytes");
#endif
