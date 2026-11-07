// BigAlloc.cpp - one reserved block for the game's recurring 256 KB temporaries.
//
// Room loads and camera cuts build 256x256 RGBA pictures (effect pages, model
// skins, weapon sheets) in short-lived 256 KB buffers. On an 8 MB heap those
// repeated big blocks fragment memory until one of them cannot be found any
// more. A single block reserved at boot serves them instead; a second request
// while it is taken, or a bigger one, falls back to malloc.
#include "re1_target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
const size_t kArena = 256 * 1024;
const size_t kMinUse = 96 * 1024;   // smaller requests never take the block
void* s_arena = nullptr;
bool  s_busy = false;
size_t s_heldSize = 0;              // size of the request holding the block
}

extern "C" void re1port_big_init(void)
{
    if (s_arena == nullptr) s_arena = re1t_alloc_large(kArena);
}

extern "C" void* re1port_big_alloc(size_t size)
{
    if (s_arena != nullptr && !s_busy && size >= kMinUse && size <= kArena) {
        s_busy = true;
        s_heldSize = size;
        return s_arena;
    }
    void* p = malloc(size);
    if (size >= kMinUse && (p == nullptr || s_busy)) {
        // Diagnostics: the block was taken (or too small) - who, and did the
        // heap cope? A failure here is a skipped unpack or picture.
        char line[160];
        snprintf(line, sizeof(line), "[MEM] big alloc %u KB from the heap (block %s%u KB): %s,"
                 " heap %ld KB, largest %ld KB\n", (unsigned)(size / 1024),
                 s_busy ? "held by " : "free, ", (unsigned)(s_busy ? s_heldSize / 1024 : kArena / 1024),
                 p != nullptr ? "ok" : "FAILED", re1t_heap_free() / 1024, re1t_heap_largest() / 1024);
        re1t_log(line);
    }
    return p;
}

extern "C" void re1port_big_free(void* p)
{
    if (p == nullptr) return;
    if (p == s_arena) { s_busy = false; return; }
    free(p);
}
