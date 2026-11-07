// re1_fastmath.h - r-esp32-evil: float helpers for the software rasteriser.
//
// re1_div(a, b) is a / b, bit for bit. On the ESP32-S3 GCC turns a float
// division into a call to __divsf3 (ROM / libgcc), which is the FPU's
// division-step sequence below wrapped in a windowed call; the rasteriser
// divides several times per scanline, so the call, entry/retw and the
// register moves around it add up. The sequence is libgcc's own (its
// __divsf3 for XCHAL_HAVE_FP_DIV, copied instruction for instruction), so the
// quotient is the same correctly rounded one. The board checks it against
// the library division at boot (re1_div_selftest).
#pragma once

#if defined(__XTENSA__)
#include <xtensa/config/core-isa.h>
#endif

#if defined(__XTENSA__) && XCHAL_HAVE_FP_DIV
static inline __attribute__((always_inline)) float re1_div(float a, float b)
{
    float f0, f3, f4, f5, f6, f7, f8;
    float f1 = a, f2 = b;
    __asm__(
        "div0.s    %[f3], %[f2]\n\t"
        "nexp01.s  %[f4], %[f2]\n\t"
        "const.s   %[f5], 1\n\t"
        "maddn.s   %[f5], %[f4], %[f3]\n\t"
        "mov.s     %[f6], %[f3]\n\t"
        "mov.s     %[f7], %[f2]\n\t"
        "nexp01.s  %[f2], %[f1]\n\t"
        "maddn.s   %[f6], %[f5], %[f6]\n\t"
        "const.s   %[f5], 1\n\t"
        "const.s   %[f0], 0\n\t"
        "neg.s     %[f8], %[f2]\n\t"
        "maddn.s   %[f5], %[f4], %[f6]\n\t"
        "maddn.s   %[f0], %[f8], %[f3]\n\t"
        "mkdadj.s  %[f7], %[f1]\n\t"
        "maddn.s   %[f6], %[f5], %[f6]\n\t"
        "maddn.s   %[f8], %[f4], %[f0]\n\t"
        "const.s   %[f3], 1\n\t"
        "maddn.s   %[f3], %[f4], %[f6]\n\t"
        "maddn.s   %[f0], %[f8], %[f6]\n\t"
        "neg.s     %[f2], %[f2]\n\t"
        "maddn.s   %[f6], %[f3], %[f6]\n\t"
        "maddn.s   %[f2], %[f4], %[f0]\n\t"
        "addexpm.s %[f0], %[f7]\n\t"
        "addexp.s  %[f6], %[f7]\n\t"
        "divn.s    %[f0], %[f2], %[f6]\n\t"
        : [f0] "=&f"(f0), [f1] "+f"(f1), [f2] "+f"(f2), [f3] "=&f"(f3), [f4] "=&f"(f4),
          [f5] "=&f"(f5), [f6] "=&f"(f6), [f7] "=&f"(f7), [f8] "=&f"(f8));
    return f0;
}
#define RE1_DIV_INLINE 1
#else
static inline float re1_div(float a, float b) { return a / b; }
#define RE1_DIV_INLINE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Compares re1_div with the library division over random and special
// operands; returns the number of results that differ in any bit (0 = good).
// `checked` gets the number of pairs tried.
int re1_div_selftest(int* checked);
#ifdef __cplusplus
}
#endif
