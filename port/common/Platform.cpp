// Platform.cpp - platform.h for every r-esp32-evil target.
//
// Counterpart of decomp/src/platform/linux/platform.cpp, minus SDL: keys come
// from the browser bridge (or the board's own buttons, which go through the pad
// mask instead), time and memory from re1_target.h, files from POSIX/newlib -
// ESP-IDF's VFS gives FATFS and the semihosting tree the same stdio/dirent API
// the host has.
#include "platform/platform.h"
#include "re1_target.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Keyboard
//
// The port's key code IS the Win32 virtual-key code (the bridge sends VKs), so
// no translation table is needed. The two GetAsyncKeyState bits are kept
// exactly like the Linux build: 0x8000 held, 0x0001 pressed since last query.
// ---------------------------------------------------------------------------
static volatile unsigned char s_keyDown[256];
extern "C" int re1port_input_muted(void);   // PortMenu.cpp
static unsigned char s_keyPrev[256];

void plat_key_event(int keycode, BOOL down)
{
    if (keycode < 0 || keycode > 255) return;
    s_keyDown[keycode] = down ? 1 : 0;
}

int plat_key_state(int vk)
{
    if (vk < 0 || vk > 255) return 0;
    // While the port menu is up (or its keys are still held after it closed)
    // the game sees no keys; the edge history keeps running so nothing fires
    // as a fresh press afterwards.
    int down = re1port_input_muted() ? 0 : s_keyDown[vk];
    int edge = (down && !s_keyPrev[vk]) ? 1 : 0;
    s_keyPrev[vk] = (unsigned char)down;
    return (down ? 0x8000 : 0) | edge;
}

// The port menu's own view of the keyboard: held state, no side effects.
extern "C" int re1port_key_down(int vk)
{
    return (vk >= 0 && vk <= 255) ? s_keyDown[vk] : 0;
}

void plat_key_flush(void)
{
    for (int vk = 0; vk < 256; vk++) plat_key_state(vk);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
DWORD plat_time_ms(void)
{
    return (DWORD)re1t_time_ms();
}

// ---------------------------------------------------------------------------
// Path normalisation (see re1t_fs_case_mode)
// ---------------------------------------------------------------------------
static void FixSeparators(char* s)
{
    for (; *s; ++s) if (*s == '\\') *s = '/';
}

// Collapse "./" components and duplicate slashes: FATFS and the semihosting
// host both accept them, but they make the case resolution below miss.
static void Tidy(char* s)
{
    char* out = s;
    const char* in = s;
    while (*in) {
        if (in[0] == '/' && in[1] == '/') { ++in; continue; }
        if (in[0] == '/' && in[1] == '.' && (in[2] == '/' || in[2] == '\0')) { in += 2; continue; }
        *out++ = *in++;
    }
    *out = '\0';
}

// Mode 2: walk the existing components and match each case-insensitively.
static void ResolveCaseInsensitive(char* path, size_t size)
{
    char out[1024];
    size_t outLen = 0;
    const char* p = path;
    if (*p == '/') { out[outLen++] = '/'; ++p; }
    out[outLen] = '\0';

    bool parentExists = true;
    while (*p) {
        const char* slash = strchr(p, '/');
        size_t compLen = slash ? (size_t)(slash - p) : strlen(p);
        char comp[256];
        if (compLen >= sizeof(comp)) return;
        memcpy(comp, p, compLen);
        comp[compLen] = '\0';

        const char* chosen = comp;
        char found[256];
        if (parentExists && compLen > 0 && strcmp(comp, ".") != 0 && strcmp(comp, "..") != 0) {
            DIR* d = opendir(outLen ? out : ".");
            bool hit = false;
            if (d) {
                struct dirent* e;
                while ((e = readdir(d)) != NULL) {
                    if (strcmp(e->d_name, comp) == 0) { hit = true; break; }
                    if (!hit && strcasecmp(e->d_name, comp) == 0) {
                        strncpy(found, e->d_name, sizeof(found) - 1);
                        found[sizeof(found) - 1] = '\0';
                        chosen = found;
                        hit = true;
                    }
                }
                closedir(d);
            }
            if (!hit) parentExists = false;
        }
        size_t cl = strlen(chosen);
        if (outLen + cl + 2 >= sizeof(out)) return;
        memcpy(out + outLen, chosen, cl);
        outLen += cl;
        if (slash) out[outLen++] = '/';
        out[outLen] = '\0';
        p = slash ? slash + 1 : p + compLen;
    }
    strncpy(path, out, size - 1);
    path[size - 1] = '\0';
}

const char* plat_normalize_path(const char* path, char* buffer, size_t size)
{
    if (buffer == NULL || size == 0) return "";
    if (path == NULL) { buffer[0] = '\0'; return buffer; }
    strncpy(buffer, path, size - 1);
    buffer[size - 1] = '\0';
    FixSeparators(buffer);
    Tidy(buffer);

    switch (re1t_fs_case_mode()) {
    case 1: {
        // Lower-case below the data root (the tree re1tool.py writes).
        const char* root = re1t_data_root();
        size_t rl = strlen(root);
        char* tail = buffer;
        if (strncmp(buffer, root, rl) == 0) tail = buffer + rl;
        else if (buffer[0] == '/') break;   // some other absolute path: leave it
        for (char* c = tail; *c; ++c) *c = (char)tolower((unsigned char)*c);
        break;
    }
    case 2:
        ResolveCaseInsensitive(buffer, size);
        break;
    default:
        break;
    }
    return buffer;
}

BOOL plat_exe_dir(char* out, size_t size)
{
    // Every relative asset / save / config path is anchored here, so the data
    // root plays the role of the executable's directory.
    if (out == NULL || size == 0) return FALSE;
    const char* root = re1t_data_root();
    if (strlen(root) >= size) { out[0] = '\0'; return FALSE; }
    strcpy(out, root);
    return TRUE;
}

BOOL plat_path_is_absolute(const char* path)
{
    if (path == NULL || path[0] == '\0') return FALSE;
    if (path[0] == '/') return TRUE;
    if (path[0] == '\\' && path[1] == '\\') return TRUE;
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
        && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
        return TRUE;
    }
    return FALSE;
}

