// MarniDX_SW.cpp - software implementation of the MarniDX backend interface.
//
// The r-esp32-evil counterpart of decomp/src/marni/MarniDX_GL.cpp. It defines
// the same class with the same semantics, so every caller in decomp/src/game
// works unchanged; the reference for each behaviour is the GL backend:
//   - textures are RGBA8 (r in the lowest byte) or 24-bit BGR, CLAMP_TO_EDGE,
//     UVs normalised against the texture's LOGICAL size
//   - fragment = texel * vertex colour
//   - blends: ALPHA (src*a + dst*(1-a)), ADD (src + dst), ADD_ALPHA
//     (src*a + dst), DISABLE (overwrite)
//   - DrawTriangles3D tests LEQUAL and optionally writes depth,
//     DrawTrianglesPersp tests only (when asked), 2D paths never touch depth
//   - the depth buffer is cleared by Clear()
//
// Resolution. The game is laid out for a 320x240 back buffer (the logical
// size reported to it). Pixels are produced at RE1_FB_W x RE1_FB_H (160x120 on
// the ST7735), i.e. every coordinate is scaled by s = RE1_FB_W / 320. To keep
// 2D art legible at half resolution and to quarter texture memory, textures
// are box-filtered by the same factor when uploaded (alpha-weighted 2x2
// average): sampling a pre-filtered texture with point sampling at s = 1/2 is
// the same as rendering at 320x240 and downscaling the frame, at a quarter of
// the fill cost.
//
// Texture storage: RGB565 plus, only when needed, an 8-bit alpha plane.
//   OPAQUE  every texel alpha 255            2 bytes/texel
//   KEYED   alpha is 0 or 255 only            2 bytes/texel, 0x0000 = clear
//                                             (opaque black stored as 0x0020)
//   ALPHA   anything else                     3 bytes/texel
#include "marni/MarniDX.h"
#include "game/Types.h"

#include "MarniSW.h"
#include "re1_remote.h"
#include "re1_target.h"
#include "re1_fastmath.h"
#include "re1_prof.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARNI_MAX_TEXTURES 2048

#if (RE1_FB_W * 2 <= RE1_LOGICAL_W)
#define SW_TEX_DOWNSCALE 2
#else
#define SW_TEX_DOWNSCALE 1
#endif

// RE1_SW_RASTER_STATS=1: count the rasteriser's CPU cycles by stage - triangle
// set-up, scanline set-up, the pixel loop - into the [WORK] line (rcyc_*),
// to see where a triangle's time goes on the target. Off by default: it reads
// the clock twice a scanline.
#ifndef RE1_SW_RASTER_STATS
#define RE1_SW_RASTER_STATS 0
#endif
#if RE1_SW_RASTER_STATS
#define RSTAT_NOW() re1t_prof_ticks()
#define RSTAT_ADD(counter, n) RE1_WORK_ADD(counter, n)
#else
#define RSTAT_NOW() 0u
#define RSTAT_ADD(counter, n) ((void)0)
#endif
#ifndef RE1_SW_PERSP_SPAN
#define RE1_SW_PERSP_SPAN 16     // perspective-correct every N pixels, affine between
#endif

namespace {

// TEX_PAL8: model skins (RE1_PORT_TEXTURE_PAL8). `c` is the 256-entry RGB565
// palette, `a` the 8-bit index plane; index 0 is clear.
enum TexFmt : uint8_t { TEX_OPAQUE = 0, TEX_KEYED = 1, TEX_ALPHA = 2, TEX_PAL8 = 3 };

struct Tex {
    bool      live = false;
    int       w = 0, h = 0;      // logical size (what callers see)
    int       sw = 0, sh = 0;    // stored size
    TexFmt    fmt = TEX_OPAQUE;
    uint16_t* c = nullptr;       // sw*sh RGB565
    uint8_t*  a = nullptr;       // sw*sh alpha (TEX_ALPHA only)
    unsigned  serial = 0;
    size_t    bytes = 0;
};

struct Vtx {
    float x, y;      // framebuffer pixels
    float z;         // 0..1
    float iw;        // 1/w (perspective) or 1
    float u, v;      // texels
    float r, g, b, a;  // 0..255
};

inline uint16_t Pack565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
inline int R8(uint16_t c) { int r = (c >> 11) & 31; return (r << 3) | (r >> 2); }
inline int G8(uint16_t c) { int g = (c >> 5) & 63;  return (g << 2) | (g >> 4); }
inline int B8(uint16_t c) { int b = c & 31;         return (b << 3) | (b >> 2); }
inline int Clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

}  // namespace

// ---------------------------------------------------------------------------
// Backend state
// ---------------------------------------------------------------------------
struct MarniDX::Impl {
    bool ready = false;
    int  viewW = RE1_LOGICAL_W;
    int  viewH = RE1_LOGICAL_H;
    float scale = (float)RE1_FB_W / (float)RE1_LOGICAL_W;

    uint16_t* fb[2] = { nullptr, nullptr };
    int       back = 0;
    uint16_t* depth = nullptr;
    const uint16_t* lastPresented = nullptr;

    MarniHandle whiteHandle = MARNI_NULL_HANDLE;
    MarniHandle fontHandle = MARNI_NULL_HANDLE;
    int fontW = 0, fontH = 0;

    Tex slots[MARNI_MAX_TEXTURES];
    size_t texBytes = 0;
    unsigned texCount = 0;

    unsigned frames = 0, trisThisFrame = 0, spritesThisFrame = 0;

    // Text layer (see FlushText)
    bool        nextFullRes = false;      // the next CreateTexture is the font
    bool        hintModelSkin = false;    // RE1_PORT_TEXTURE_PAL8 (MarniSystem.cpp)
    MarniHandle textFont = MARNI_NULL_HANDLE;
    float       textScaleMsg = 2.0f;      // 8x14 game text
    float       textScaleMenu = 2.0f;     // 8x8 menu text
    // Text blocks already drawn this frame (laid-out and original boxes), so
    // a later run never grows into them. Reset by Present.
    static const int kMaxPlaced = 128;
    struct PlacedBox { float x0, y0, x1, y1; };
    PlacedBox placedLaid[kMaxPlaced], placedOrig[kMaxPlaced];
    int       placedCount = 0;
    // Scale level of the last frames (see FlushText); 0 = full size.
    static const int kHold = 20;
    int       textLevelHistory[kHold] = {};
    int       textLevelPos = 0;
    int       textLevelFrame = 0;     // largest level any run needed this frame
    unsigned trisLast = 0, spritesLast = 0;

    // FPS overlay (see DrawFpsOverlay): presents per second, measured over
    // half-second windows. The pixels under the box are kept so the next
    // frame and CaptureBackbufferToRGBA see the picture without it.
    static const int kFpsW = 40, kFpsH = 9;
    bool     fpsShow = true;
    uint32_t fpsT0 = 0;
    unsigned fpsF0 = 0;
    int      fpsValue = -1;            // -1 until the first window closes
    int      fpsBoxW = 0;              // 0 = nothing drawn on the last frame
    uint16_t fpsUnder[kFpsW * kFpsH];
    // Status band (re1port_set_status): two lines at the bottom, kept like
    // the FPS box.
    static const int kBandH = 21;
    bool     bandOn = false;           // drawn on the last frame
    uint16_t bandUnder[RE1_FB_W * kBandH];

    uint16_t* Back() { return fb[back]; }

    void FreeTexStorage(Tex& t) {
        if (t.c) re1t_free_large(t.c);
        if (t.a) re1t_free_large(t.a);
        texBytes -= t.bytes;
        t.c = nullptr; t.a = nullptr; t.bytes = 0;
    }

    void ReleaseSlot(MarniHandle h) {
        if (h == MARNI_NULL_HANDLE || h >= MARNI_MAX_TEXTURES) return;
        Tex& t = slots[h];
        if (!t.live) return;
        FreeTexStorage(t);
        t.live = false;
        if (h == textFont) textFont = MARNI_NULL_HANDLE;   // the handle may be reused
        t.w = t.h = t.sw = t.sh = 0;
        t.serial++;
        texCount--;
    }
};

static MarniDX* g_pDX = nullptr;
MarniDX* Marni_DX() { return g_pDX; }

// ---------------------------------------------------------------------------
// Texture conversion
// ---------------------------------------------------------------------------

// RE1_PORT_TEXTURE_INDEXED (decomp TextureLoader.cpp): PS1 indexed pixels
// (4 bpp nibble-packed low nibble first, or 8 bpp) + a 15-bit CLUT; colour
// 0x0000 is clear, every other colour opaque - the PS1 GPU rule that
// EffectTextureSheet_BuildRGBA applies.
#define RE1_BPP_INDEXED 0x1D8
#define RE1_BPP_KEYED15 0x1D9   // RE1_PORT_VRAM16: bit 15 opaque, bits 0-14 BGR555
struct Re1IndexedPixels {
    const void*           pixels;
    const unsigned short* clut;
    int                   bpp;
};

// Read source texel (x,y) as RGBA8 from a 32-bit RGBA (r lowest) or 24-bit
// BGR image, 15-bit PS1 pixels, or indexed PS1 pixels.
static inline void SrcTexel(const uint8_t* src, int bpp, int w, int x, int y,
                            int* r, int* g, int* b, int* a)
{
    if (bpp == 32) {
        const uint8_t* p = src + ((size_t)y * w + x) * 4;
        *r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
    } else if (bpp == RE1_BPP_KEYED15) {
        const uint16_t px = ((const uint16_t*)src)[(size_t)y * w + x];
        *r = (px & 0x1F) * 255 / 31;
        *g = ((px >> 5) & 0x1F) * 255 / 31;
        *b = ((px >> 10) & 0x1F) * 255 / 31;
        *a = (px & 0x8000) ? 255 : 0;
    } else if (bpp == RE1_BPP_INDEXED) {
        const Re1IndexedPixels* d = (const Re1IndexedPixels*)src;
        const uint8_t* px = (const uint8_t*)d->pixels;
        unsigned idx;
        if (d->bpp == 4) {
            const uint8_t two = px[(size_t)y * (w / 2) + x / 2];
            idx = (x & 1) ? (two >> 4) : (two & 0xF);
        } else {
            idx = px[(size_t)y * w + x];
        }
        const uint16_t c = d->clut[idx];
        *r = (c & 0x1F) * 255 / 31;
        *g = ((c >> 5) & 0x1F) * 255 / 31;
        *b = ((c >> 10) & 0x1F) * 255 / 31;
        *a = (c == 0x0000) ? 0 : 255;
    } else if (bpp == 15) {
        // PS1 BGR555 as display_image converts it (red in the low bits), opaque.
        const uint16_t px = ((const uint16_t*)src)[(size_t)y * w + x];
        *r = (px & 0x1F) * 255 / 31;
        *g = ((px >> 5) & 0x1F) * 255 / 31;
        *b = ((px >> 10) & 0x1F) * 255 / 31;
        *a = 255;
    } else {
        const uint8_t* p = src + ((size_t)y * w + x) * 3;
        *r = p[2]; *g = p[1]; *b = p[0]; *a = 255;
    }
}

// Convert + box filter `src` into texture `t` (logical size already set).
// `factor` 1 = copy, 2 = alpha-weighted 2x2 average.
// Model skin at half size as palette art: each texel takes the colour most of
// its 2x2 source texels have (so the palette stays the source's own colours),
// clear when most are clear. false when the picture has partial alpha or more
// than 255 colours - the caller then stores it the usual way.
static bool StorePal8(MarniDX::Impl* p, Tex& t, const uint8_t* src, int factor)
{
    const int sw = (t.w + factor - 1) / factor, sh = (t.h + factor - 1) / factor;
    const size_t n = (size_t)sw * sh;
    uint8_t* idx = (uint8_t*)re1t_alloc_large(n);
    uint16_t* pal = (uint16_t*)re1t_alloc_large(256 * 2);
    if (idx == nullptr || pal == nullptr) { re1t_free_large(idx); re1t_free_large(pal); return false; }
    // Open-addressing map 565 colour -> palette index.
    static uint16_t keys[1024];
    static uint8_t  vals[1024];
    static bool     used[1024];
    memset(used, 0, sizeof(used));
    int count = 1;
    pal[0] = 0;
    bool ok = true;
    for (int y = 0; y < sh && ok; ++y) {
        for (int x = 0; x < sw && ok; ++x) {
            uint16_t cand[4];
            int votes[4];
            int nc = 0, clear = 0, total = 0;
            for (int dy = 0; dy < factor; ++dy) {
                const int yy = y * factor + dy;
                if (yy >= t.h) break;
                for (int dx = 0; dx < factor; ++dx) {
                    const int xx = x * factor + dx;
                    if (xx >= t.w) break;
                    const uint8_t* q = src + ((size_t)yy * t.w + xx) * 4;
                    ++total;
                    if (q[3] == 0) { ++clear; continue; }
                    if (q[3] != 255) { ok = false; break; }
                    const uint16_t c = Pack565(q[0], q[1], q[2]);
                    int k = 0;
                    while (k < nc && cand[k] != c) ++k;
                    if (k == nc && nc < 4) { cand[nc] = c; votes[nc] = 0; ++nc; }
                    if (k < nc) votes[k]++;
                }
                if (!ok) break;
            }
            if (!ok) break;
            uint8_t out = 0;
            if (clear * 2 < total && nc > 0) {
                int best = 0;
                for (int k = 1; k < nc; ++k) if (votes[k] > votes[best]) best = k;
                const uint16_t c = cand[best];
                unsigned hsh = ((unsigned)c * 2654435761u) >> 22;
                while (used[hsh] && keys[hsh] != c) hsh = (hsh + 1) & 1023;
                if (!used[hsh]) {
                    if (count >= 256) { ok = false; break; }
                    used[hsh] = true;
                    keys[hsh] = c;
                    vals[hsh] = (uint8_t)count;
                    pal[count++] = c;
                }
                out = vals[hsh];
            }
            idx[(size_t)y * sw + x] = out;
        }
    }
    if (!ok) { re1t_free_large(idx); re1t_free_large(pal); return false; }
    p->FreeTexStorage(t);
    t.sw = sw;
    t.sh = sh;
    t.fmt = TEX_PAL8;
    t.c = pal;
    t.a = idx;
    t.bytes = n + 512;
    p->texBytes += t.bytes;
    return true;
}

