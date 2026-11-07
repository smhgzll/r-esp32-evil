// Prof.cpp - exclusive time buckets (re1_prof.h). Game thread only.
#include "re1_prof.h"
#include "re1_target.h"

#include <string.h>

namespace {
const int kDepth = 16;
uint64_t s_acc[RE1_PROF_COUNT];   // in re1t_prof_ticks
int      s_stack[kDepth];
int      s_top = 0;
int      s_cur = RE1_PROF_IDLE;
uint32_t s_last = 0;
bool     s_started = false;

void Charge(void)
{
    const uint32_t now = re1t_prof_ticks();
    if (s_started) s_acc[s_cur] += (uint32_t)(now - s_last);   // wraps cleanly
    s_started = true;
    s_last = now;
}
}  // namespace

extern "C" void re1prof_enter(int bucket)
{
    Charge();
    if (s_top < kDepth) s_stack[s_top] = s_cur;
    ++s_top;
    s_cur = bucket;
}

extern "C" void re1prof_leave(void)
{
    Charge();
    if (s_top > 0) --s_top;
    s_cur = s_top < kDepth ? s_stack[s_top] : RE1_PROF_GAME;
}

extern "C" void re1prof_take(uint64_t out[RE1_PROF_COUNT])
{
    Charge();
    const uint32_t perUs = re1t_prof_ticks_per_us();
    for (int b = 0; b < RE1_PROF_COUNT; ++b) out[b] = perUs > 0 ? s_acc[b] / perUs : s_acc[b];
    memset(s_acc, 0, sizeof(s_acc));
}

extern "C" uint32_t g_re1work[RE1_WORK_COUNT];
uint32_t g_re1work[RE1_WORK_COUNT];

extern "C" void re1prof_take_work(uint32_t out[RE1_WORK_COUNT])
{
    memcpy(out, g_re1work, sizeof(g_re1work));
    memset(g_re1work, 0, sizeof(g_re1work));
}

extern "C" const char* re1prof_work_name(int counter)
{
    static const char* const kNames[RE1_WORK_COUNT] = {
        "obj", "vtx", "prim", "offscr", "tri", "batch", "tripx", "spr", "sprpx", "sprslow", "flush", "lines", "rcyc_vtx", "rcyc_tri", "rcyc_line", "rcyc_px",
    };
    return (counter >= 0 && counter < RE1_WORK_COUNT) ? kNames[counter] : "?";
}

extern "C" const char* re1prof_name(int bucket)
{
    static const char* const kNames[RE1_PROF_COUNT] = {
        "idle", "game", "scripts", "enemies", "player", "models", "tmd", "tmdasm", "tmdsort", "tmdsub",
        "sprsort",
        "text", "raster", "sprite", "present",
    };
    return (bucket >= 0 && bucket < RE1_PROF_COUNT) ? kNames[bucket] : "?";
}
