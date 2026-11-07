// main_host.cpp - the PC host simulator target.
//
// Runs the exact port layer the board runs (software renderer at panel
// resolution, streamed audio, .rv1 movies, the RVL browser link) as a native
// 32-bit Linux process, so the game can be iterated on without flashing:
//
//   build/host/re1_host --data <prepared SD tree> [--port 7720]
//   python3 tools/re1tool.py web --tcp 127.0.0.1:7720     -> http://localhost:8080
//
// Test hooks (headless runs, CI):
//   --ticks N                 exit after N game ticks
//   --press VK TICK [HOLD]    press a virtual key at a tick (repeatable)
//   --capture TICK FILE.ppm   write the presented frame at a tick (repeatable)
//   --no-port                 no TCP link
#include "re1_game.h"
#include "re1_target.h"

#include "platform/platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
extern "C" void moncontrol(int mode);   // glibc gprof control (no-op without -pg)

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
static std::string s_dataRoot;
static int s_port = 7720;
static int s_maxTicks = 0;
struct Press { int vk, tick, hold; };
struct Capture { int tick; std::string path; };
static std::vector<Press> s_presses;
static std::vector<Capture> s_captures;
struct Command { int tick; std::string text; };   // --cmd
static std::vector<Command> s_commands;
extern "C" void re1port_command(const char* line);

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
static timespec s_t0;

extern "C" uint32_t re1t_time_ms(void)
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - s_t0.tv_sec) * 1000 + (t.tv_nsec - s_t0.tv_nsec) / 1000000);
}

extern "C" uint32_t re1t_prof_ticks(void) { return (uint32_t)re1t_time_us(); }
extern "C" uint32_t re1t_prof_ticks_per_us(void) { return 1; }

extern "C" uint64_t re1t_time_us(void)
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)(t.tv_sec - s_t0.tv_sec) * 1000000u + (uint64_t)((t.tv_nsec - s_t0.tv_nsec) / 1000);
}

extern "C" void re1t_sleep_ms(uint32_t ms)
{
    usleep(ms * 1000);
}

// ---------------------------------------------------------------------------
// Display: keep the last frame for --capture
// ---------------------------------------------------------------------------
static std::vector<uint16_t> s_lastFrame(RE1_FB_W * RE1_FB_H);
static std::atomic<unsigned> s_presented{0};

extern "C" void re1t_present(const uint16_t* fb, int w, int h)
{
    memcpy(s_lastFrame.data(), fb, (size_t)w * h * 2);
    s_presented++;
}

static void WritePpm(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    fprintf(f, "P6\n%d %d\n255\n", RE1_FB_W, RE1_FB_H);
    for (uint16_t c : s_lastFrame) {
        unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 31) * 255 / 31),
            (unsigned char)(((c >> 5) & 63) * 255 / 63),
            (unsigned char)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "[HOST] captured %s\n", path.c_str());
}

// ---------------------------------------------------------------------------
// Input: the browser sends keys/pad over the link; the host has no pad.
// ---------------------------------------------------------------------------
struct PadPress { uint32_t mask; int tick, hold; };
static std::vector<PadPress> s_padPresses;
static std::atomic<uint32_t> s_hostPad{0};
extern "C" uint32_t re1t_pad_mask(void) { return s_hostPad.load(); }
extern "C" int re1t_pad_present(void) { return 1; }   // the browser's virtual pad

// ---------------------------------------------------------------------------
// Audio: no device; a real-time pacing thread pulls the mix (which also feeds
// the browser link) so the FMV audio clock advances like on the board.
// ---------------------------------------------------------------------------
static std::mutex s_audioMutex;
static re1t_audio_pull_fn s_pull = nullptr;
static int s_rate = 22050;

extern "C" void re1t_audio_lock(void) { s_audioMutex.lock(); }
extern "C" void re1t_audio_unlock(void) { s_audioMutex.unlock(); }

extern "C" int re1t_audio_open(int rate, re1t_audio_pull_fn pull)
{
    s_rate = rate;
    s_pull = pull;
    std::thread([] {
        const int chunk = 512;
        std::vector<int16_t> buf(chunk * 2);
        uint64_t produced = 0;
        uint32_t start = re1t_time_ms();
        for (;;) {
            uint64_t due = (uint64_t)(re1t_time_ms() - start) * (uint64_t)s_rate / 1000;
            while (produced + chunk <= due) {
                s_pull(buf.data(), chunk);
                produced += chunk;
            }
            usleep(5000);
        }
    }).detach();
    return 1;
}

