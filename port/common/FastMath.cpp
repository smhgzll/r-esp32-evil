// FastMath.cpp - the self-test of re1_div (re1_fastmath.h).
#include "re1_fastmath.h"

#include <stdint.h>
#include <string.h>

static uint32_t Bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static float FromBits(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

extern "C" int re1_div_selftest(int* checked)
{
    // Specials (zeros, denormals, infinities, NaN, the extremes) against each
    // other, then random bit patterns and random values in the rasteriser's
    // working range.
    static const uint32_t kSpecial[] = {
        0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x3F800000u,
        0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u,
        0x3EAAAAABu, 0x40490FDBu, 0x4B000000u, 0x33800000u,
    };
    const int nSpecial = (int)(sizeof(kSpecial) / sizeof(kSpecial[0]));
    int tried = 0, bad = 0;
    volatile float va, vb;   // keeps the library division a library call
    for (int i = 0; i < nSpecial; ++i) {
        for (int j = 0; j < nSpecial; ++j) {
            va = FromBits(kSpecial[i]);
            vb = FromBits(kSpecial[j]);
            const float lib = va / vb;
            if (Bits(re1_div(va, vb)) != Bits(lib)) ++bad;
            ++tried;
        }
    }
    uint32_t x = 0x12345678u;
    for (int i = 0; i < 20000; ++i) {
        x = x * 1664525u + 1013904223u;
        const uint32_t ra = x;
        x = x * 1664525u + 1013904223u;
        const uint32_t rb = x;
        if (i & 1) {
            va = FromBits(ra);
            vb = FromBits(rb);
        } else {   // -1000..1000-ish values, like screen coordinates and 1/w
            va = (float)(int32_t)(ra >> 8) * (1.0f / 8192.0f);
            vb = (float)(int32_t)(rb >> 8) * (1.0f / 65536.0f);
        }
        const float lib = va / vb;
        if (Bits(re1_div(va, vb)) != Bits(lib)) ++bad;
        ++tried;
    }
    if (checked != nullptr) *checked = tried;
    return bad;
}