static bool StoreTexture(MarniDX::Impl* p, Tex& t, const void* pixels, int bpp, int factor)
{
    // Palette art (model skins, effect pages) and the full-size font: 8-bit
    // indices + palette. At factor 1 (the font) the copy is exact.
    if ((p->hintModelSkin || p->nextFullRes) && bpp == 32 &&
        StorePal8(p, t, (const uint8_t*)pixels, factor))
        return true;

    const uint8_t* src = (const uint8_t*)pixels;
    const int sw = (t.w + factor - 1) / factor;
    const int sh = (t.h + factor - 1) / factor;
    const size_t n = (size_t)sw * sh;

    const bool opaqueSrc = bpp != 32 && bpp != RE1_BPP_INDEXED && bpp != RE1_BPP_KEYED15;
    uint16_t* c = (uint16_t*)re1t_alloc_large(n * 2);
    uint8_t*  a = opaqueSrc ? nullptr : (uint8_t*)re1t_alloc_large(n);   // freed unless needed
    if (c == nullptr || (a == nullptr && !opaqueSrc)) {
        re1t_free_large(c);
        re1t_free_large(a);
        return false;
    }

    // A source whose alpha is only 0/255 (colour-keyed PSX art: fonts, sprites,
    // model skins) stays binary when filtered: a filtered texel is opaque when
    // at least half of its source texels are, with the average colour of the
    // opaque ones. Keeps those textures at 2 bytes per texel (KEYED).
    bool srcBinary = true;
    if (factor > 1) {
        for (int yy = 0; yy < t.h && srcBinary; ++yy)
            for (int xx = 0; xx < t.w; ++xx) {
                int r, g, b, al;
                SrcTexel(src, bpp, t.w, xx, yy, &r, &g, &b, &al);
                if (al != 0 && al != 255) { srcBinary = false; break; }
            }
    }

    bool allOpaque = true, allBinary = true;
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            int sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
            for (int dy = 0; dy < factor; ++dy) {
                int yy = y * factor + dy;
                if (yy >= t.h) break;
                for (int dx = 0; dx < factor; ++dx) {
                    int xx = x * factor + dx;
                    if (xx >= t.w) break;
                    int r, g, b, al;
                    SrcTexel(src, bpp, t.w, xx, yy, &r, &g, &b, &al);
                    sr += r * al; sg += g * al; sb += b * al; sa += al;
                    cnt++;
                }
            }
            int r = 0, g = 0, b = 0, al = cnt ? sa / cnt : 0;
            if (sa > 0) { r = sr / sa; g = sg / sa; b = sb / sa; }
            if (factor > 1 && srcBinary) al = (sa * 2 >= cnt * 255) ? 255 : 0;
            size_t i = (size_t)y * sw + x;
            c[i] = Pack565(r, g, b);
            if (a) a[i] = (uint8_t)al;
            if (al != 255) allOpaque = false;
            if (al != 0 && al != 255) allBinary = false;
        }
    }

    p->FreeTexStorage(t);
    t.sw = sw;
    t.sh = sh;
    if (allOpaque) {
        t.fmt = TEX_OPAQUE;
        if (a) re1t_free_large(a);
        a = nullptr;
    } else if (allBinary) {
        t.fmt = TEX_KEYED;
        for (size_t i = 0; i < n; ++i) {
            if (a[i] == 0) c[i] = 0x0000;
            else if (c[i] == 0x0000) c[i] = 0x0020;
        }
        re1t_free_large(a);
        a = nullptr;
    } else {
        t.fmt = TEX_ALPHA;
    }
    t.c = c;
    t.a = a;
    t.bytes = n * 2 + (a ? n : 0);
    p->texBytes += t.bytes;
    return true;
}

static int FactorFor(int w, int h)
{
    // 1x1 / tiny textures (white, solid fills) stay as they are.
    return (w >= 4 && h >= 4) ? SW_TEX_DOWNSCALE : 1;
}