extern "C" void re1t_thread_start(const char* name, void (*fn)(void*), void* arg, int, int, int)
{
    std::thread t([fn, arg] { fn(arg); });
    pthread_setname_np(t.native_handle(), name);
    t.detach();
}

// ---------------------------------------------------------------------------
// Filesystem / memory / diagnostics
// ---------------------------------------------------------------------------
extern "C" const char* re1t_data_root(void) { return s_dataRoot.c_str(); }
extern "C" int re1t_fs_case_mode(void) { return 2; }

extern "C" size_t re1t_readable_bytes(const void* p)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (f == NULL) return 0;
    unsigned long addr = (unsigned long)p;
    size_t avail = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (addr >= lo && addr < hi) {
            if (perms[0] == 'r') avail = (size_t)(hi - addr);
            break;
        }
    }
    fclose(f);
    return avail;
}

extern "C" void* re1t_alloc_large(size_t size) { return malloc(size); }
extern "C" void  re1t_free_large(void* p) { free(p); }
extern "C" void* re1t_alloc_fast(size_t size) { return malloc(size); }

extern "C" void re1t_log(const char* s) { fputs(s, stderr); }

extern "C" void re1t_fatal(const char* s)
{
    fprintf(stderr, "[HOST] FATAL: %s\n", s);
    exit(1);
}

extern "C" long re1t_heap_largest(void) { return -1; }
extern "C" long re1t_heap_internal_free(void) { return -1; }
extern "C" void re1t_renderer_ready(void) {}
extern "C" int re1t_audio_test_tone(int) { return -1; }

static size_t s_heapPeak = 0;
extern "C" long re1t_heap_free(void)
{
    // The host has no fixed heap; report what the game holds (negative =
    // "in use") so the logs show the number that matters for the board.
    struct mallinfo2 mi = mallinfo2();
    if (mi.uordblks > s_heapPeak) s_heapPeak = mi.uordblks;
    return -(long)mi.uordblks;
}

// Task stacks: guard-paged like the Linux build, painted so the high-water
// mark can be reported (sizes the board's FreeRTOS stacks).
static unsigned char* s_stacks = nullptr;
static size_t s_stackSize = 0, s_guard = 0;
static int s_stackCount = 0;

void* plat_alloc_guarded_stacks(int count, size_t stackSize, size_t guardSize)
{
    size_t total = (size_t)count * (stackSize + guardSize) + guardSize;
    void* mem = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    for (int i = 0; i < count; i++) {
        unsigned char* slot = (unsigned char*)mem + (size_t)i * (stackSize + guardSize) + guardSize;
        if (mprotect(slot, stackSize, PROT_READ | PROT_WRITE) != 0) return NULL;
        memset(slot, 0xA5, stackSize);
    }
    s_stacks = (unsigned char*)mem + guardSize;
    s_stackSize = stackSize;
    s_guard = guardSize;
    s_stackCount = count;
    return s_stacks;
}

static void ReportStacks()
{
    for (int i = 0; i < s_stackCount; ++i) {
        unsigned char* slot = s_stacks + (size_t)i * (s_stackSize + s_guard);
        size_t untouched = 0;
        while (untouched < s_stackSize && slot[untouched] == 0xA5) untouched++;
        fprintf(stderr, "[HOST] task %d stack high-water: %zu bytes\n", i, s_stackSize - untouched);
    }
    fprintf(stderr, "[HOST] heap peak: %zu KB\n", s_heapPeak / 1024);
}

// ---------------------------------------------------------------------------
// Remote link: TCP server, one client at a time.
// ---------------------------------------------------------------------------
static int s_listenFd = -1;
static std::atomic<int> s_clientFd{-1};
static std::mutex s_sendMutex;

extern "C" int re1t_remote_enabled(void) { return s_port > 0; }

static void AcceptThread()
{
    for (;;) {
        int fd = accept(s_listenFd, nullptr, nullptr);
        if (fd < 0) { usleep(100000); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int old = s_clientFd.exchange(fd);
        if (old >= 0) close(old);
        fprintf(stderr, "[HOST] bridge connected\n");
    }
}

static void StartServer()
{
    s_listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)s_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s_listenFd, (sockaddr*)&a, sizeof(a)) != 0 || listen(s_listenFd, 1) != 0) {
        perror("[HOST] remote link");
        s_port = 0;
        return;
    }
    fprintf(stderr, "[HOST] waiting for the bridge on 127.0.0.1:%d "
                    "(python3 tools/re1tool.py web --tcp 127.0.0.1:%d)\n", s_port, s_port);
    std::thread(AcceptThread).detach();
}

