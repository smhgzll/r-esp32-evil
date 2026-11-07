// newdelete.cpp - operator new that returns NULL instead of aborting.
//
// The firmware is built without C++ exceptions, so libstdc++'s operator new
// turns an allocation failure into abort() - a reboot in the middle of the
// game. The decomp checks the result of its big `new`s (texture conversion
// buffers, VRAM pages) the way the 1997 MSVC code did, so returning NULL
// degrades an out-of-memory moment into a missing texture instead. The game
// library is compiled with -fcheck-new so those checks are not optimised out.
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <new>
#include <stdlib.h>

static void* Alloc(size_t n)
{
    if (n == 0) n = 1;
    void* p = malloc(n);
    if (p == nullptr) {
        static unsigned s_failures = 0;
        if (s_failures++ < 8) {
            ESP_LOGW("re1", "new(%u) failed: free %u, largest block %u", (unsigned)n,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
    }
    return p;
}

void* operator new(size_t n) { return Alloc(n); }
void* operator new[](size_t n) { return Alloc(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return Alloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return Alloc(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { free(p); }