// ---------------------------------------------------------------------------
// Pixel pipeline
// ---------------------------------------------------------------------------
namespace {

struct DrawState {
    uint16_t* fb;
    uint16_t* depth;
    const Tex* tex;
    MarniBlend blend;
    bool depthTest;
    bool depthWrite;
    bool persp;
};

// Fetch a texel at (iu, iv) (already clamped). Returns alpha 0..255.
inline int Fetch(const Tex* t, int iu, int iv, uint16_t* out)
{
    const size_t i = (size_t)iv * t->sw + iu;
    if (t->fmt == TEX_PAL8) {
        // `c` is only the 256-entry palette here: never index it by texel.
        const uint8_t k = t->a[i];
        *out = t->c[k];
        return k == 0 ? 0 : 255;
    }
    const uint16_t c = t->c[i];
    *out = c;
    switch (t->fmt) {
    case TEX_OPAQUE: return 255;
    case TEX_KEYED:  return c == 0 ? 0 : 255;
    default:         return t->a[i];
    }
}

// Shade + blend one fragment. tc = texel, ta = texel alpha, (r,g,b,a) vertex
// colour 0..256 (256 = 1.0).
RE1_HOT_FN inline void Fragment(const DrawState& s, uint16_t* dst, uint16_t tc, int ta,
                     int r, int g, int b, int a)
{
    int sr, sg, sb;
    if (r == 256 && g == 256 && b == 256) {
        sr = R8(tc); sg = G8(tc); sb = B8(tc);
    } else {
        sr = (R8(tc) * r) >> 8; sg = (G8(tc) * g) >> 8; sb = (B8(tc) * b) >> 8;
    }
    int sa = (ta * a) >> 8;

    switch (s.blend) {
    case MARNI_BLEND_DISABLE:
        *dst = Pack565(Clamp255(sr), Clamp255(sg), Clamp255(sb));
        return;
    case MARNI_BLEND_ADD: {
        uint16_t d = *dst;
        *dst = Pack565(Clamp255(R8(d) + sr), Clamp255(G8(d) + sg), Clamp255(B8(d) + sb));
        return;
    }
    case MARNI_BLEND_ADD_ALPHA: {
        if (sa <= 0) return;
        uint16_t d = *dst;
        *dst = Pack565(Clamp255(R8(d) + ((sr * sa) >> 8)), Clamp255(G8(d) + ((sg * sa) >> 8)),
                       Clamp255(B8(d) + ((sb * sa) >> 8)));
        return;
    }
    case MARNI_BLEND_ALPHA:
    default: {
        if (sa <= 0) return;
        if (sa >= 255) { *dst = Pack565(Clamp255(sr), Clamp255(sg), Clamp255(sb)); return; }
        uint16_t d = *dst;
        int ia = 255 - sa;
        *dst = Pack565((sr * sa + R8(d) * ia) / 255, (sg * sa + G8(d) * ia) / 255,
                       (sb * sa + B8(d) * ia) / 255);
        return;
    }
    }
}

// (int)ceilf(x) without the libm call (a call into flash, twice per scanline
// on the target): (int) truncates towards zero, which is the ceiling for
// x <= 0 and the floor for x > 0 - one more then, unless x is whole. The same
// value for every x in int range.
inline int CeilInt(float x)
{
    const int i = (int)x;
    return (float)i < x ? i + 1 : i;
}

// Plane gradients of one attribute over the triangle.
struct Grad { float dx, dy; };
inline Grad Gradient(const Vtx& v0, const Vtx& v1, const Vtx& v2, float a0, float a1, float a2,
                     float invDet)
{
    Grad g;
    g.dx = ((a1 - a0) * (v2.y - v0.y) - (a2 - a0) * (v1.y - v0.y)) * invDet;
    g.dy = ((a2 - a0) * (v1.x - v0.x) - (a1 - a0) * (v2.x - v0.x)) * invDet;
    return g;
}

RE1_HOT_FN void RasterTriangleImpl(const DrawState& s, Vtx v0, Vtx v1, Vtx v2);

RE1_HOT_FN void RasterTriangle(const DrawState& s, const Vtx& v0, const Vtx& v1, const Vtx& v2)
{
#if RE1_SW_RASTER_STATS
    const uint32_t t0 = RSTAT_NOW();
    RasterTriangleImpl(s, v0, v1, v2);
    RSTAT_ADD(RE1_WORK_RCYC_TRI, RSTAT_NOW() - t0);   // all of it; lines and pixels inside
#else
    RasterTriangleImpl(s, v0, v1, v2);
#endif
}

RE1_HOT_FN void RasterTriangleImpl(const DrawState& s, Vtx v0, Vtx v1, Vtx v2)
{
    const float det = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (!(fabsf(det) > 1e-6f)) return;   // degenerate (also rejects NaN)

    // Sort by y for the scanline extents.
    if (v1.y < v0.y) { Vtx t = v0; v0 = v1; v1 = t; }
    if (v2.y < v0.y) { Vtx t = v0; v0 = v2; v2 = t; }
    if (v2.y < v1.y) { Vtx t = v1; v1 = v2; v2 = t; }
    const float sdet = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    const float sInv = re1_div(1.0f, sdet);

    int y0 = CeilInt(v0.y - 0.5f);
    int y2 = CeilInt(v2.y - 0.5f);
    if (y0 < 0) y0 = 0;
    if (y2 > RE1_FB_H) y2 = RE1_FB_H;
    if (y0 >= y2) return;

    const bool textured = s.tex != nullptr;
    const bool persp = s.persp;
    // Attributes: perspective path interpolates u/w, v/w, 1/w.
    float au0 = persp ? v0.u * v0.iw : v0.u, au1 = persp ? v1.u * v1.iw : v1.u,
          au2 = persp ? v2.u * v2.iw : v2.u;
    float av0 = persp ? v0.v * v0.iw : v0.v, av1 = persp ? v1.v * v1.iw : v1.v,
          av2 = persp ? v2.v * v2.iw : v2.v;
    const Grad gu = Gradient(v0, v1, v2, au0, au1, au2, sInv);
    const Grad gv = Gradient(v0, v1, v2, av0, av1, av2, sInv);
    const Grad gw = Gradient(v0, v1, v2, v0.iw, v1.iw, v2.iw, sInv);
    const Grad gz = Gradient(v0, v1, v2, v0.z, v1.z, v2.z, sInv);
    const bool flat = v0.r == v1.r && v0.r == v2.r && v0.g == v1.g && v0.g == v2.g &&
                      v0.b == v1.b && v0.b == v2.b && v0.a == v1.a && v0.a == v2.a;
    Grad gr = {0, 0}, gg = {0, 0}, gb = {0, 0}, ga = {0, 0};
    if (!flat) {
        gr = Gradient(v0, v1, v2, v0.r, v1.r, v2.r, sInv);
        gg = Gradient(v0, v1, v2, v0.g, v1.g, v2.g, sInv);
        gb = Gradient(v0, v1, v2, v0.b, v1.b, v2.b, sInv);
        ga = Gradient(v0, v1, v2, v0.a, v1.a, v2.a, sInv);
    }
    const int tw = textured ? s.tex->sw : 1, th = textured ? s.tex->sh : 1;
    // The models' case - perspective, textured, alpha-blended, depth-tested -
    // gets a pixel loop of its own below: the same steps as the general one,
    // with Fetch() and Fragment() written out for it (no per-pixel format and
    // blend switches, no call), so the same pixels.
    const bool fastPx = persp && textured && s.blend == MARNI_BLEND_ALPHA && s.depthTest;
    const bool pal8 = textured && s.tex->fmt == TEX_PAL8;
    const uint8_t* const texIdx = pal8 ? s.tex->a : nullptr;
    const uint16_t* const texPal = textured ? s.tex->c : nullptr;
    const bool depthWrite = s.depthWrite;

    for (int y = y0; y < y2; ++y) {
#if RE1_SW_RASTER_STATS
        const uint32_t tl = RSTAT_NOW();
#endif
        const float yc = (float)y + 0.5f;
        // Long edge v0-v2 and the short edge the scanline crosses.
        float xa = v0.x + (v2.x - v0.x) * re1_div(yc - v0.y, v2.y - v0.y);
        float xb;
        if (yc < v1.y) {
            float dy = v1.y - v0.y;
            xb = dy > 0 ? v0.x + (v1.x - v0.x) * re1_div(yc - v0.y, dy) : v1.x;
        } else {
            float dy = v2.y - v1.y;
            xb = dy > 0 ? v1.x + (v2.x - v1.x) * re1_div(yc - v1.y, dy) : v1.x;
        }
        if (xb < xa) { float t = xa; xa = xb; xb = t; }
        int x0 = CeilInt(xa - 0.5f);
        int x1 = CeilInt(xb - 0.5f);
        if (x0 < 0) x0 = 0;
        if (x1 > RE1_FB_W) x1 = RE1_FB_W;
        if (x0 >= x1) {
            RSTAT_ADD(RE1_WORK_RCYC_LINE, RSTAT_NOW() - tl);
            continue;
        }
        RE1_WORK_ADD(RE1_WORK_TRI_PX, x1 - x0);
        RSTAT_ADD(RE1_WORK_TRI_LINES, 1);

        const float px = (float)x0 + 0.5f - v0.x;
        const float py = yc - v0.y;
        float u = au0 + gu.dx * px + gu.dy * py;
        float v = av0 + gv.dx * px + gv.dy * py;
        float w = v0.iw + gw.dx * px + gw.dy * py;
        float z = v0.z + gz.dx * px + gz.dy * py;
        float r = v0.r + gr.dx * px + gr.dy * py;
        float g = v0.g + gg.dx * px + gg.dy * py;
        float b = v0.b + gb.dx * px + gb.dy * py;
        float a = v0.a + ga.dx * px + ga.dy * py;

        uint16_t* dst = s.fb + y * RE1_FB_W + x0;
        uint16_t* dz = s.depth + y * RE1_FB_W + x0;

        // Perspective: exact u,v every RE1_SW_PERSP_SPAN pixels, affine between.
        float pu = 0, pv = 0, pdu = 0, pdv = 0;
        int seg = 0;
#if RE1_SW_RASTER_STATS
        const uint32_t tp = RSTAT_NOW();
        RSTAT_ADD(RE1_WORK_RCYC_LINE, tp - tl);
#endif

        if (fastPx) {
            // Flat colour does not change along the span.
            const int ri0 = (int)r, gi0 = (int)g, bi0 = (int)b, ai0 = (int)a;
            for (int x = x0; x < x1; ++x, ++dst, ++dz) {
                if (seg == 0) {
                    float iw0 = w > 1e-8f ? re1_div(1.0f, w) : 1e8f;
                    pu = u * iw0;
                    pv = v * iw0;
                    int n = x1 - x < RE1_SW_PERSP_SPAN ? x1 - x : RE1_SW_PERSP_SPAN;
                    float wn = w + gw.dx * n;
                    float iwn = wn > 1e-8f ? re1_div(1.0f, wn) : 1e8f;
                    pdu = re1_div((u + gu.dx * n) * iwn - pu, (float)n);
                    pdv = re1_div((v + gv.dx * n) * iwn - pv, (float)n);
                    seg = n;
                }
                const float su = pu, sv = pv;
                pu += pdu; pv += pdv;
                seg--;

                const float zc = z < 0 ? 0 : (z > 1 ? 1 : z);
                const uint16_t zq = (uint16_t)(zc * 65535.0f);
                if (!(zq > *dz)) {
                    int iu = (int)su, iv = (int)sv;
                    if (iu < 0) iu = 0; else if (iu >= tw) iu = tw - 1;
                    if (iv < 0) iv = 0; else if (iv >= th) iv = th - 1;
                    uint16_t tc;
                    int ta;
                    if (pal8) {
                        const uint8_t k = texIdx[(size_t)iv * tw + iu];
                        tc = texPal[k];
                        ta = k == 0 ? 0 : 255;
                    } else {
                        ta = Fetch(s.tex, iu, iv, &tc);
                    }
                    if (ta != 0) {
                        // Fragment(), MARNI_BLEND_ALPHA.
                        const int ri = flat ? ri0 : (int)r, gi = flat ? gi0 : (int)g;
                        const int bi = flat ? bi0 : (int)b, ai = flat ? ai0 : (int)a;
                        int sr, sg, sb;
                        if (ri == 256 && gi == 256 && bi == 256) {
                            sr = R8(tc); sg = G8(tc); sb = B8(tc);
                        } else {
                            sr = (R8(tc) * ri) >> 8; sg = (G8(tc) * gi) >> 8; sb = (B8(tc) * bi) >> 8;
                        }
                        const int sa = (ta * ai) >> 8;
                        if (sa > 0) {
                            if (sa >= 255) {
                                *dst = Pack565(Clamp255(sr), Clamp255(sg), Clamp255(sb));
                            } else {
                                const uint16_t d = *dst;
                                const int ia = 255 - sa;
                                *dst = Pack565((sr * sa + R8(d) * ia) / 255,
                                               (sg * sa + G8(d) * ia) / 255,
                                               (sb * sa + B8(d) * ia) / 255);
                            }
                        }
                        if (depthWrite) *dz = zq;
                    }
                }
                u += gu.dx; v += gv.dx; w += gw.dx; z += gz.dx;
                if (!flat) { r += gr.dx; g += gg.dx; b += gb.dx; a += ga.dx; }
            }
#if RE1_SW_RASTER_STATS
            RSTAT_ADD(RE1_WORK_RCYC_PX, RSTAT_NOW() - tp);
#endif
            continue;
        }

        for (int x = x0; x < x1; ++x, ++dst, ++dz) {
            float su, sv;
            if (persp) {
                if (seg == 0) {
                    float iw0 = w > 1e-8f ? re1_div(1.0f, w) : 1e8f;
                    pu = u * iw0;
                    pv = v * iw0;
                    int n = x1 - x < RE1_SW_PERSP_SPAN ? x1 - x : RE1_SW_PERSP_SPAN;
                    float wn = w + gw.dx * n;
                    float iwn = wn > 1e-8f ? re1_div(1.0f, wn) : 1e8f;
                    pdu = re1_div((u + gu.dx * n) * iwn - pu, (float)n);
                    pdv = re1_div((v + gv.dx * n) * iwn - pv, (float)n);
                    seg = n;
                }
                su = pu; sv = pv;
                pu += pdu; pv += pdv;
                seg--;
            } else {
                su = u; sv = v;
            }

            bool pass = true;
            uint16_t zq = 0;
            if (s.depthTest || s.depthWrite) {
                float zc = z < 0 ? 0 : (z > 1 ? 1 : z);
                zq = (uint16_t)(zc * 65535.0f);
                if (s.depthTest && zq > *dz) pass = false;
            }
            if (pass) {
                uint16_t tc = 0xFFFF;
                int ta = 255;
                if (textured) {
                    int iu = (int)su, iv = (int)sv;
                    if (iu < 0) iu = 0; else if (iu >= tw) iu = tw - 1;
                    if (iv < 0) iv = 0; else if (iv >= th) iv = th - 1;
                    ta = Fetch(s.tex, iu, iv, &tc);
                }
                const int ca = (int)a;
                if (ta != 0 || s.blend == MARNI_BLEND_DISABLE || s.blend == MARNI_BLEND_ADD) {
                    Fragment(s, dst, tc, ta, (int)r, (int)g, (int)b, ca);
                    // Transparent texels do not occlude (they would in GL,
                    // which has no alpha test - harmless for back-to-front
                    // sorted geometry and it saves the depth traffic).
                    if (s.depthWrite && ta != 0) *dz = zq;
                }
            }
            u += gu.dx; v += gv.dx; w += gw.dx; z += gz.dx;
            if (!flat) { r += gr.dx; g += gg.dx; b += gb.dx; a += ga.dx; }
        }
#if RE1_SW_RASTER_STATS
        RSTAT_ADD(RE1_WORK_RCYC_PX, RSTAT_NOW() - tp);
#endif
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Text layer
//
// At 160x120 the game's 8x8 / 8x14 glyphs land on 4x4 / 4x7 pixels: too small
// for a 1.8" panel. Glyph sprites (draws with the game font, registered by the
// TextureLoader hook) are therefore not drawn immediately but queued; at the
// next non-text draw (or Present) the queued run is laid out again:
//
//  - lines = glyphs on the same baseline; blocks = lines stacked close together
//  - a "paragraph" block (no column gaps: subtitles, messages, descriptions)
//    is scaled by the message scale, every line too wide for the screen is
//    re-wrapped at word gaps, and the block keeps its side of the screen
//    (bottom-anchored when it sat in the lower half, centred when it was)
//  - a "table" block (columns: menus, option pages) is scaled as one piece, by
//    no more than its line spacing and the screen width allow, so columns stay
//    aligned and rows never overlap
//
// Glyphs are drawn from the full-resolution font with a coverage sampler: a
// pixel is ink when >= 25% of its footprint is, in the colour covering most of
// it, so one-texel strokes survive any down-scaling (point sampling drops them).
// Text stays in draw order: a run ends at the first non-text draw.
// ---------------------------------------------------------------------------
namespace {

struct Glyph {
    float x, y, w, h;          // logical (320x240) rectangle
    float u0, v0, u1, v1;
    DWORD color;
    MarniBlend blend;
    float nx, ny, nw, nh;      // laid-out rectangle
};

const int kMaxGlyphs = 512;
Glyph s_glyphs[kMaxGlyphs];
int   s_glyphCount = 0;

struct Line { int first, count; float minX, maxX, y, h; bool columns; int block; };
Line  s_lines[256];
int   s_lineCount = 0;

int s_order[kMaxGlyphs];

void DrawGlyph(MarniDX::Impl* p, const Tex* tex, const Glyph& g)
{
    const float sc = p->scale;
    const float X0 = g.nx * sc, Y0 = g.ny * sc, X1 = (g.nx + g.nw) * sc, Y1 = (g.ny + g.nh) * sc;
    int px0 = (int)floorf(X0 + 0.5f), px1 = (int)floorf(X1 + 0.5f);
    int py0 = (int)floorf(Y0 + 0.5f), py1 = (int)floorf(Y1 + 0.5f);
    if (px1 <= px0) px1 = px0 + 1;
    if (py1 <= py0) py1 = py0 + 1;
    const float su0 = g.u0 * tex->sw, sv0 = g.v0 * tex->sh;
    const float kx = (g.u1 - g.u0) * tex->sw / (float)(px1 - px0);
    const float ky = (g.v1 - g.v0) * tex->sh / (float)(py1 - py0);
    const int cr = (int)((g.color >> 16) & 0xFF), cg = (int)((g.color >> 8) & 0xFF),
              cb = (int)(g.color & 0xFF), ca = (int)((g.color >> 24) & 0xFF);
    const int r = cr + (cr >> 7), gg = cg + (cg >> 7), b = cb + (cb >> 7), a = ca + (ca >> 7);
    DrawState s;
    s.fb = p->Back();
    s.depth = p->depth;
    s.tex = tex;
    s.blend = g.blend;
    s.depthTest = s.depthWrite = s.persp = false;

    for (int py = py0; py < py1; ++py) {
        if (py < 0 || py >= RE1_FB_H) continue;
        const float ya = sv0 + (py - py0) * ky, yb = ya + ky;
        for (int px = px0; px < px1; ++px) {
            if (px < 0 || px >= RE1_FB_W) continue;
            const float xa = su0 + (px - px0) * kx, xb = xa + kx;
            // Sharp glyphs: a pixel is ink when >= 25% of its footprint is.
            // Its colour is the glyph body (the brightest ink) when that covers
            // a fair part of the pixel - so thin strokes survive shrinking -
            // otherwise the colour covering most of it (the outline).
            uint16_t colors[9];
            float area[9];
            int n = 0;
            float ink = 0.0f;
            for (int ty = (int)floorf(ya); ty < (int)ceilf(yb); ++ty) {
                const float wy = fminf(yb, (float)(ty + 1)) - fmaxf(ya, (float)ty);
                if (wy <= 0 || ty < 0 || ty >= tex->sh) continue;
                for (int tx = (int)floorf(xa); tx < (int)ceilf(xb); ++tx) {
                    const float wx = fminf(xb, (float)(tx + 1)) - fmaxf(xa, (float)tx);
                    if (wx <= 0 || tx < 0 || tx >= tex->sw) continue;
                    uint16_t c;
                    if (Fetch(tex, tx, ty, &c) < 128) continue;
                    const float w = wx * wy;
                    ink += w;
                    int k = 0;
                    while (k < n && colors[k] != c) ++k;
                    if (k == n && n < 9) { colors[n] = c; area[n] = 0; ++n; }
                    if (k < n) area[k] += w;
                }
            }
            if (ink < 0.25f * kx * ky || n == 0) continue;
            int best = 0;
            for (int k = 1; k < n; ++k) if (area[k] > area[best]) best = k;
            int bright = -1, brightLum = -1;
            for (int k = 0; k < n; ++k) {
                const int lum = R8(colors[k]) * 3 + G8(colors[k]) * 6 + B8(colors[k]);
                if (area[k] >= 0.3f * kx * ky && lum > brightLum) { bright = k; brightLum = lum; }
            }
            if (bright >= 0) best = bright;
            Fragment(s, s.fb + py * RE1_FB_W + px, colors[best], 255, r, gg, b, a);
        }
    }
}

float ScaleFor(const MarniDX::Impl* p, float h)
{
    return h <= 9.0f ? p->textScaleMenu : p->textScaleMsg;
}

typedef MarniDX::Impl::PlacedBox Box;

bool Overlap(const Box& a, const Box& b)
{
    const float m = 1.0f;   // keep one logical pixel between blocks
    return a.x0 < b.x1 + m && b.x0 < a.x1 + m && a.y0 < b.y1 + m && b.y0 < a.y1 + m;
}

// Lays out lines [firstLine, lastLine] at scale S (S may be lowered to fit
// the screen). Writes each glyph's n* rectangle.
void LayoutBlockAt(int firstLine, int lastLine, float S)
{
    const float kW = (float)RE1_LOGICAL_W, kH = (float)RE1_LOGICAL_H, kMargin = 3.0f;
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    bool columns = false, centred = true;
    for (int l = firstLine; l <= lastLine; ++l) {
        const Line& L = s_lines[l];
        minX = fminf(minX, L.minX); maxX = fmaxf(maxX, L.maxX);
        minY = fminf(minY, L.y);    maxY = fmaxf(maxY, L.y + L.h);
        columns |= L.columns;
        if (fabsf((L.minX + L.maxX) * 0.5f - kW * 0.5f) > 10.0f) centred = false;
    }
    // Left-aligned when most lines share their left edge (a list, a table, a
    // paragraph); only then is "every line centred on the screen" not taken
    // for centred text - the load screen's rows happen to sit centred.
    {
        const int nLines = lastLine - firstLine + 1;
        int bestVotes = 0;
        for (int l = firstLine; l <= lastLine; ++l) {
            int votes = 0;
            for (int m = firstLine; m <= lastLine; ++m)
                if (fabsf(s_lines[m].minX - s_lines[l].minX) < 2.0f) ++votes;
            if (votes > bestVotes) bestVotes = votes;
        }
        if (nLines >= 2 && bestVotes * 10 >= nLines * 6) centred = false;
    }
    const float h = s_lines[firstLine].h;
    const float adv = 8.0f;
    if (S <= 1.0f) {
        for (int l = firstLine; l <= lastLine; ++l)
            for (int i = 0; i < s_lines[l].count; ++i) {
                Glyph& g = s_glyphs[s_order[s_lines[l].first + i]];
                g.nx = g.x; g.ny = g.y; g.nw = g.w; g.nh = g.h;
            }
        return;
    }
    const float pitch = lastLine > firstLine
        ? (s_lines[lastLine].y - s_lines[firstLine].y) / (float)(lastLine - firstLine) : h + 2.0f;

    // The 8x8 menu font grows IN PLACE: every row keeps its height on the
    // screen and only its letters get bigger (no more than the row spacing
    // allows), so the game's own selection bars, cursors and frames - drawn
    // at the original row positions - stay on their rows.
    const bool inPlace = h <= 9.0f;
    if (inPlace && lastLine > firstLine) {
        float minPitch = 1e9f;
        for (int l = firstLine + 1; l <= lastLine; ++l)
            minPitch = fminf(minPitch, s_lines[l].y - s_lines[l - 1].y);
        S = fminf(S, fmaxf(1.0f, (minPitch - 1.0f) / h));
    }
    if (columns || inPlace) {
        // Table. Every line is cut into segments at its column gaps; segments
        // ending at the block's right edge (right-aligned values) grow
        // leftward from that edge, all others rightward from the shared left
        // edge, and rows grow downward from the top. So "LABEL ..... VALUE"
        // keeps its two ends and spends the gap between them. S drops until
        // no segment runs into its neighbour and the block fits the screen.
        float ax = minX;
        {
            int bestVotes = 0;
            for (int l = firstLine; l <= lastLine; ++l) {
                int votes = 0;
                for (int m = firstLine; m <= lastLine; ++m)
                    if (fabsf(s_lines[m].minX - s_lines[l].minX) < 2.0f) ++votes;
                if (votes > bestVotes) { bestVotes = votes; ax = s_lines[l].minX; }
            }
        }
        const float bx = maxX;
        for (; S >= 1.0f; S -= 0.125f) {
            bool ok = true;
            float nMinX = 1e9f, nMaxX = -1e9f, nMaxY = -1e9f;
            for (int l = firstLine; l <= lastLine && ok; ++l) {
                const Line& L = s_lines[l];
                float prevEnd = -1e9f;
                int segStart = 0;
                for (int i = 1; i <= L.count && ok; ++i) {
                    bool cut = (i == L.count);
                    if (!cut) {
                        const Glyph& a0 = s_glyphs[s_order[L.first + i - 1]];
                        const Glyph& b0 = s_glyphs[s_order[L.first + i]];
                        cut = b0.x - (a0.x + a0.w) > 2.5f * adv;
                    }
                    if (!cut) continue;
                    const Glyph& f = s_glyphs[s_order[L.first + segStart]];
                    const Glyph& e = s_glyphs[s_order[L.first + i - 1]];
                    const bool right = segStart > 0 && fabsf((e.x + e.w) - bx) < 12.0f;
                    const float o = right ? bx : (centred ? kW * 0.5f : ax);
                    for (int k = segStart; k < i; ++k) {
                        Glyph& g = s_glyphs[s_order[L.first + k]];
                        g.nx = o + (g.x - o) * S;
                        g.ny = inPlace ? (L.y + L.h * 0.5f) - (L.y + L.h * 0.5f - g.y) * S
                                       : minY + (g.y - minY) * S;
                        g.nw = g.w * S;
                        g.nh = g.h * S;
                    }
                    const float s0 = o + (f.x - o) * S, s1 = o + (e.x + e.w - o) * S;
                    if (s0 < prevEnd + adv * 0.5f) ok = false;   // ran into the previous column
                    prevEnd = s1;
                    nMinX = fminf(nMinX, s0);
                    nMaxX = fmaxf(nMaxX, s1);
                    nMaxY = fmaxf(nMaxY, inPlace ? L.y + L.h * 0.5f * (1.0f + S)
                                                 : minY + (L.y + L.h - minY) * S);
                    segStart = i;
                }
            }
            if (ok && nMaxX - nMinX <= kW - 2 * kMargin && (inPlace || nMaxY - minY <= kH - 2 * kMargin)) {
                float ox = 0, oy = 0;
                if (nMaxX > kW - kMargin) ox = kW - kMargin - nMaxX;
                if (nMinX + ox < kMargin) ox = kMargin - nMinX;
                if (nMaxY > kH - kMargin) oy = kH - kMargin - nMaxY;
                for (int l = firstLine; l <= lastLine; ++l)
                    for (int i = 0; i < s_lines[l].count; ++i) {
                        Glyph& g = s_glyphs[s_order[s_lines[l].first + i]];
                        g.nx += ox;
                        g.ny += oy;
                    }
                return;
            }
            if (S - 0.125f < 1.0f && S > 1.0f) S = 1.125f;   // last try at 1.0 below
        }
        for (int l = firstLine; l <= lastLine; ++l)
            for (int i = 0; i < s_lines[l].count; ++i) {
                Glyph& g = s_glyphs[s_order[s_lines[l].first + i]];
                g.nx = g.x; g.ny = g.y; g.nw = g.w; g.nh = g.h;
            }
        return;
    }

    // Paragraph: re-wrap at word gaps so every line fits.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const float maxW = (kW - 2 * kMargin) / S;   // in unscaled units
        struct Out { int line; int from; int to; float width; };
        Out outs[96];
        int nOut = 0;
        for (int l = firstLine; l <= lastLine && nOut < 96; ++l) {
            const Line& L = s_lines[l];
            int start = 0;
            while (start < L.count && nOut < 96) {
                // Extend word by word while the line fits.
                int end = start, lastFit = -1;
                while (end < L.count) {
                    int wEnd = end + 1;   // one word: glyphs without a gap
                    while (wEnd < L.count) {
                        const Glyph& a = s_glyphs[s_order[L.first + wEnd - 1]];
                        const Glyph& b = s_glyphs[s_order[L.first + wEnd]];
                        if (b.x - (a.x + a.w) > adv * 0.4f) break;
                        ++wEnd;
                    }
                    const Glyph& f = s_glyphs[s_order[L.first + start]];
                    const Glyph& e = s_glyphs[s_order[L.first + wEnd - 1]];
                    if (e.x + e.w - f.x <= maxW || lastFit < 0) lastFit = wEnd;
                    else break;
                    end = wEnd;
                }
                if (lastFit < 0) lastFit = L.count;
                const Glyph& f = s_glyphs[s_order[L.first + start]];
                const Glyph& e = s_glyphs[s_order[L.first + lastFit - 1]];
                outs[nOut++] = {l, start, lastFit, e.x + e.w - f.x};
                start = lastFit;
            }
        }
        const float pitchS = pitch * S;
        const float totalH = (nOut - 1) * pitchS + h * S;
        if (totalH > kH - 2 * kMargin && S > 1.05f) { S -= 0.1f; continue; }

        // Grow downward from where the text started (into the box or band it
        // sits in); only pushed up when it would leave the screen.
        float top = minY;
        if (top + totalH > kH - kMargin) top = kH - kMargin - totalH;
        if (top < kMargin) top = kMargin;
        // Each line keeps its own indent, measured from the left edge most
        // lines share (a cursor in front of one line must not move the rest);
        // then the block as a whole is shifted to fit the screen.
        float ax = minX;
        {
            int bestVotes = 0;
            for (int l = firstLine; l <= lastLine; ++l) {
                int votes = 0;
                for (int m = firstLine; m <= lastLine; ++m)
                    if (fabsf(s_lines[m].minX - s_lines[l].minX) < 2.0f) ++votes;
                if (votes > bestVotes) { bestVotes = votes; ax = s_lines[l].minX; }
            }
        }
        // The fit-to-screen shift only looks at lines that start at the
        // anchor, so an outlier (a blinking cursor) cannot change it.
        float nMin = 1e9f, nMax = -1e9f;
        for (int i = 0; i < nOut; ++i) {
            const float lineX = s_lines[outs[i].line].minX;
            if (fabsf(lineX - ax) >= 2.0f) continue;
            const float x0 = ax + (lineX - ax) * S;
            nMin = fminf(nMin, x0);
            nMax = fmaxf(nMax, x0 + outs[i].width * S);
        }
        float shift = 0;
        if (nMax + shift > kW - kMargin) shift = kW - kMargin - nMax;
        if (nMin + shift < kMargin) shift = kMargin - nMin;

        for (int i = 0; i < nOut; ++i) {
            const Line& L = s_lines[outs[i].line];
            const Glyph& f = s_glyphs[s_order[L.first + outs[i].from]];
            const float lx = centred ? (kW - outs[i].width * S) * 0.5f
                                     : ax + (L.minX - ax) * S + shift;
            const float ly = top + i * pitchS;
            for (int k = outs[i].from; k < outs[i].to; ++k) {
                Glyph& g = s_glyphs[s_order[L.first + k]];
                g.nx = lx + (g.x - f.x) * S;
                g.ny = ly + (g.y - L.y) * S;
                g.nw = g.w * S;
                g.nh = g.h * S;
            }
        }
        return;
    }
}

}  // namespace

static void FlushTextImpl(MarniDX::Impl* p);

static void FlushText(MarniDX::Impl* p)
{
    if (s_glyphCount == 0) return;
    RE1_PROF_SCOPE(RE1_PROF_TEXT);
    FlushTextImpl(p);
}

static void FlushTextImpl(MarniDX::Impl* p)
{
    if (s_glyphCount == 0) return;
    const int n = s_glyphCount;
    s_glyphCount = 0;
    if (p == nullptr || !p->ready || p->textFont == MARNI_NULL_HANDLE ||
        !p->slots[p->textFont].live) return;
    const Tex* tex = &p->slots[p->textFont];

    // Order by baseline then x, and cut into lines.
    for (int i = 0; i < n; ++i) s_order[i] = i;
    for (int i = 1; i < n; ++i) {           // insertion sort: runs are small
        int v = s_order[i], j = i - 1;
        const Glyph& gv = s_glyphs[v];
        while (j >= 0) {
            const Glyph& gj = s_glyphs[s_order[j]];
            const bool after = (gj.y > gv.y + 1.0f) || (fabsf(gj.y - gv.y) <= 1.0f && gj.x > gv.x);
            if (!after) break;
            s_order[j + 1] = s_order[j];
            --j;
        }
        s_order[j + 1] = v;
    }
    s_lineCount = 0;
    for (int i = 0; i < n && s_lineCount < 256; ++i) {
        const Glyph& g = s_glyphs[s_order[i]];
        Line* L = s_lineCount ? &s_lines[s_lineCount - 1] : nullptr;
        if (L && fabsf(g.y - L->y) <= 1.0f && fabsf(g.h - L->h) < 0.5f) {
            const Glyph& prev = s_glyphs[s_order[L->first + L->count - 1]];
            if (g.x - (prev.x + prev.w) > 2.5f * 8.0f) L->columns = true;
            L->count++;
            L->maxX = fmaxf(L->maxX, g.x + g.w);
        } else {
            s_lines[s_lineCount++] = {i, 1, g.x, g.x + g.w, g.y, g.h, false, -1};
        }
    }

    // Blocks: consecutive lines of one height, close below each other and
    // overlapping horizontally.
    struct Block { int first, last; float S; Box orig, laid; int glyphs; };
    static Block blocks[128];
    int nb = 0;
    int blockStart = 0;
    for (int l = 1; l <= s_lineCount && nb < 128; ++l) {
        bool split = (l == s_lineCount);
        if (!split) {
            const Line& a = s_lines[l - 1];
            const Line& b = s_lines[l];
            split = fabsf(a.h - b.h) >= 0.5f || b.y - a.y > 2.0f * a.h ||
                    b.minX > a.maxX + 8.0f || b.maxX < a.minX - 8.0f;
        }
        if (split) {
            Block& B = blocks[nb++];
            B.first = blockStart;
            B.last = l - 1;
            B.S = ScaleFor(p, s_lines[blockStart].h);
            B.orig = {1e9f, 1e9f, -1e9f, -1e9f};
            B.glyphs = 0;
            for (int k = B.first; k <= B.last; ++k) B.glyphs += s_lines[k].count;
            for (int k = B.first; k <= B.last; ++k) {
                const Line& L = s_lines[k];
                B.orig.x0 = fminf(B.orig.x0, L.minX); B.orig.x1 = fmaxf(B.orig.x1, L.maxX);
                B.orig.y0 = fminf(B.orig.y0, L.y);    B.orig.y1 = fmaxf(B.orig.y1, L.y + L.h);
            }
            blockStart = l;
        }
    }
    if (s_lineCount == 0) return;

    // One scale LEVEL for the whole frame: level k scales every block by
    // max(1, Sclass - k * kStep). Pick the smallest k (largest text) at which
    // no grown block runs into another one, nor into text already placed this
    // frame, then hold it: the level used is the largest of this frame's and
    // the last kHold frames', so text shrinks at once when it must but only
    // grows back after the screen has been calm - blinking cursors, rows that
    // appear one frame and not the next, cannot make it pulse.
    const float kStep = 0.25f;
    const float sMax = fmaxf(p->textScaleMsg, p->textScaleMenu);
    const int kMax = (int)ceilf((sMax - 1.0f) / kStep);
    auto layoutAll = [&](int k) {
        for (int bi = 0; bi < nb; ++bi) {
            Block& B = blocks[bi];
            B.S = fmaxf(1.0f, ScaleFor(p, s_lines[B.first].h) - k * kStep);
            LayoutBlockAt(B.first, B.last, B.S);
            B.laid = {1e9f, 1e9f, -1e9f, -1e9f};
            for (int q = B.first; q <= B.last; ++q)
                for (int i = 0; i < s_lines[q].count; ++i) {
                    const Glyph& g = s_glyphs[s_order[s_lines[q].first + i]];
                    B.laid.x0 = fminf(B.laid.x0, g.nx); B.laid.x1 = fmaxf(B.laid.x1, g.nx + g.nw);
                    B.laid.y0 = fminf(B.laid.y0, g.ny); B.laid.y1 = fmaxf(B.laid.y1, g.ny + g.nh);
                }
        }
    };
    // Blocks of one or two glyphs are markers - a blinking "more text" arrow,
    // a cursor - and do not take part: they follow the nearest real block.
    auto tiny = [&](int bi) { return blocks[bi].glyphs <= 2; };
    auto collides = [&]() {
        for (int bi = 0; bi < nb; ++bi) {
            if (tiny(bi)) continue;
            for (int o = bi + 1; o < nb; ++o)
                if (!tiny(o) &&
                    Overlap(blocks[bi].laid, blocks[o].laid) && !Overlap(blocks[bi].orig, blocks[o].orig))
                    return true;
            for (int o = 0; o < p->placedCount; ++o)
                if (Overlap(blocks[bi].laid, p->placedLaid[o]) && !Overlap(blocks[bi].orig, p->placedOrig[o]))
                    return true;
        }
        return false;
    };
    int kNow = kMax;
    for (int k = 0; k <= kMax; ++k) {
        layoutAll(k);
        if (!collides()) { kNow = k; break; }
    }
    if (kNow > p->textLevelFrame) p->textLevelFrame = kNow;
    int kUse = kNow;
    for (int i = 0; i < MarniDX::Impl::kHold; ++i)
        if (p->textLevelHistory[i] > kUse) kUse = p->textLevelHistory[i];
    if (kUse > kMax) kUse = kMax;
    layoutAll(kUse);

    // Markers ride along with the nearest real block (same offset, scaled).
    for (int bi = 0; bi < nb; ++bi) {
        if (!tiny(bi)) continue;
        int best = -1;
        float bestD = 48.0f;
        for (int o = 0; o < nb; ++o) {
            if (tiny(o)) continue;
            const Box& a = blocks[bi].orig;
            const Box& c = blocks[o].orig;
            const float dx = fmaxf(0.0f, fmaxf(c.x0 - a.x1, a.x0 - c.x1));
            const float dy = fmaxf(0.0f, fmaxf(c.y0 - a.y1, a.y0 - c.y1));
            const float d = dx > dy ? dx : dy;
            if (d < bestD) { bestD = d; best = o; }
        }
        if (best < 0) continue;
        const Block& R = blocks[best];
        for (int q = blocks[bi].first; q <= blocks[bi].last; ++q)
            for (int i = 0; i < s_lines[q].count; ++i) {
                Glyph& g = s_glyphs[s_order[s_lines[q].first + i]];
                g.nw = g.w * R.S;
                g.nh = g.h * R.S;
                // Right of / below the block: keep the gap to its laid-out edge.
                g.nx = g.x >= R.orig.x1 ? R.laid.x1 + (g.x - R.orig.x1) * R.S
                                        : R.laid.x0 + (g.x - R.orig.x0) * R.S;
                g.ny = g.y >= R.orig.y1 ? R.laid.y1 + (g.y - R.orig.y1) * R.S
                                        : R.laid.y0 + (g.y - R.orig.y0) * R.S;
                g.nx = fminf(fmaxf(g.nx, 1.0f), (float)RE1_LOGICAL_W - g.nw - 1.0f);
                g.ny = fminf(fmaxf(g.ny, 1.0f), (float)RE1_LOGICAL_H - g.nh - 1.0f);
            }
    }
    for (int bi = 0; bi < nb && p->placedCount < MarniDX::Impl::kMaxPlaced; ++bi) {
        p->placedLaid[p->placedCount] = blocks[bi].laid;
        p->placedOrig[p->placedCount] = blocks[bi].orig;
        p->placedCount++;
    }

    for (int l = 0; l < s_lineCount; ++l)
        for (int i = 0; i < s_lines[l].count; ++i)
            DrawGlyph(p, tex, s_glyphs[s_order[s_lines[l].first + i]]);
}

// Convert caller vertices to framebuffer-space Vtx. Layouts:
//   8 floats  { x, y, u, v, r, g, b, a }            (2D)
//   10 floats { x, y, z, w, u, v, r, g, b, a }       (persp / 3D)
// Triangles drawn since the last MarniSW_TakeTriangleCount ([PERF] line).
static unsigned s_perfTriCount = 0;

// `list` (DrawTriangles3DList) holds one pointer per triangle to its three
// vertices; without it the triangles are consecutive in `verts`.
static void DrawBatchSWImpl(MarniDX::Impl* p, const float* verts, const float* const* list,
                            int triCount, int stride, MarniHandle texH, MarniBlend blend,
                            bool depthTest, bool depthWrite, bool persp);

RE1_HOT_FN static void DrawBatchSW(MarniDX::Impl* p, const float* verts, int triCount, int stride,
                        MarniHandle texH, MarniBlend blend, bool depthTest, bool depthWrite,
                        bool persp, const float* const* list = nullptr)
{
    RE1_PROF_SCOPE(RE1_PROF_RASTER);
    s_perfTriCount += triCount > 0 ? (unsigned)triCount : 0u;
    RE1_WORK_ADD(RE1_WORK_TRIS, triCount > 0 ? triCount : 0);
    RE1_WORK_ADD(RE1_WORK_BATCHES, 1);
    DrawBatchSWImpl(p, verts, list, triCount, stride, texH, blend, depthTest, depthWrite, persp);
}

RE1_HOT_FN static void DrawBatchSWImpl(MarniDX::Impl* p, const float* verts,
                                       const float* const* list, int triCount, int stride,
                                       MarniHandle texH, MarniBlend blend, bool depthTest,
                                       bool depthWrite, bool persp)
{
    if (p == nullptr || !p->ready || (verts == nullptr && list == nullptr) || triCount <= 0)
        return;
    auto vertex = [&](int t, int k) -> const float* {
        return list != nullptr ? list[t] + (size_t)k * stride
                               : verts + (size_t)(t * 3 + k) * stride;
    };
    if (s_glyphCount > 0) {
        // As for sprites: only a large batch (fade quad, full-screen effect)
        // ends the pending text run. Its extent is the union of its triangles'
        // on-screen parts: geometry off the screen draws nothing (and
        // FlushTmdObjects drops it before it gets here), so it must not make
        // a batch "large".
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int t = 0; t < triCount; ++t) {
            float tx0 = 1e9f, ty0 = 1e9f, tx1 = -1e9f, ty1 = -1e9f;
            for (int k = 0; k < 3; ++k) {
                const float* v = vertex(t, k);
                tx0 = fminf(tx0, v[0]); tx1 = fmaxf(tx1, v[0]);
                ty0 = fminf(ty0, v[1]); ty1 = fmaxf(ty1, v[1]);
            }
            tx0 = fmaxf(tx0, 0.0f); ty0 = fmaxf(ty0, 0.0f);
            tx1 = fminf(tx1, (float)RE1_LOGICAL_W); ty1 = fminf(ty1, (float)RE1_LOGICAL_H);
            if (tx0 > tx1 || ty0 > ty1) continue;   // wholly off the screen
            x0 = fminf(x0, tx0); x1 = fmaxf(x1, tx1);
            y0 = fminf(y0, ty0); y1 = fmaxf(y1, ty1);
        }
        if (x1 >= x0 && (x1 - x0) * (y1 - y0) >= (float)(RE1_LOGICAL_W * RE1_LOGICAL_H) / 3.0f)
            FlushText(p);
    }
    const Tex* tex = nullptr;
    if (texH != MARNI_NULL_HANDLE && texH < MARNI_MAX_TEXTURES && p->slots[texH].live)
        tex = &p->slots[texH];
    else if (p->whiteHandle != MARNI_NULL_HANDLE)
        tex = nullptr;   // white: vertex colour only
    if (tex != nullptr && tex->w == 1 && tex->h == 1 && tex->fmt == TEX_OPAQUE &&
        tex->c[0] == 0xFFFF) {
        tex = nullptr;
    }

    DrawState s;
    s.fb = p->Back();
    s.depth = p->depth;
    s.tex = tex;
    s.blend = blend;
    s.depthTest = depthTest;
    s.depthWrite = depthTest && depthWrite;
    s.persp = persp;

    const float sc = p->scale;
    const float tw = tex ? (float)tex->sw : 1.0f;
    const float th = tex ? (float)tex->sh : 1.0f;
    const bool hasZW = stride == 10;

    for (int t = 0; t < triCount; ++t) {
        Vtx v[3];
#if RE1_SW_RASTER_STATS
        const uint32_t tv = RSTAT_NOW();
#endif
        for (int k = 0; k < 3; ++k) {
            const float* in = vertex(t, k);
            const float* rest = hasZW ? in + 4 : in + 2;
            v[k].x = in[0] * sc;
            v[k].y = in[1] * sc;
            v[k].z = hasZW ? in[2] : 0.0f;
            float w = hasZW ? in[3] : 1.0f;
            if (w < 1e-4f) w = 1e-4f;          // as the GL vertex shader clamps
            v[k].iw = persp ? re1_div(1.0f, w) : 1.0f;
            v[k].u = rest[0] * tw;
            v[k].v = rest[1] * th;
            v[k].r = rest[2] * 256.0f;
            v[k].g = rest[3] * 256.0f;
            v[k].b = rest[4] * 256.0f;
            v[k].a = rest[5] * 256.0f;
        }
#if RE1_SW_RASTER_STATS
        RSTAT_ADD(RE1_WORK_RCYC_VTX, RSTAT_NOW() - tv);
#endif
        RasterTriangle(s, v[0], v[1], v[2]);
    }
    p->trisThisFrame += (unsigned)triCount;
}

// ---------------------------------------------------------------------------
// MarniDX lifecycle
// ---------------------------------------------------------------------------

MarniDX::MarniDX() : m_pImpl(new Impl()) {}
MarniDX::~MarniDX() { Destroy(); delete m_pImpl; m_pImpl = nullptr; }

BOOL MarniDX::Create(HWND, int, int, BOOL, int* outWidth, int* outHeight)
{
    Impl* p = m_pImpl;
    if (p == nullptr) return FALSE;
    const size_t fbBytes = (size_t)RE1_FB_W * RE1_FB_H * 2;
    for (int i = 0; i < 2; ++i) {
        if (p->fb[i] == nullptr) p->fb[i] = (uint16_t*)re1t_alloc_fast(fbBytes);
    }
    if (p->depth == nullptr) p->depth = (uint16_t*)re1t_alloc_fast(fbBytes);
    if (p->fb[0] == nullptr || p->fb[1] == nullptr || p->depth == nullptr) {
        fprintf(stderr, "[SW] framebuffer allocation failed\n");
        return FALSE;
    }
    memset(p->fb[0], 0, fbBytes);
    memset(p->fb[1], 0, fbBytes);
    p->lastPresented = p->fb[1];

    const DWORD white = 0xFFFFFFFFu;
    p->whiteHandle = CreateTexture(1, 1, 32, &white, nullptr, nullptr);
    if (p->whiteHandle == MARNI_NULL_HANDLE) return FALSE;

    p->ready = true;
    if (outWidth)  *outWidth = p->viewW;
    if (outHeight) *outHeight = p->viewH;
    fprintf(stderr, "[SW] software renderer %dx%d (logical %dx%d, texture /%d)\n",
            RE1_FB_W, RE1_FB_H, p->viewW, p->viewH, SW_TEX_DOWNSCALE);
    return TRUE;
}

void MarniDX::Destroy()
{
    Impl* p = m_pImpl;
    if (p == nullptr) return;
    for (MarniHandle h = 1; h < MARNI_MAX_TEXTURES; ++h) p->ReleaseSlot(h);
    p->whiteHandle = p->fontHandle = MARNI_NULL_HANDLE;
    p->ready = false;
}

BOOL MarniDX::IsReady() const
{
    return (m_pImpl != nullptr && m_pImpl->ready) ? TRUE : FALSE;
}

void MarniDX::GetBackBufferSize(DWORD* outWidth, DWORD* outHeight) const
{
    const Impl* p = m_pImpl;
    if (outWidth)  *outWidth  = p ? (DWORD)p->viewW : 0;
    if (outHeight) *outHeight = p ? (DWORD)p->viewH : 0;
}

void MarniDX::SetAspectLock(BOOL) {}   // the logical view is always 4:3

int MarniDX::ChangeDisplayMode(DWORD, DWORD, BOOL)
{
    return 1;   // fixed panel
}

int MarniDX::HandleWindowMessage(HWND, UINT, WPARAM, LPARAM)
{
    return 1;
}

unsigned int MarniDX::QueryVideoMemory(BOOL) const
{
    return 8u * 1024u * 1024u;
}

void MarniDX::EnumerateDisplayModes(DisplayModeInfo* outModes, int maxModes, int* outCount)
{
    int count = 0;
    if (outModes != nullptr && maxModes > 0) {
        outModes[0].dwWidth = RE1_LOGICAL_W;
        outModes[0].dwHeight = RE1_LOGICAL_H;
        outModes[0].dwBPP = 16;
        outModes[0].dwRefreshRate = 30;
        outModes[0].dwFlags = 0;
        count = 1;
    }
    if (outCount) *outCount = count;
}

void MarniDX::EnumerateAdapters(D3DRendererInfo* outRenderers, int max, int* outCount)
{
    int count = 0;
    if (outRenderers != nullptr && max > 0) {
        strncpy(outRenderers[0].name, "r-esp32-evil software", sizeof(outRenderers[0].name) - 1);
        outRenderers[0].name[sizeof(outRenderers[0].name) - 1] = '\0';
        outRenderers[0].flags = 0;
        count = 1;
    }
    if (outCount) *outCount = count;
}

RE1_HOT_FN void MarniDX::Clear(float r, float g, float b, float)
{
    Impl* p = m_pImpl;
    if (p == nullptr || !p->ready) return;
    RE1_PROF_SCOPE(RE1_PROF_SPRITE);
    FlushText(p);
    const uint16_t c = Pack565(Clamp255((int)(r * 255.0f)), Clamp255((int)(g * 255.0f)),
                               Clamp255((int)(b * 255.0f)));
    uint16_t* fb = p->Back();
    const int n = RE1_FB_W * RE1_FB_H;
    if (c == 0) memset(fb, 0, (size_t)n * 2);
    else for (int i = 0; i < n; ++i) fb[i] = c;
    memset(p->depth, 0xFF, (size_t)n * 2);
}

// 5x7 glyphs for the FPS overlay: rows top to bottom, bit 4 = left column.
static const uint8_t* FpsGlyph(char ch)
{
    static const uint8_t kDigits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    };
    static const uint8_t kF[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t kP[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t kS[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static const uint8_t kDash[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    if (ch >= '0' && ch <= '9') return kDigits[ch - '0'];
    switch (ch) {
    case 'F': return kF;
    case 'P': return kP;
    case 'S': return kS;
    case '-': return kDash;
    default:  return nullptr;   // space
    }
}

// "NN FPS" in the top-left corner of `fb`, on a darkened box. Saves the
// pixels it covers in p->fpsUnder (restored by Present into the next back
// buffer and patched into CaptureBackbufferToRGBA).
static void DrawFpsOverlay(MarniDX::Impl* p, uint16_t* fb)
{
    const uint32_t now = re1t_time_ms();
    if (p->fpsT0 == 0) { p->fpsT0 = now; p->fpsF0 = p->frames; }
    const uint32_t dt = now - p->fpsT0;
    if (dt >= 500) {
        const unsigned n = p->frames - p->fpsF0;
        p->fpsValue = (int)((n * 1000u + dt / 2) / dt);
        p->fpsT0 = now;
        p->fpsF0 = p->frames;
    }
    p->fpsBoxW = 0;
    if (!p->fpsShow) return;

    char text[8];
    if (p->fpsValue < 0) snprintf(text, sizeof(text), "-- FPS");
    else snprintf(text, sizeof(text), "%d FPS", p->fpsValue > 99 ? 99 : p->fpsValue);
    const int len = (int)strlen(text);
    const int w = len * 6 + 1, h = MarniDX::Impl::kFpsH;   // 1 px border, 1 px gaps
    if (w > MarniDX::Impl::kFpsW || w > RE1_FB_W || h > RE1_FB_H) return;

    for (int y = 0; y < h; ++y)
        memcpy(&p->fpsUnder[y * w], &fb[y * RE1_FB_W], (size_t)w * 2);
    p->fpsBoxW = w;

    // Green at full speed (the game ticks at 30 Hz), yellow, red when slow.
    const uint16_t ink = p->fpsValue >= 25 ? 0x07E0 : (p->fpsValue >= 15 ? 0xFFE0 : 0xF800);
    for (int y = 0; y < h; ++y) {
        uint16_t* row = &fb[y * RE1_FB_W];
        for (int x = 0; x < w; ++x) row[x] = (uint16_t)((row[x] >> 2) & 0x39E7);   // 1/4 bright
    }
    for (int i = 0; i < len; ++i) {
        const uint8_t* g = FpsGlyph(text[i]);
        if (g == nullptr) continue;
        const int x0 = 1 + i * 6;
        for (int y = 0; y < 7; ++y) {
            uint16_t* row = &fb[(1 + y) * RE1_FB_W + x0];
            for (int x = 0; x < 5; ++x)
                if (g[y] & (0x10 >> x)) row[x] = ink;
        }
    }
}

// Status band: two lines the caller sets for the next frame (the room tour's
// room and position), on a dark band at the bottom, in the game's 8x8 font
// at the framebuffer's own pixel size - drawn straight into the frame, not
// through the text layer, so it costs next to nothing.
static char     s_status[2][24];
static unsigned s_statusFrame = ~0u;    // the frame the lines are for

extern "C" void re1port_set_status(const char* line1, const char* line2)
{
    MarniDX* dx = Marni_DX();
    if (dx == nullptr || dx->m_pImpl == nullptr) return;
    snprintf(s_status[0], sizeof(s_status[0]), "%s", line1 ? line1 : "");
    snprintf(s_status[1], sizeof(s_status[1]), "%s", line2 ? line2 : "");
    s_status[0][20] = s_status[1][20] = 0;      // 20 cells of 8 px across
    s_statusFrame = dx->m_pImpl->frames;
}

static void DrawStatusBand(MarniDX::Impl* p, uint16_t* fb)
{
    p->bandOn = false;
    if (s_statusFrame != p->frames) return;
    const int y0 = RE1_FB_H - MarniDX::Impl::kBandH;
    memcpy(p->bandUnder, &fb[y0 * RE1_FB_W], sizeof(p->bandUnder));
    p->bandOn = true;
    for (int i = y0 * RE1_FB_W; i < RE1_FB_H * RE1_FB_W; ++i)
        fb[i] = (uint16_t)((fb[i] >> 2) & 0x39E7);                   // 1/4 bright
    MarniSW_OverlayText(fb, 2, y0 + 2, s_status[0], 0xFFFF);
    MarniSW_OverlayText(fb, 2, y0 + 11, s_status[1], 0xC618);
}

void MarniDX::Present()
{
    Impl* p = m_pImpl;
    if (p == nullptr || !p->ready) return;
    FlushText(p);
    p->placedCount = 0;
    p->textLevelHistory[p->textLevelPos] = p->textLevelFrame;
    p->textLevelPos = (p->textLevelPos + 1) % Impl::kHold;
    p->textLevelFrame = 0;
    uint16_t* done = p->Back();
    DrawFpsOverlay(p, done);
    DrawStatusBand(p, done);
    {
        RE1_PROF_SCOPE(RE1_PROF_PRESENT);   // includes waiting for the panel
        re1t_present(done, RE1_FB_W, RE1_FB_H);
        re1_remote_offer_frame(done, RE1_FB_W, RE1_FB_H);
    }
    p->lastPresented = done;
    p->back ^= 1;
    // GL leaves the new back buffer undefined; keeping the previous frame in
    // it makes a caller that draws without clearing degrade gracefully.
    memcpy(p->Back(), done, (size_t)RE1_FB_W * RE1_FB_H * 2);
    for (int y = 0; y < Impl::kFpsH && p->fpsBoxW > 0; ++y)
        memcpy(&p->Back()[y * RE1_FB_W], &p->fpsUnder[y * p->fpsBoxW], (size_t)p->fpsBoxW * 2);
    if (p->bandOn)
        memcpy(&p->Back()[(RE1_FB_H - Impl::kBandH) * RE1_FB_W], p->bandUnder, sizeof(p->bandUnder));
    p->frames++;
    p->trisLast = p->trisThisFrame;
    p->spritesLast = p->spritesThisFrame;
    p->trisThisFrame = p->spritesThisFrame = 0;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

static MarniHandle AllocSlot(MarniDX::Impl* p)
{
    for (MarniHandle h = 1; h < MARNI_MAX_TEXTURES; ++h) {
        if (!p->slots[h].live) return h;
    }
    return MARNI_NULL_HANDLE;
}

MarniHandle MarniDX::CreateTexture(int width, int height, int bpp, const void* pixelData,
                                   int* outWidth, int* outHeight)
{
    Impl* p = m_pImpl;
    if (p == nullptr || width <= 0 || height <= 0 || pixelData == nullptr) return MARNI_NULL_HANDLE;
    // GL's subset (32, 24) plus 15 = PS1 BGR555 (RE1_PORT_TEXTURE_BGR555).
    if (bpp != 32 && bpp != 24 && bpp != 15 && bpp != RE1_BPP_INDEXED && bpp != RE1_BPP_KEYED15)
        return MARNI_NULL_HANDLE;

    MarniHandle h = AllocSlot(p);
    if (h == MARNI_NULL_HANDLE) return MARNI_NULL_HANDLE;
    Tex& t = p->slots[h];
    t.w = width;
    t.h = height;
    int factor = p->nextFullRes ? 1 : FactorFor(width, height);
    const bool fullRes = p->nextFullRes;
    // Quality is never traded for memory up front. Only when a model/effect
    // picture (RGBA page up to 256x256) cannot be stored at all is it stored
    // smaller - blurry beats white. Backgrounds, masks, the font and every
    // other picture always keep their resolution.
    const bool mayDrop = !fullRes && bpp == 32 && width <= 256 && height <= 256;
    bool stored = StoreTexture(p, t, pixelData, bpp, factor);   // sees nextFullRes (font)
    p->nextFullRes = false;
    if (!stored && mayDrop && width >= 64 && height >= 64) {
        stored = StoreTexture(p, t, pixelData, bpp, factor * 2);
        if (stored) fprintf(stderr, "[SW] low memory: %dx%d texture stored at 1/%d\n",
                            width, height, factor * 2);
    }
    if (!stored) {
        fprintf(stderr, "[SW] out of memory for a %dx%d texture (%u bytes live)\n",
                width, height, (unsigned)p->texBytes);
        return MARNI_NULL_HANDLE;
    }
    t.live = true;
    p->texCount++;
    if (outWidth)  *outWidth = width;
    if (outHeight) *outHeight = height;
    return h;
}

MarniHandle MarniDX::CreateTextureFromBits(CMarniBits* bits)
{
    if (bits == nullptr || bits->m_pPixelData == nullptr) return MARNI_NULL_HANDLE;
    return CreateTexture((int)bits->m_width, (int)bits->m_height, (int)bits->m_bitDepth,
                         bits->m_pPixelData, nullptr, nullptr);
}

BOOL MarniDX::UpdateTexturePixels(MarniHandle tex, const void* pixelData, int width, int height,
                                  int bpp)
{
    Impl* p = m_pImpl;
    if (p == nullptr || tex == MARNI_NULL_HANDLE || tex >= MARNI_MAX_TEXTURES) return FALSE;
    Tex& t = p->slots[tex];
    if (!t.live || pixelData == nullptr) return FALSE;
    if (width != t.w || height != t.h || bpp != 32) return FALSE;
    if (!StoreTexture(p, t, pixelData, 32, FactorFor(width, height))) {
        // The slot keeps its previous picture: say so (a stale mask/skin).
        fprintf(stderr, "[SW] out of memory updating a %dx%d texture (%u bytes live)\n",
                width, height, (unsigned)p->texBytes);
        return FALSE;
    }
    return TRUE;
}

BOOL MarniDX::ReplaceTexture(MarniHandle tex, int width, int height, const void* rgba32)
{
    Impl* p = m_pImpl;
    if (p == nullptr || tex == MARNI_NULL_HANDLE || tex >= MARNI_MAX_TEXTURES) return FALSE;
    Tex& t = p->slots[tex];
    if (!t.live || rgba32 == nullptr || width <= 0 || height <= 0) return FALSE;
    // An HD picture keeps the caller's logical size semantics: UVs stay
    // normalised, so storing it at its own size is transparent.
    int oldW = t.w, oldH = t.h;
    t.w = width;
    t.h = height;
    if (!StoreTexture(p, t, rgba32, 32, FactorFor(width, height))) {
        t.w = oldW;
        t.h = oldH;
        return FALSE;
    }
    return TRUE;
}

unsigned int MarniDX::TextureSerial(MarniHandle tex) const
{
    const Impl* p = m_pImpl;
    if (p == nullptr || tex == MARNI_NULL_HANDLE || tex >= MARNI_MAX_TEXTURES) return 0;
    return p->slots[tex].serial;
}

void MarniDX::GetTextureSize(MarniHandle tex, int* outWidth, int* outHeight) const
{
    const Impl* p = m_pImpl;
    if (outWidth)  *outWidth = 0;
    if (outHeight) *outHeight = 0;
    if (p == nullptr || tex == MARNI_NULL_HANDLE || tex >= MARNI_MAX_TEXTURES) return;
    if (!p->slots[tex].live) return;
    if (outWidth)  *outWidth = p->slots[tex].w;
    if (outHeight) *outHeight = p->slots[tex].h;
}

void MarniDX::DestroyTexture(MarniHandle tex)
{
    if (m_pImpl != nullptr) m_pImpl->ReleaseSlot(tex);
}

MarniHandle MarniDX::WhiteTexture() const
{
    return m_pImpl != nullptr ? m_pImpl->whiteHandle : MARNI_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void DrawSpriteImpl(MarniDX* dx, float x, float y, float w, float h, float u0, float v0,
                           float u1, float v1, DWORD color, MarniHandle texH, MarniBlend blend);

RE1_HOT_FN void MarniDX::DrawSprite(float x, float y, float w, float h, float u0, float v0,
                                    float u1, float v1, DWORD color, MarniHandle texH,
                                    MarniSampler, MarniBlend blend)
{
    RE1_PROF_SCOPE(RE1_PROF_SPRITE);
    DrawSpriteImpl(this, x, y, w, h, u0, v0, u1, v1, color, texH, blend);
}

RE1_HOT_FN static void DrawSpriteImpl(MarniDX* dx, float x, float y, float w, float h, float u0,
                                      float v0, float u1, float v1, DWORD color,
                                      MarniHandle texH, MarniBlend blend)
{
    MarniDX::Impl* p = dx->m_pImpl;
    if (p == nullptr || !p->ready) return;
    p->spritesThisFrame++;

    if (texH != MARNI_NULL_HANDLE && texH == p->textFont && w > 0 && h > 0) {
        if (s_glyphCount >= kMaxGlyphs) FlushText(p);
        Glyph& g = s_glyphs[s_glyphCount++];
        g.x = x; g.y = y; g.w = w; g.h = h;
        g.u0 = u0; g.v0 = v0; g.u1 = u1; g.v1 = v1;
        g.color = color;
        g.blend = blend;
        return;
    }
    // Small sprites (cursors, highlight bars, icons) do not end a text run, so
    // a screen's text is laid out as a whole; a large draw - a full-screen fade
    // or background - does, so text keeps its place under fades.
    if (fabsf(w * h) >= (float)(RE1_LOGICAL_W * RE1_LOGICAL_H) / 3.0f) FlushText(p);

    // Normalise a flipped rectangle (the UVs flip with it).
    if (w < 0) { x += w; w = -w; float t = u0; u0 = u1; u1 = t; }
    if (h < 0) { y += h; h = -h; float t = v0; v0 = v1; v1 = t; }
    if (!(w > 0) || !(h > 0)) return;

    const Tex* tex = nullptr;
    if (texH != MARNI_NULL_HANDLE && texH < MARNI_MAX_TEXTURES && p->slots[texH].live)
        tex = &p->slots[texH];
    if (tex != nullptr && tex->w == 1 && tex->h == 1 && tex->c[0] == 0xFFFF &&
        tex->fmt == TEX_OPAQUE)
        tex = nullptr;

    const float sc = p->scale;
    const float X0 = x * sc, Y0 = y * sc, X1 = (x + w) * sc, Y1 = (y + h) * sc;
    int px0 = (int)ceilf(X0 - 0.5f), px1 = (int)ceilf(X1 - 0.5f);
    int py0 = (int)ceilf(Y0 - 0.5f), py1 = (int)ceilf(Y1 - 0.5f);
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > RE1_FB_W) px1 = RE1_FB_W;
    if (py1 > RE1_FB_H) py1 = RE1_FB_H;
    if (px0 >= px1 || py0 >= py1) return;
    RE1_WORK_ADD(RE1_WORK_SPRITES, 1);
    RE1_WORK_ADD(RE1_WORK_SPRITE_PX, (px1 - px0) * (py1 - py0));

    const int cr = (int)((color >> 16) & 0xFF), cg = (int)((color >> 8) & 0xFF),
              cb = (int)(color & 0xFF), ca = (int)((color >> 24) & 0xFF);
    // 0..255 -> 0..256 so 0xFF is exactly 1.0.
    const int r = cr + (cr >> 7), g = cg + (cg >> 7), b = cb + (cb >> 7), a = ca + (ca >> 7);

    DrawState s;
    s.fb = p->Back();
    s.depth = p->depth;
    s.tex = tex;
    s.blend = blend;
    s.depthTest = s.depthWrite = s.persp = false;

    uint16_t* fb = p->Back();
    if (tex == nullptr) {
        RE1_WORK_ADD(RE1_WORK_SPRITE_SLOW_PX, (px1 - px0) * (py1 - py0));
        for (int py = py0; py < py1; ++py) {
            uint16_t* dst = fb + py * RE1_FB_W + px0;
            for (int px = px0; px < px1; ++px, ++dst) Fragment(s, dst, 0xFFFF, 255, r, g, b, a);
        }
        return;
    }

    // Texel stepping in 16.16 fixed point, sampled at pixel centres.
    const float tw = (float)tex->sw, th = (float)tex->sh;
    const float du = (u1 - u0) * tw / (X1 - X0);
    const float dv = (v1 - v0) * th / (Y1 - Y0);
    const float uStart = u0 * tw + ((float)px0 + 0.5f - X0) * du;
    const float vStart = v0 * th + ((float)py0 + 0.5f - Y0) * dv;
    const int32_t fdu = (int32_t)(du * 65536.0f);
    const int sw = tex->sw, sh = tex->sh;
    const bool plain = r == 256 && g == 256 && b == 256 && a == 256 &&
                       tex->fmt == TEX_OPAQUE &&
                       (blend == MARNI_BLEND_ALPHA || blend == MARNI_BLEND_DISABLE);
    // Room masks and most 2D art: keyed or palette texels at full colour,
    // drawn over (ALPHA) or straight (DISABLE). Fragment() then comes down to
    // "the texel where it is not clear" / "the texel" - the 565 -> 8-bit ->
    // 565 round trip it makes is exact - so the loop below goes straight to
    // that, without the per-texel format and blend switches. Same pixels.
    const bool keyedPlain = r == 256 && g == 256 && b == 256 && a == 256 &&
                            (tex->fmt == TEX_KEYED || tex->fmt == TEX_PAL8) &&
                            (blend == MARNI_BLEND_ALPHA || blend == MARNI_BLEND_DISABLE);
    const bool over = blend == MARNI_BLEND_ALPHA;   // keyedPlain: clear texels kept out

    float vf = vStart;
    for (int py = py0; py < py1; ++py, vf += dv) {
        int iv = (int)vf;
        if (iv < 0) iv = 0; else if (iv >= sh) iv = sh - 1;
        uint16_t* dst = fb + py * RE1_FB_W + px0;
        int32_t fu = (int32_t)(uStart * 65536.0f);
        const uint16_t* row = tex->c + (size_t)iv * sw;
        if (plain) {
            for (int px = px0; px < px1; ++px, ++dst, fu += fdu) {
                int iu = fu >> 16;
                if (iu < 0) iu = 0; else if (iu >= sw) iu = sw - 1;
                *dst = row[iu];
            }
            continue;
        }
        if (keyedPlain && tex->fmt == TEX_PAL8) {
            const uint8_t* idx = tex->a + (size_t)iv * sw;
            const uint16_t* pal = tex->c;
            for (int px = px0; px < px1; ++px, ++dst, fu += fdu) {
                int iu = fu >> 16;
                if (iu < 0) iu = 0; else if (iu >= sw) iu = sw - 1;
                const uint8_t k = idx[iu];
                if (k != 0 || !over) *dst = pal[k];
            }
            continue;
        }
        if (keyedPlain) {
            for (int px = px0; px < px1; ++px, ++dst, fu += fdu) {
                int iu = fu >> 16;
                if (iu < 0) iu = 0; else if (iu >= sw) iu = sw - 1;
                const uint16_t c = row[iu];
                if (c != 0 || !over) *dst = c;
            }
            continue;
        }
        RE1_WORK_ADD(RE1_WORK_SPRITE_SLOW_PX, px1 - px0);
        for (int px = px0; px < px1; ++px, ++dst, fu += fdu) {
            int iu = fu >> 16;
            if (iu < 0) iu = 0; else if (iu >= sw) iu = sw - 1;
            uint16_t tc;
            int ta = Fetch(tex, iu, iv, &tc);
            if (ta == 0 && blend != MARNI_BLEND_DISABLE && blend != MARNI_BLEND_ADD) continue;
            Fragment(s, dst, tc, ta, r, g, b, a);
        }
    }
}

void MarniDX::DrawRect(int x, int y, int w, int h, DWORD color)
{
    DrawSprite((float)x, (float)y, (float)w, (float)h, 0.0f, 0.0f, 1.0f, 1.0f, color,
               MARNI_NULL_HANDLE, MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA);
}

void MarniDX::DrawLine(float x0, float y0, float x1, float y1, float thickness, DWORD color)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0f) return;
    // At half resolution a 1-pixel line would vanish between pixel centres;
    // keep it at least one framebuffer pixel wide.
    const float minT = 1.0f / m_pImpl->scale;
    if (thickness < minT) thickness = minT;
    float nx = -dy / len * (thickness * 0.5f);
    float ny =  dx / len * (thickness * 0.5f);
    float r = ((color >> 16) & 0xFF) / 255.0f, g = ((color >> 8) & 0xFF) / 255.0f,
          b = (color & 0xFF) / 255.0f, a = ((color >> 24) & 0xFF) / 255.0f;
    const float verts[6 * 8] = {
        x0 + nx, y0 + ny, 0, 0, r, g, b, a,
        x1 + nx, y1 + ny, 1, 0, r, g, b, a,
        x0 - nx, y0 - ny, 0, 1, r, g, b, a,
        x0 - nx, y0 - ny, 0, 1, r, g, b, a,
        x1 + nx, y1 + ny, 1, 0, r, g, b, a,
        x1 - nx, y1 - ny, 1, 1, r, g, b, a,
    };
    DrawBatchSW(m_pImpl, verts, 2, 8, MARNI_NULL_HANDLE, MARNI_BLEND_ALPHA, false, false, false);
}

void MarniDX::DrawTriangles(const float* verts, int triCount, MarniHandle tex, MarniSampler,
                            MarniBlend blend)
{
    DrawBatchSW(m_pImpl, verts, triCount, 8, tex, blend, false, false, false);
}

void MarniDX::DrawTrianglesPersp(const float* verts, int triCount, MarniHandle tex, MarniSampler,
                                 MarniBlend blend, bool depthTest)
{
    DrawBatchSW(m_pImpl, verts, triCount, 10, tex, blend, depthTest, false, true);
}

void MarniDX::DrawTriangles3D(const float* verts, int triCount, MarniHandle tex, MarniSampler,
                              MarniBlend blend, bool depthWrite)
{
    DrawBatchSW(m_pImpl, verts, triCount, 10, tex, blend, true, depthWrite, true);
}

#if defined(RE1_PORT_FAST)
void MarniDX::DrawTriangles3DList(const float* const* tris, int triCount, MarniHandle tex,
                                  MarniSampler, MarniBlend blend, bool depthWrite)
{
    DrawBatchSW(m_pImpl, nullptr, triCount, 10, tex, blend, true, depthWrite, true, tris);
}
#endif

// ---------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------

BOOL MarniDX::CaptureBackbufferToRGBA(void** outPixels, DWORD* outWidth, DWORD* outHeight)
{
    Impl* p = m_pImpl;
    if (p == nullptr || !p->ready || outPixels == nullptr) return FALSE;
    const uint16_t* src = p->lastPresented;
    const size_t n = (size_t)RE1_FB_W * RE1_FB_H;
    uint8_t* buf = (uint8_t*)malloc(n * 4);
    if (buf == nullptr) return FALSE;
    for (size_t i = 0; i < n; ++i) {
        uint16_t c = src[i];
        const int x = (int)(i % RE1_FB_W), y = (int)(i / RE1_FB_W);
        if (x < p->fpsBoxW && y < Impl::kFpsH) c = p->fpsUnder[y * p->fpsBoxW + x];
        if (p->bandOn && y >= RE1_FB_H - Impl::kBandH)
            c = p->bandUnder[(y - (RE1_FB_H - Impl::kBandH)) * RE1_FB_W + x];
        buf[i * 4 + 0] = (uint8_t)R8(c);
        buf[i * 4 + 1] = (uint8_t)G8(c);
        buf[i * 4 + 2] = (uint8_t)B8(c);
        buf[i * 4 + 3] = 255;
    }
    *outPixels = buf;
    if (outWidth)  *outWidth = RE1_FB_W;
    if (outHeight) *outHeight = RE1_FB_H;
    return TRUE;
}

BOOL MarniDX::ReadTextureRGBA(MarniHandle, void**, int*, int*)
{
    return FALSE;   // as the GL backend: only the character-select card mask asks
}

// ---------------------------------------------------------------------------
// Font texture
// ---------------------------------------------------------------------------
void MarniDX::SetFontTexture(MarniHandle tex, int width, int height)
{
    if (m_pImpl == nullptr) return;
    m_pImpl->fontHandle = tex;
    m_pImpl->fontW = width;
    m_pImpl->fontH = height;
}
MarniHandle MarniDX::FontTexture() const { return m_pImpl ? m_pImpl->fontHandle : MARNI_NULL_HANDLE; }
int MarniDX::FontTextureWidth() const { return m_pImpl ? m_pImpl->fontW : 0; }
int MarniDX::FontTextureHeight() const { return m_pImpl ? m_pImpl->fontH : 0; }

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
MarniDX* MarniDX_Create()
{
    if (g_pDX != nullptr) return g_pDX;
    g_pDX = new MarniDX();
    return g_pDX;
}

void MarniDX_DestroyGlobal()
{
    if (g_pDX != nullptr) {
        g_pDX->Destroy();
        delete g_pDX;
        g_pDX = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Port extensions (MarniSW.h)
// ---------------------------------------------------------------------------
extern "C" void re1port_texture_hint(int modelSkin)
{
    MarniDX* dx = Marni_DX();
    if (dx != nullptr && dx->m_pImpl != nullptr) dx->m_pImpl->hintModelSkin = modelSkin != 0;
}

extern "C" void re1port_font_upload_begin(void)
{
    MarniDX* dx = Marni_DX();
    if (dx != nullptr && dx->m_pImpl != nullptr) dx->m_pImpl->nextFullRes = true;
}

extern "C" void re1port_font_upload_end(MarniHandle font)
{
    MarniDX* dx = Marni_DX();
    if (dx == nullptr || dx->m_pImpl == nullptr) return;
    dx->m_pImpl->nextFullRes = false;
    dx->m_pImpl->textFont = font;
}

void MarniSW_GetTextScale(float* messages, float* menus)
{
    MarniDX* dx = Marni_DX();
    const bool ok = dx != nullptr && dx->m_pImpl != nullptr;
    if (messages) *messages = ok ? dx->m_pImpl->textScaleMsg : 1.0f;
    if (menus) *menus = ok ? dx->m_pImpl->textScaleMenu : 1.0f;
}

// ---------------------------------------------------------------------------
// Port menu overlay (port/common/PortMenu.cpp). The game is frozen while the
// menu is up; each menu frame is the last game frame, dimmed, with the menu
// drawn on it in the game's own 8x8 font.
// ---------------------------------------------------------------------------
static uint16_t* s_ovlSnap = nullptr;   // the game frame under the menu

int MarniSW_OverlayBegin(void)
{
    MarniDX* dx = Marni_DX();
    if (dx == nullptr || dx->m_pImpl == nullptr || !dx->m_pImpl->ready) return 0;
    MarniDX::Impl* p = dx->m_pImpl;
    const size_t bytes = (size_t)RE1_FB_W * RE1_FB_H * 2;
    if (s_ovlSnap == nullptr) s_ovlSnap = (uint16_t*)re1t_alloc_large(bytes);
    if (s_ovlSnap != nullptr) {
        if (p->lastPresented != nullptr) memcpy(s_ovlSnap, p->lastPresented, bytes);
        else memset(s_ovlSnap, 0, bytes);
        for (int y = 0; y < MarniDX::Impl::kFpsH && p->fpsBoxW > 0; ++y)   // not the FPS box
            memcpy(&s_ovlSnap[y * RE1_FB_W], &p->fpsUnder[y * p->fpsBoxW], (size_t)p->fpsBoxW * 2);
        if (p->bandOn)                                                       // nor the band
            memcpy(&s_ovlSnap[(RE1_FB_H - MarniDX::Impl::kBandH) * RE1_FB_W], p->bandUnder,
                   sizeof(p->bandUnder));
    }
    return 1;
}

uint16_t* MarniSW_OverlayFrame(void)
{
    MarniDX* dx = Marni_DX();
    if (dx == nullptr || dx->m_pImpl == nullptr || !dx->m_pImpl->ready) return nullptr;
    uint16_t* fb = dx->m_pImpl->Back();
    const int n = RE1_FB_W * RE1_FB_H;
    if (s_ovlSnap == nullptr) {
        memset(fb, 0, (size_t)n * 2);
    } else {
        for (int i = 0; i < n; ++i) fb[i] = (uint16_t)((s_ovlSnap[i] >> 2) & 0x39E7);   // 1/4
    }
    return fb;
}

void MarniSW_OverlayPresent(void)
{
    MarniDX* dx = Marni_DX();
    if (dx == nullptr || dx->m_pImpl == nullptr || !dx->m_pImpl->ready) return;
    MarniDX::Impl* p = dx->m_pImpl;
    uint16_t* done = p->Back();
    re1t_present(done, RE1_FB_W, RE1_FB_H);
    re1_remote_offer_frame(done, RE1_FB_W, RE1_FB_H);
    p->lastPresented = done;
    p->back ^= 1;
    p->fpsBoxW = 0;
    p->bandOn = false;
}

void MarniSW_OverlayEnd(void)
{
    MarniDX* dx = Marni_DX();
    if (dx != nullptr && dx->m_pImpl != nullptr && dx->m_pImpl->ready && s_ovlSnap != nullptr) {
        // The game goes on from its own picture, not from the menu's.
        MarniDX::Impl* p = dx->m_pImpl;
        memcpy(p->Back(), s_ovlSnap, (size_t)RE1_FB_W * RE1_FB_H * 2);
        p->lastPresented = p->Back();
    }
    re1t_free_large(s_ovlSnap);
    s_ovlSnap = nullptr;
}

void MarniSW_OverlayFill(uint16_t* fb, int x, int y, int w, int h, uint16_t color)
{
    if (fb == nullptr) return;
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < RE1_FB_H; ++yy)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < RE1_FB_W; ++xx)
            fb[yy * RE1_FB_W + xx] = color;
}

// PrintText8x8's cells: character c at u = c * 8 (mod 256), v = row of 32
// from 0x20, 8x8 each, in the game font texture. Each texel's brightness
// scales `color`, so the glyph keeps its own edge shading. Returns the width
// drawn (8 per character), 0 when the font is not loaded yet.
int MarniSW_OverlayText(uint16_t* fb, int x, int y, const char* text, uint16_t color)
{
    MarniDX* dx = Marni_DX();
    if (fb == nullptr || text == nullptr || dx == nullptr || dx->m_pImpl == nullptr) return 0;
    MarniDX::Impl* p = dx->m_pImpl;
    if (p->textFont == MARNI_NULL_HANDLE || !p->slots[p->textFont].live) return 0;
    const Tex& t = p->slots[p->textFont];
    if (t.w < 256 || t.h < 24 || t.sw <= 0 || t.sh <= 0) return 0;
    const int cr = (color >> 11) & 31, cg = (color >> 5) & 63, cb = color & 31;
    int cx = x;
    for (const char* s = text; *s != 0; ++s, cx += 8) {
        const unsigned char ch = (unsigned char)*s;
        if (ch <= 0x20 || ch >= 0x80) continue;
        const int u = (ch * 8) & 255, v = ((ch - 0x20) >> 5) * 8;
        for (int gy = 0; gy < 8; ++gy) {
            const int py = y + gy;
            if (py < 0 || py >= RE1_FB_H) continue;
            const int ty = (v + gy) * t.sh / t.h;
            for (int gx = 0; gx < 8; ++gx) {
                const int px = cx + gx;
                if (px < 0 || px >= RE1_FB_W) continue;
                const size_t i = (size_t)ty * t.sw + (size_t)((u + gx) * t.sw / t.w);
                uint16_t c;
                int a;
                if (t.fmt == TEX_PAL8) { const uint8_t k = t.a[i]; c = t.c[k]; a = k ? 255 : 0; }
                else {
                    c = t.c[i];
                    a = t.fmt == TEX_OPAQUE ? 255 : (t.fmt == TEX_KEYED ? (c ? 255 : 0) : t.a[i]);
                }
                if (a < 128) continue;
                // 0..256 from the texel's brightness (max of its channels).
                const int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
                int lum = r5 * 8;
                if (g6 * 4 > lum) lum = g6 * 4;
                if (b5 * 8 > lum) lum = b5 * 8;
                lum = lum * 256 / 248;
                fb[py * RE1_FB_W + px] = (uint16_t)(((cr * lum >> 8) << 11) | ((cg * lum >> 8) << 5) |
                                                    (cb * lum >> 8));
            }
        }
    }
    return cx - x;
}

void MarniSW_SetShowFps(int on)
{
    MarniDX* dx = Marni_DX();
    if (dx == nullptr || dx->m_pImpl == nullptr) return;
    dx->m_pImpl->fpsShow = on != 0;
}

int MarniSW_GetShowFps(void)
{
    MarniDX* dx = Marni_DX();
    return dx != nullptr && dx->m_pImpl != nullptr && dx->m_pImpl->fpsShow ? 1 : 0;
}

void MarniSW_SetTextScale(float messages, float menus)
{
    MarniDX* dx = Marni_DX();
    if (dx == nullptr || dx->m_pImpl == nullptr) return;
    auto clampS = [](float v) { return v < 1.0f ? 1.0f : (v > 2.5f ? 2.5f : v); };
    dx->m_pImpl->textScaleMsg = clampS(messages);
    dx->m_pImpl->textScaleMenu = clampS(menus);
}
MarniHandle MarniSW_CreateTexture565(int width, int height, const uint16_t* pixels)
{
    MarniDX* dx = Marni_DX();
    if (dx == nullptr || dx->m_pImpl == nullptr || width <= 0 || height <= 0) return MARNI_NULL_HANDLE;
    MarniDX::Impl* p = dx->m_pImpl;
    MarniHandle h = AllocSlot(p);
    if (h == MARNI_NULL_HANDLE) return MARNI_NULL_HANDLE;
    Tex& t = p->slots[h];
    const size_t n = (size_t)width * height;
    t.c = (uint16_t*)re1t_alloc_large(n * 2);
    if (t.c == nullptr) return MARNI_NULL_HANDLE;
    t.a = nullptr;
    t.w = t.sw = width;
    t.h = t.sh = height;
    t.fmt = TEX_OPAQUE;
    t.bytes = n * 2;
    p->texBytes += t.bytes;
    if (pixels) memcpy(t.c, pixels, n * 2);
    t.live = true;
    p->texCount++;
    return h;
}

BOOL MarniSW_UpdateTexture565(MarniHandle tex, int width, int height, const uint16_t* pixels)
{
    MarniDX* dx = Marni_DX();
    if (dx == nullptr || tex == MARNI_NULL_HANDLE || tex >= MARNI_MAX_TEXTURES) return FALSE;
    Tex& t = dx->m_pImpl->slots[tex];
    if (!t.live || t.sw != width || t.sh != height || t.fmt != TEX_OPAQUE || pixels == nullptr)
        return FALSE;
    memcpy(t.c, pixels, (size_t)width * height * 2);
    return TRUE;
}

unsigned MarniSW_TakeTriangleCount(void)
{
    const unsigned n = s_perfTriCount;
    s_perfTriCount = 0;
    return n;
}

void MarniSW_GetStats(MarniSWStats* out)
{
    MarniDX* dx = Marni_DX();
    if (out == nullptr) return;
    memset(out, 0, sizeof(*out));
    if (dx == nullptr || dx->m_pImpl == nullptr) return;
    const MarniDX::Impl* p = dx->m_pImpl;
    out->frames = p->frames;
    out->trianglesLastFrame = p->trisLast;
    out->spritesLastFrame = p->spritesLast;
    out->textureBytes = (unsigned)p->texBytes;
    out->textureCount = p->texCount;
}