extern "C" void re1t_remote_write(const void* data, size_t size)
{
    std::lock_guard<std::mutex> lock(s_sendMutex);
    int fd = s_clientFd.load();
    if (fd < 0) return;
    const char* p = (const char*)data;
    while (size > 0) {
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            close(fd);
            s_clientFd.compare_exchange_strong(fd, -1);
            return;
        }
        p += n;
        size -= (size_t)n;
    }
}

extern "C" size_t re1t_remote_read(void* data, size_t max)
{
    int fd = s_clientFd.load();
    if (fd < 0) return 0;
    ssize_t n = recv(fd, data, max, MSG_DONTWAIT);
    return n > 0 ? (size_t)n : 0;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
static void Usage()
{
    fprintf(stderr,
            "usage: re1_host --data DIR [--port N | --no-port] [--ticks N]\n"
            "                [--press VK TICK [HOLD]]... [--pad MASK TICK [HOLD]]...\n"
            "                [--capture TICK FILE.ppm]... [--cmd TICK TEXT]...\n"
            "  --cmd: a text command at that tick, as the bridge's 'C' packet\n"
            "         (e.g. --cmd 100 \"room 112 2\")\n");
}

int main(int argc, char** argv)
{
    clock_gettime(CLOCK_MONOTONIC, &s_t0);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) s_dataRoot = argv[++i];
        else if (a == "--port" && i + 1 < argc) s_port = atoi(argv[++i]);
        else if (a == "--no-port") s_port = 0;
        else if (a == "--ticks" && i + 1 < argc) s_maxTicks = atoi(argv[++i]);
        else if (a == "--press" && i + 2 < argc) {
            Press p{atoi(argv[i + 1]), atoi(argv[i + 2]), 3};
            i += 2;
            if (i + 1 < argc && argv[i + 1][0] != '-') p.hold = atoi(argv[++i]);
            s_presses.push_back(p);
        } else if (a == "--pad" && i + 2 < argc) {
            PadPress p{(uint32_t)strtoul(argv[i + 1], nullptr, 0), atoi(argv[i + 2]), 3};
            i += 2;
            if (i + 1 < argc && argv[i + 1][0] != '-') p.hold = atoi(argv[++i]);
            s_padPresses.push_back(p);
        } else if (a == "--capture" && i + 2 < argc) {
            s_captures.push_back({atoi(argv[i + 1]), argv[i + 2]});
            i += 2;
        } else if (a == "--cmd" && i + 2 < argc) {
            s_commands.push_back({atoi(argv[i + 1]), argv[i + 2]});
            i += 2;
        } else { Usage(); return 2; }
    }
    if (s_dataRoot.empty()) { Usage(); return 2; }
    char real[4096];
    if (realpath(s_dataRoot.c_str(), real) != nullptr) s_dataRoot = real;
    while (s_dataRoot.size() > 1 && s_dataRoot.back() == '/') s_dataRoot.pop_back();

    if (s_port > 0) StartServer();

    if (!re1_game_init()) return 1;
    // Profiling aid (a -pg build): RE1_PROF_FROM_TICK=N samples only from
    // tick N on, so a room's frames are not mixed with boot and title.
    const char* profFrom = getenv("RE1_PROF_FROM_TICK");
    const int profTick = profFrom != nullptr ? atoi(profFrom) : 0;
    if (profTick > 0) moncontrol(0);
    int tick = 0;
    for (;;) {
        int ran = 0;
        if (!re1_game_step(&ran)) break;
        if (!ran) { usleep(1000); continue; }
        ++tick;
        if (profTick > 0 && tick == profTick) moncontrol(1);
        for (const Press& p : s_presses) {
            if (tick == p.tick) plat_key_event(p.vk, TRUE);
            if (tick == p.tick + p.hold) plat_key_event(p.vk, FALSE);
        }
        uint32_t pad = 0;
        for (const PadPress& p : s_padPresses)
            if (tick >= p.tick && tick < p.tick + p.hold) pad |= p.mask;
        s_hostPad.store(pad);
        for (const Capture& c : s_captures) {
            if (tick == c.tick) WritePpm(c.path);
        }
        for (const Command& c : s_commands) {
            if (tick == c.tick) re1port_command(c.text.c_str());
        }
        if (s_maxTicks > 0 && tick >= s_maxTicks) break;
    }
    re1t_heap_free();
    ReportStacks();
    // Leak hunting: report what the game still holds, before shutdown frees it.
    if (getenv("RE1_HEAPTRACE_LIVE")) exit(0);
    re1_game_shutdown();
    return 0;
}
