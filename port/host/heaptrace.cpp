// heaptrace.cpp - optional host-only allocation tracer (-DRE1_HOST_HEAPTRACE).
//
// Wraps malloc/calloc/realloc/free and records live bytes per call site, so
// the board's PSRAM budget can be tuned on the PC. RE1_HEAPTRACE=1 at run time
// prints the 25 largest live call sites at exit (resolve with addr2line -e).
#if defined(RE1_HOST_HEAPTRACE)
#include <dlfcn.h>
#include <execinfo.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void* __libc_malloc(size_t);
extern "C" void* __libc_calloc(size_t, size_t);
extern "C" void* __libc_realloc(void*, size_t);
extern "C" void  __libc_free(void*);

namespace {
const int kSites = 4096;
struct Site { void* pc; long live; long peak; long count; long atPeak; };
Site s_sites[kSites];
pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
long s_total = 0, s_totalPeak = 0;
struct Hdr { size_t size; int site; int magic; };
const int kMagic = 0x52453148;

int SiteFor(void* pc)
{
    unsigned h = ((unsigned)(uintptr_t)pc >> 2) % kSites;
    for (int i = 0; i < kSites; ++i) {
        int k = (h + i) % kSites;
        if (s_sites[k].pc == pc) return k;
        if (s_sites[k].pc == nullptr) { s_sites[k].pc = pc; return k; }
    }
    return 0;
}

void* Track(void* raw, size_t size, void* pc)
{
    if (raw == nullptr) return nullptr;
    Hdr* h = (Hdr*)raw;
    pthread_mutex_lock(&s_lock);
    int s = SiteFor(pc);
    s_sites[s].live += (long)size;
    s_sites[s].count++;
    s_total += (long)size;
    if (s_total > s_totalPeak) {
        s_totalPeak = s_total;
        if (size >= 4096) for (int k = 0; k < kSites; ++k) s_sites[k].atPeak = s_sites[k].live;
    }
    if (s_sites[s].live > s_sites[s].peak) s_sites[s].peak = s_sites[s].live;
    pthread_mutex_unlock(&s_lock);
    h->size = size;
    h->site = s;
    h->magic = kMagic;
    return (char*)raw + 16;
}

void Untrack(void* p)
{
    Hdr* h = (Hdr*)((char*)p - 16);
    pthread_mutex_lock(&s_lock);
    s_sites[h->site].live -= (long)h->size;
    s_total -= (long)h->size;
    pthread_mutex_unlock(&s_lock);
}

struct Reporter {
    ~Reporter() {
        if (getenv("RE1_HEAPTRACE") == nullptr) return;
        Site sorted[kSites];
        memcpy(sorted, s_sites, sizeof(sorted));
        qsort(sorted, kSites, sizeof(Site), [](const void* a, const void* b) {
            long x = ((const Site*)a)->atPeak, y = ((const Site*)b)->atPeak;
            return x < y ? 1 : (x > y ? -1 : 0);
        });
        fprintf(stderr, "[HEAP] total peak %ld KB, live at exit %ld KB\n", s_totalPeak / 1024,
                s_total / 1024);
        if (getenv("RE1_HEAPTRACE_LIVE")) {
            for (int i = 0; i < kSites; ++i)
                if (s_sites[i].pc && s_sites[i].live > 0)
                    fprintf(stderr, "[LIVE] %ld %ld %p\n", s_sites[i].live, s_sites[i].count,
                            s_sites[i].pc);
        }
        fprintf(stderr, "[HEAP] peak-live by call site:\n");
        for (int i = 0; i < 25 && sorted[i].pc; ++i)
            fprintf(stderr, "[HEAP] %8ld KB atpeak %8ld KB live %6ld allocs  pc=%p\n",
                    sorted[i].atPeak / 1024, sorted[i].live / 1024, sorted[i].count, sorted[i].pc);
    }
} s_reporter;
}  // namespace

extern "C" void* malloc(size_t n)
{
    return Track(__libc_malloc(n + 16), n, __builtin_return_address(0));
}
extern "C" void* calloc(size_t a, size_t b)
{
    void* p = __libc_calloc(1, a * b + 16);
    return Track(p, a * b, __builtin_return_address(0));
}
extern "C" void free(void* p)
{
    if (p == nullptr) return;
    Untrack(p);
    __libc_free((char*)p - 16);
}
extern "C" void* realloc(void* p, size_t n)
{
    if (p == nullptr) return malloc(n);
    Hdr* h = (Hdr*)((char*)p - 16);
    size_t old = h->size;
    void* q = Track(__libc_malloc(n + 16), n, __builtin_return_address(0));
    if (q) memcpy(q, p, old < n ? old : n);
    free(p);
    return q;
}
// operator new goes through malloc in libstdc++, so it is covered, but its
// return address would be libstdc++'s: record the caller of new instead.
void* operator new(size_t n) { return Track(__libc_malloc(n + 16), n, __builtin_return_address(0)); }
void* operator new[](size_t n) { return Track(__libc_malloc(n + 16), n, __builtin_return_address(0)); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
#endif
