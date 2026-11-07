// re1_prof.h - r-esp32-evil: where a frame's time goes, measured on the target.
//
// Exclusive buckets: time is charged to the innermost open scope only, so a
// model batch's rasterisation is not counted again under the model pipeline
// that issued it. GameMain prints the per-frame averages every 5 s ([PERF]).
//
//   RE1_PROF_SCOPE(RE1_PROF_TMD);   // until the end of the enclosing block
#pragma once

#include <stdint.h>

enum {
    RE1_PROF_IDLE = 0,      // outside the game tick (not printed)
    RE1_PROF_GAME,          // the rest of the game tick
    RE1_PROF_SCRIPTS,       // room scripts and events
    RE1_PROF_ENEMIES,       // update_entities: monster AI and animation
    RE1_PROF_PLAYER,        // the player's animation and movement
    RE1_PROF_MODELS,        // posing and queuing models (render_entity, room objects)
    RE1_PROF_TMD,           // FlushTmdObjects: transform the models' vertices
    RE1_PROF_TMDASM,        // ...cull, light and assemble their triangles
    RE1_PROF_TMDSORT,       // ...depth-sort their triangles
    RE1_PROF_TMDSUB,        // ...hand them to the rasteriser in order
    RE1_PROF_SPRSORT,       // the sprite queue: sort and dispatch
    RE1_PROF_TEXT,          // the text layer: layout and glyphs
    RE1_PROF_RASTER,        // triangle rasterisation
    RE1_PROF_SPRITE,        // sprite drawing: background, masks, HUD
    RE1_PROF_PRESENT,       // handing the frame to the panel / browser link
    RE1_PROF_COUNT
};

// Work counters: how much each stage handled, so a bucket's time can be read
// per unit ([WORK] line). Bumped with RE1_WORK(counter, n); cheap enough for
// a per-span call.
enum {
    RE1_WORK_OBJECTS = 0,   // model pieces transformed (FlushTmdObjects)
    RE1_WORK_VERTICES,      // ...their vertices
    RE1_WORK_PRIMS,         // ...their primitives looked at
    RE1_WORK_OFFSCREEN,     // ...of those, dropped as wholly off the screen
    RE1_WORK_TRIS,          // triangles handed to the rasteriser (all passes)
    RE1_WORK_BATCHES,       // DrawTriangles* calls
    RE1_WORK_TRI_PX,        // pixels the rasteriser stepped over (spans)
    RE1_WORK_SPRITES,       // sprites drawn
    RE1_WORK_SPRITE_PX,     // ...their pixels
    RE1_WORK_SPRITE_SLOW_PX,// ...of those, through the general texel/blend path
    RE1_WORK_RANGE_FLUSHES, // FlushSpriteCommandsRange calls
    RE1_WORK_TRI_LINES,     // RE1_SW_RASTER_STATS: scanlines with pixels
    RE1_WORK_RCYC_VTX,      // ...cycles: reading a triangle's vertices (batch)
    RE1_WORK_RCYC_TRI,      // ...cycles: RasterTriangle, all of it
    RE1_WORK_RCYC_LINE,     // ...cycles: of that, scanline set-up
    RE1_WORK_RCYC_PX,       // ...cycles: of that, the pixel loops
    RE1_WORK_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif
void     re1prof_enter(int bucket);
void     re1prof_leave(void);
// Copies the totals in us since the last call and resets them.
void     re1prof_take(uint64_t out[RE1_PROF_COUNT]);
const char* re1prof_name(int bucket);
extern uint32_t g_re1work[RE1_WORK_COUNT];
// Copies the work counters since the last call and resets them.
void     re1prof_take_work(uint32_t out[RE1_WORK_COUNT]);
const char* re1prof_work_name(int counter);
#ifdef __cplusplus
}

struct Re1ProfScope {
    explicit Re1ProfScope(int bucket) { re1prof_enter(bucket); }
    ~Re1ProfScope() { re1prof_leave(); }
    Re1ProfScope(const Re1ProfScope&) = delete;
    Re1ProfScope& operator=(const Re1ProfScope&) = delete;
};
#define RE1_PROF_CAT2(a, b) a##b
#define RE1_PROF_CAT(a, b) RE1_PROF_CAT2(a, b)
#define RE1_PROF_SCOPE(bucket) Re1ProfScope RE1_PROF_CAT(re1prof_scope_, __LINE__)(bucket)
#endif
#define RE1_WORK_ADD(counter, n) (g_re1work[(counter)] += (uint32_t)(n))
