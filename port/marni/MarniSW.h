// MarniSW.h - extensions of the software MarniDX backend (MarniDX_SW.cpp)
// that only port code uses. Game code keeps using MarniDX.h.
#pragma once

#include "marni/MarniDX.h"
#include <stdint.h>

// An opaque RGB565 texture stored 1:1 (never box-filtered on upload): movie
// frames are already at panel resolution.
MarniHandle MarniSW_CreateTexture565(int width, int height, const uint16_t* pixels);
BOOL        MarniSW_UpdateTexture565(MarniHandle tex, int width, int height,
                                     const uint16_t* pixels);

// Text layer scales (1.0 = original layout; clamped to 1.0..2.5).
// messages: the 8x14 game text; menus: the 8x8 font.
void MarniSW_SetTextScale(float messages, float menus);
void MarniSW_GetTextScale(float* messages, float* menus);

// "NN FPS" counter in the top-left corner (LCD and browser stream).
void MarniSW_SetShowFps(int on);
int  MarniSW_GetShowFps(void);

// Port menu overlay (PortMenu.cpp): Begin snapshots the last game frame,
// Frame returns the back buffer filled with it dimmed, Present shows it,
// End puts the game's own frame back. Text uses the game's 8x8 font at the
// framebuffer's own pixel size (8 px per character); 0 = font not loaded.
int       MarniSW_OverlayBegin(void);
uint16_t* MarniSW_OverlayFrame(void);
void      MarniSW_OverlayPresent(void);
void      MarniSW_OverlayEnd(void);
void      MarniSW_OverlayFill(uint16_t* fb, int x, int y, int w, int h, uint16_t color);
int       MarniSW_OverlayText(uint16_t* fb, int x, int y, const char* text, uint16_t color);

// Two status lines on a dark band at the bottom of the NEXT presented frame
// only (call it every frame to keep them): the room tour's overlay.
extern "C" void re1port_set_status(const char* line1, const char* line2);

// Renderer statistics for the debug overlay / logs.
struct MarniSWStats {
    unsigned frames;
    unsigned trianglesLastFrame;
    unsigned spritesLastFrame;
    unsigned textureBytes;      // memory held by live textures
    unsigned textureCount;
};
void MarniSW_GetStats(MarniSWStats* out);

// Triangles drawn since the last call (then reset): the [PERF] line.
unsigned MarniSW_TakeTriangleCount(void);