BOOL plat_mkdir(const char* path)
{
    char norm[512];
    plat_normalize_path(path, norm, sizeof(norm));
    if (mkdir(norm, 0755) == 0) return TRUE;
    if (errno == EEXIST) return TRUE;
    struct stat st;
    return (stat(norm, &st) == 0 && S_ISDIR(st.st_mode)) ? TRUE : FALSE;
}

BOOL plat_dir_list(const char* path, PlatDirVisit visit, void* user)
{
    char norm[512];
    const char* dirPath = plat_normalize_path(path, norm, sizeof(norm));
    DIR* d = opendir(dirPath);
    if (d == NULL) return FALSE;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        BOOL isDir = FALSE;
#if defined(DT_DIR)
        isDir = (e->d_type == DT_DIR) ? TRUE : FALSE;
        if (e->d_type == DT_UNKNOWN)
#endif
        {
            char full[768];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", dirPath, e->d_name);
            isDir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) ? TRUE : FALSE;
        }
        visit(e->d_name, isDir, user);
    }
    closedir(d);
    return TRUE;
}

BOOL plat_file_write(const char* path, const void* data, size_t size)
{
    char norm[512];
    FILE* f = fopen(plat_normalize_path(path, norm, sizeof(norm)), "wb");
    if (f == NULL) return FALSE;
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return (written == size) ? TRUE : FALSE;
}

unsigned int plat_file_mtime(const char* path)
{
    char norm[512];
    struct stat st;
    if (stat(plat_normalize_path(path, norm, sizeof(norm)), &st) != 0) return 0;
    return (unsigned int)st.st_mtime;
}

void* plat_file_read_all(const char* path, size_t* outSize)
{
    char norm[512];
    FILE* f = fopen(plat_normalize_path(path, norm, sizeof(norm)), "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long fileSize = ftell(f);
    if (fileSize < 0) { fclose(f); return NULL; }
    rewind(f);

    size_t alignedSize = (size_t)((fileSize & ~3L) + 0x10);   // original rounding
    void* buffer = malloc(alignedSize);
    if (buffer == NULL) { fclose(f); return NULL; }
    size_t got = fread(buffer, 1, (size_t)fileSize, f);
    fclose(f);
    if (got != (size_t)fileSize) { free(buffer); return NULL; }
    if (outSize != NULL) *outSize = (size_t)fileSize;
    return buffer;
}

size_t plat_readable_bytes(const void* p)
{
    return re1t_readable_bytes(p);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
void plat_debug_output(const char* s)
{
    if (s != NULL) re1t_log(s);
}

BOOL plat_is_debugger_present(void)
{
    // dbg_printf (DebugPrint.h) only prints under a debugger. On these targets
    // the console IS the debugger, so let the traces through when asked.
#if defined(RE1_VERBOSE_DEBUG)
    return TRUE;
#else
    return FALSE;
#endif
}

DWORD plat_env_get(const char* name, char* buffer, DWORD size)
{
    const char* v = getenv(name);
    if (v == NULL || buffer == NULL || size == 0) return 0;
    size_t n = strlen(v);
    if (n >= (size_t)size) n = (size_t)size - 1;
    memcpy(buffer, v, n);
    buffer[n] = '\0';
    return (DWORD)n;
}

void plat_audio_probe_and_cache_volume(void) {}
void plat_audio_restore_volume(void) {}

void plat_fatal(const char* message)
{
    re1t_fatal(message ? message : "(fatal)");
}

void plat_window_destroy(HWND) {}
void plat_cursor_show(BOOL) {}

// ---------------------------------------------------------------------------
// Display / lifecycle requests (title OPTIONS menu)
// ---------------------------------------------------------------------------
extern "C" BOOL g_re1port_quit_requested;
BOOL g_re1port_quit_requested = FALSE;

void plat_display_request_apply(void)
{
    // Fixed panel: the back buffer never changes size.
}

void plat_request_restart(void)
{
    g_re1port_quit_requested = TRUE;
}

void plat_request_quit(void)
{
    g_re1port_quit_requested = TRUE;
}

BOOL plat_display_desktop_size(int* width, int* height)
{
    if (width)  *width = RE1_LOGICAL_W;
    if (height) *height = RE1_LOGICAL_H;
    return TRUE;
}

BOOL plat_single_instance_check(void)
{
    return TRUE;
}

void crashlog_install(void) {}

void crashlog_mark(const char* step)
{
    (void)step;
}
