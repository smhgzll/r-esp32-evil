// PortMenu.cpp - the port's own menu (r-esp32-evil).
//
// Opens anywhere - title, movies, the game's menus, play - over a frozen
// game: while it is up re1_game_step does not run the game at all, and each
// tick shows the last game frame dimmed with the menu on it (MarniSW_Overlay*).
// It is drawn at the panel's own pixel size in the game's 8x8 font.
//
// Open / close:  browser M key or Menu button (RE1_PAD_PORT_MENU),
//                board or gamepad OPTIONS + INVENTORY together.
// In the menu:   UP/DOWN row, LEFT/RIGHT change, ACTION (C / Enter) select,
//                RUN or OPTIONS (V / Esc) close.
//
// The game sees no input while the menu is up, nor until every key and
// button that was held is released (re1port_input_muted), so the key that
// closed the menu does not also act in the game.
//
// Rows that need a character in a room (cheats, items) are dimmed outside
// play: "play" means the game loop ran a gameplay frame just before the menu
// opened (g_re1portGameplayFrames, DebugMenu.cpp). The room tours work from
// anywhere: from the title they start a new game as Chris.
#include "MarniSW.h"
#include "system/AssetPath.h"   // GetSaveRoot
#include "re1_remote.h"
#include "re1_target.h"

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

extern "C" float re1port_text_scale(int menuFont);
extern "C" void  re1port_set_text_scale(int menuFont, float scale);
extern "C" int   re1port_cheat(int which);        // 0 invincible, 1 invisible, 2 FPS
extern "C" void  re1port_set_cheat(int which, int on);
extern "C" int   re1port_give_item(int id, int qty);
extern "C" void  re1port_full_heal(void);
extern "C" int   re1port_key_down(int vk);        // Platform.cpp

extern int g_playingGameFlag;                     // 1 from game_start until the title
// Save / load by command (the decomp's debug quick access: the same file image
// as the typewriter's save, savedat<N>.dat in the save folder).
void DebugQuick_SaveSlot(int slot);
extern int g_debugOpenLoadScreenFlag;
extern int g_debugLoadSlot;

#if defined(RE1_PORT_BENCH)
extern int g_re1portGameplayFrames;               // DebugMenu.cpp: gameplay frames so far
extern int g_re1benchActive;
void re1bench_request(int mode);                  // 1 = tour from this room, 2 = auto tour
void re1bench_goto(int hexRoom, int mode);        // the tour at a room (0 = the first)
void re1bench_stop(void);
void re1bench_camera(int n);                      // ...and its n-th camera (1-based)
#endif

namespace {

// Pad bits (docs/GAMEPAD_INPUT.md in the decomp) and Win32 key codes.
const uint32_t kPadUp = 0x1, kPadDown = 0x2, kPadLeft = 0x4, kPadRight = 0x8;
const uint32_t kPadAction = 0x100, kPadRun = 0x200, kPadInventory = 0x800, kPadOptions = 0x4000;
const int kVkUp = 0x26, kVkDown = 0x28, kVkLeft = 0x25, kVkRight = 0x27;
const int kVkEnter = 0x0D, kVkEsc = 0x1B, kVkSpace = 0x20, kVkC = 'C', kVkV = 'V', kVkM = 'M';

enum : unsigned {
    IN_UP = 1, IN_DOWN = 2, IN_LEFT = 4, IN_RIGHT = 8,
    IN_OK = 16, IN_BACK = 32, IN_TOGGLE = 64,
};

// RGB565
const uint16_t kTitle = 0xFFE0, kText = 0xC618, kSel = 0xFFFF, kDim = 0x52AA, kValue = 0x87FF;
const uint16_t kBar = 0x2967, kRule = 0x4208, kGood = 0x07E0, kBad = 0xF9E7;

struct Item { unsigned char id, qty; const char* name; };
// Names fit the row (12 characters at most).
const Item kItems[] = {
    { 0x02, 15, "BERETTA" },      { 0x03,  7, "SHOTGUN" },      { 0x05,  6, "COLT PYTHON" },
    { 0x06, 100, "FLAMETHROWER" }, { 0x0A, 10, "R. LAUNCHER" },  { 0x07,  6, "BAZOOKA" },
    { 0x0B, 15, "CLIP" },         { 0x0C,  7, "SHELLS" },       { 0x0E,  6, "MAGNUM AMMO" },
    { 0x10,  6, "EXPLOSIVE R." }, { 0x11,  6, "ACID ROUNDS" },  { 0x12,  6, "FLAME ROUNDS" },
    { 0x0F, 100, "FUEL" },        { 0x41,  1, "F.AID SPRAY" },  { 0x42,  1, "SERUM" },
    { 0x43,  1, "RED HERB" },     { 0x44,  1, "GREEN HERB" },   { 0x45,  1, "BLUE HERB" },
    { 0x2F,  3, "INK RIBBON" },   { 0x30,  1, "LIGHTER" },      { 0x31,  1, "LOCKPICK" },
    { 0x33,  1, "SWORD KEY" },    { 0x34,  1, "ARMOR KEY" },    { 0x35,  1, "SHIELD KEY" },
    { 0x36,  1, "HELMET KEY" },   { 0x37,  1, "MASTER KEY" },   { 0x29,  1, "WIND CREST" },
    { 0x2C,  1, "MOON CREST" },   { 0x2D,  1, "STAR CREST" },   { 0x2E,  1, "SUN CREST" },
    { 0x1F,  1, "EMBLEM" },       { 0x24,  1, "WOLF MEDAL" },   { 0x25,  1, "EAGLE MEDAL" },
    { 0x27,  1, "BATTERY" },      { 0x28,  1, "MO DISK" },      { 0x26,  1, "HERBICIDE" },
};
const int kItemCount = (int)(sizeof(kItems) / sizeof(kItems[0]));

enum Row {
    ROW_FPS, ROW_TEXT, ROW_MENU_TEXT, ROW_INVINCIBLE, ROW_INVISIBLE, ROW_ITEM, ROW_HEAL,
    ROW_TOUR, ROW_AUTO_TOUR, ROW_CLOSE, ROW_COUNT
};

bool        s_open = false;
bool        s_muted = false;        // input hidden from the game until all is released
bool        s_inGame = false;       // a room was being played when the menu opened
unsigned    s_prev = 0;             // last tick's inputs (edge detection)
bool        s_comboWasDown = false; // OPTIONS + INVENTORY, held across ticks
int         s_cursor = 0;
int         s_item = 0;
const char* s_msg = nullptr;
uint16_t    s_msgColor = 0;
int         s_msgTicks = 0;
int         s_seenFrames = -1, s_quietTicks = 1000;   // gameplay frames seen / ticks since one

uint32_t RawPad(void)
{
    return re1t_pad_mask() | re1_remote_pad_mask();
}

unsigned ReadInputs(void)
{
    const uint32_t pad = RawPad();
    unsigned in = 0;
    if ((pad & kPadUp) || re1port_key_down(kVkUp)) in |= IN_UP;
    if ((pad & kPadDown) || re1port_key_down(kVkDown)) in |= IN_DOWN;
    if ((pad & kPadLeft) || re1port_key_down(kVkLeft)) in |= IN_LEFT;
    if ((pad & kPadRight) || re1port_key_down(kVkRight)) in |= IN_RIGHT;
    if ((pad & kPadAction) || re1port_key_down(kVkEnter) || re1port_key_down(kVkC) ||
        re1port_key_down(kVkSpace)) in |= IN_OK;
    if ((pad & (kPadRun | kPadOptions)) || re1port_key_down(kVkEsc) || re1port_key_down(kVkV))
        in |= IN_BACK;
    if ((pad & RE1_PAD_PORT_MENU) || re1port_key_down(kVkM)) in |= IN_TOGGLE;
    return in;
}

bool AnythingHeld(void)
{
    if (RawPad() != 0) return true;
    for (int vk = 1; vk < 256; ++vk)
        if (re1port_key_down(vk)) return true;
    return false;
}

const char* ScaleName(float s)
{
    return s < 1.25f ? "ORIGINAL" : (s < 1.75f ? "LARGE" : "X-LARGE");
}

float ScaleStep(float s, int step)
{
    static const float kScales[] = { 1.0f, 1.5f, 2.0f };
    int i = s < 1.25f ? 0 : (s < 1.75f ? 1 : 2);
    i = (i + 3 + step) % 3;
    return kScales[i];
}

// The cheats and items need a character in a room. The tours do not: from
// the title they start a new game for themselves (TitleScreen.cpp).
bool RowNeedsGame(int row)
{
    return row >= ROW_INVINCIBLE && row <= ROW_HEAL;
}

bool IsTourRow(int row)
{
    return row == ROW_TOUR || row == ROW_AUTO_TOUR;
}

void Message(const char* text, uint16_t color)
{
    s_msg = text;
    s_msgColor = color;
    s_msgTicks = 60;
}

void Open(void)
{
    if (!MarniSW_OverlayBegin()) return;
    s_open = true;
    s_muted = true;
    s_inGame = s_quietTicks < 15;
#if defined(RE1_PORT_BENCH)
    if (g_re1benchActive) s_inGame = false;   // the tour owns the game; end it there first
#endif
    s_msg = nullptr;
    fprintf(stderr, "[MENU] open (%s)\n", s_inGame ? "in play" : "outside play");
}

void Close(void)
{
    s_open = false;
    MarniSW_OverlayEnd();
    fprintf(stderr, "[MENU] closed\n");
}

void Activate(int row, int step)
{
    const bool toggle = step != 0;
    if (RowNeedsGame(row) && !s_inGame) {
        if (step != 0 || row != ROW_ITEM) Message("IN A GAME ONLY", kBad);
        return;
    }
    switch (row) {
    case ROW_FPS:
        re1port_set_cheat(2, !re1port_cheat(2));
        break;
    case ROW_TEXT:
    case ROW_MENU_TEXT: {
        const int menu = row == ROW_MENU_TEXT;
        re1port_set_text_scale(menu, ScaleStep(re1port_text_scale(menu), step ? step : 1));
        break;
    }
    case ROW_INVINCIBLE:
    case ROW_INVISIBLE:
        re1port_set_cheat(row - ROW_INVINCIBLE, !re1port_cheat(row - ROW_INVINCIBLE));
        break;
    case ROW_ITEM:
        if (toggle) {
            s_item = (s_item + kItemCount + step) % kItemCount;
        } else if (re1port_give_item(kItems[s_item].id, kItems[s_item].qty)) {
            Message("ADDED TO INVENTORY", kGood);
        } else {
            Message("NO FREE SLOT", kBad);
        }
        break;
    case ROW_HEAL:
        if (!toggle) { re1port_full_heal(); Message("HEALTH RESTORED", kGood); }
        break;
#if defined(RE1_PORT_BENCH)
    case ROW_TOUR:
    case ROW_AUTO_TOUR:
        if (!toggle) {
            re1bench_request(row == ROW_TOUR ? 1 : 2);
            Close();                // the tour starts on the next game frame
        }
        break;
#endif
    case ROW_CLOSE:
        if (!toggle) Close();
        break;
    default:
        break;
    }
}

void Draw(void)
{
    uint16_t* fb = MarniSW_OverlayFrame();
    if (fb == nullptr) return;
    const int W = RE1_FB_W;
    MarniSW_OverlayText(fb, 4, 2, "PORT MENU", kTitle);
    MarniSW_OverlayFill(fb, 2, 12, W - 4, 1, kRule);

    char value[24];
    const int top = 15, pitch = 9;
    for (int r = 0; r < ROW_COUNT; ++r) {
        const int y = top + r * pitch;
        const bool sel = r == s_cursor;
        const bool off = RowNeedsGame(r) && !s_inGame;
        if (sel) MarniSW_OverlayFill(fb, 1, y - 1, W - 2, 10, kBar);
        const uint16_t col = off ? kDim : (sel ? kSel : kText);
        static const char* const kLabels[ROW_COUNT] = {
            "FPS", "TEXT SIZE", "MENU TEXT", "INVINCIBLE", "INVISIBLE", "ITEM",
            "FULL HEALTH", "ROOM TOUR", "AUTO TOUR", "CLOSE",
        };
        MarniSW_OverlayText(fb, 4, y, kLabels[r], col);
        value[0] = 0;
        switch (r) {
        case ROW_FPS:        snprintf(value, sizeof(value), "%s", re1port_cheat(2) ? "ON" : "OFF"); break;
        case ROW_TEXT:       snprintf(value, sizeof(value), "%s", ScaleName(re1port_text_scale(0))); break;
        case ROW_MENU_TEXT:  snprintf(value, sizeof(value), "%s", ScaleName(re1port_text_scale(1))); break;
        case ROW_INVINCIBLE: snprintf(value, sizeof(value), "%s", re1port_cheat(0) ? "ON" : "OFF"); break;
        case ROW_INVISIBLE:  snprintf(value, sizeof(value), "%s", re1port_cheat(1) ? "ON" : "OFF"); break;
        case ROW_ITEM:       snprintf(value, sizeof(value), "<%s>", kItems[s_item].name); break;
        default: break;
        }
        if (value[0] != 0) {
            const int len = (int)strlen(value);
            MarniSW_OverlayText(fb, W - 4 - len * 8, y, value, off ? kDim : (sel ? kValue : kText));
        }
    }

    // Footer: a message for a moment, else what the selected row does.
    const int fy = top + ROW_COUNT * pitch + 2;
    const char* foot = nullptr;
    uint16_t footCol = kDim;
    if (s_msgTicks > 0 && s_msg != nullptr) {
        foot = s_msg;
        footCol = s_msgColor;
        --s_msgTicks;
    } else if (RowNeedsGame(s_cursor) && !s_inGame) {
        foot = "IN A GAME ONLY";
    } else if (IsTourRow(s_cursor) && !s_inGame) {
        foot = g_playingGameFlag ? "STARTS AS PLAY GOES ON" : "NEW GAME AS CHRIS";
    } else if (s_cursor == ROW_ITEM) {
        foot = "ACTION: ADD IT";
    } else {
        foot = "M: CLOSE";
    }
    MarniSW_OverlayText(fb, 4, fy, foot, footCol);
    MarniSW_OverlayPresent();
}

}  // namespace

// Called by re1_game_step on every game tick, before the game runs.
// 1 = the menu is up: do not run the game this tick.
extern "C" int re1port_menu_tick(void)
{
#if defined(RE1_PORT_BENCH)
    // Was the game playing a room just now? (gameplay frames keep coming)
    if (g_re1portGameplayFrames != s_seenFrames) {
        s_seenFrames = g_re1portGameplayFrames;
        s_quietTicks = 0;
    } else if (s_quietTicks < 1000) {
        ++s_quietTicks;
    }
#endif

    const unsigned in = ReadInputs();
    const unsigned edge = in & ~s_prev;
    s_prev = in;
    const uint32_t pad = RawPad();
    const bool combo = (pad & (kPadOptions | kPadInventory)) == (kPadOptions | kPadInventory);
    const bool comboEdge = combo && !s_comboWasDown;
    s_comboWasDown = combo;

    if (!s_open) {
        if ((edge & IN_TOGGLE) || comboEdge) Open();
        if (!s_open) {
            if (s_muted && !AnythingHeld()) s_muted = false;
            return 0;
        }
        Draw();
        return 1;
    }

    if ((edge & IN_TOGGLE) || comboEdge || (edge & IN_BACK)) {
        Close();
        return 1;                       // muted until everything is released
    }
    if (edge & IN_UP) s_cursor = (s_cursor + ROW_COUNT - 1) % ROW_COUNT;
    if (edge & IN_DOWN) s_cursor = (s_cursor + 1) % ROW_COUNT;
    if (edge & IN_LEFT) Activate(s_cursor, -1);
    if (edge & IN_RIGHT) Activate(s_cursor, 1);
    if (edge & IN_OK) Activate(s_cursor, 0);
    if (s_open) Draw();
    return 1;
}

extern "C" int re1port_input_muted(void)
{
    return s_open || s_muted;
}

// Text commands from the browser link ('C' packets, re1tool.py room / cmd).
extern "C" void re1port_command(const char* line)
{
    char word[16] = {0};
    unsigned hex = 0;
    int cam = 0;
    const int n = sscanf(line, "%15s %x %d", word, &hex, &cam);
    if (n < 1) return;
#if defined(RE1_PORT_BENCH)
    if (strcmp(word, "room") == 0 && n >= 2) {
        // room <hex> [camera]: the camera is 1-based, as the band counts them.
        if (n == 3) fprintf(stderr, "[CMD] room %X camera %d\n", hex, cam);
        else fprintf(stderr, "[CMD] room %X\n", hex);
        if (s_open) Close();
        re1bench_goto((int)hex, 1);
        if (n == 3) re1bench_camera(cam);
        return;
    }
    if (strcmp(word, "cam") == 0 && n == 2) {
        int k = 0;
        sscanf(line, "%*s %d", &k);
        fprintf(stderr, "[CMD] camera %d\n", k);
        if (s_open) Close();
        re1bench_camera(k);
        return;
    }
    if (strcmp(word, "auto") == 0) {
        fprintf(stderr, "[CMD] auto tour%s\n", n == 2 ? " from a room" : "");
        if (s_open) Close();
        re1bench_goto(n == 2 ? (int)hex : 0, 2);
        return;
    }
    if (strcmp(word, "stop") == 0) {
        fprintf(stderr, "[CMD] stop\n");
        re1bench_stop();
        return;
    }
#endif
    if ((strcmp(word, "save") == 0 || strcmp(word, "load") == 0) && n >= 2) {
        // save <1-8> / load <1-8>: savedat<N>.dat, as the typewriter writes it.
        // Only in play: the save needs a game, the load runs through the game
        // loop (it fades out and restarts from the file).
        int slot = 0;
        sscanf(line, "%*s %d", &slot);
        if (slot < 1 || slot > 8 || !g_playingGameFlag || g_re1benchActive) {
            fprintf(stderr, "[CMD] %s %d: needs a game in progress (not the room tour), slot 1-8\n",
                    word, slot);
            return;
        }
        if (word[0] == 's') {
            DebugQuick_SaveSlot(slot - 1);
            fprintf(stderr, "[CMD] saved to slot %d\n", slot);
        } else {
            g_debugLoadSlot = slot - 1;
            g_debugOpenLoadScreenFlag = 1;
            fprintf(stderr, "[CMD] loading slot %d\n", slot);
        }
        return;
    }
    if (strcmp(word, "saves") == 0) {
        // What the card holds in the save folder, and what opening each slot's
        // file the way the save screen does gives.
        const char* root = GetSaveRoot();
        fprintf(stderr, "[CMD] saves in %s:\n", root);
        char dir[260];
        snprintf(dir, sizeof(dir), "%s", root);
        size_t len = strlen(dir);
        if (len > 1 && (dir[len - 1] == '/' || dir[len - 1] == '\\')) dir[len - 1] = '\0';
        if (DIR* d = opendir(dir)) {
            while (struct dirent* e = readdir(d)) fprintf(stderr, "[CMD]   entry %s\n", e->d_name);
            closedir(d);
        } else {
            fprintf(stderr, "[CMD]   cannot list %s (%s)\n", dir, strerror(errno));
        }
        for (int slot = 1; slot <= 8; ++slot) {
            char path[300];
            snprintf(path, sizeof(path), "%ssavedat%d.dat", root, slot);
            errno = 0;
            FILE* f = fopen(path, "r");
            if (f == nullptr) {
                if (errno != ENOENT) fprintf(stderr, "[CMD]   slot %d: %s\n", slot, strerror(errno));
                continue;
            }
            fseek(f, 0, SEEK_END);
            fprintf(stderr, "[CMD]   slot %d: %ld bytes\n", slot, ftell(f));
            fclose(f);
        }
        return;
    }
    if (strcmp(word, "sdtest") == 0) {
        // Write files of several sizes to a scratch folder next to the saves,
        // read them back through a fresh open and compare; then remove them.
        char dir[300];
        snprintf(dir, sizeof(dir), "%s_sdtest", GetSaveRoot());
        mkdir(dir, 0777);
        static const int kSizes[] = {512, 4096, 65536, 262144};
        uint8_t* w = (uint8_t*)malloc(262144);
        uint8_t* r = (uint8_t*)malloc(262144);
        if (w == nullptr || r == nullptr) {
            fprintf(stderr, "[CMD] sdtest: no memory\n");
            free(w); free(r);
            return;
        }
        int bad = 0;
        for (int k = 0; k < 4; ++k) {
            const int n = kSizes[k];
            uint32_t x = 0x9E3779B9u * (uint32_t)(k + 1);
            for (int i = 0; i < n; ++i) { x = x * 1664525u + 1013904223u; w[i] = (uint8_t)(x >> 24); }
            char path[340];
            snprintf(path, sizeof(path), "%s/t%d.bin", dir, n);
            const uint64_t t0 = re1t_time_us();
            FILE* f = fopen(path, "wb");
            size_t put = f ? fwrite(w, 1, n, f) : 0;
            const int closed = f ? fclose(f) : -1;
            const uint64_t t1 = re1t_time_us();
            memset(r, 0, n);
            f = fopen(path, "rb");
            size_t got = f ? fread(r, 1, n, f) : 0;
            if (f) fclose(f);
            const uint64_t t2 = re1t_time_us();
            int at = -1;
            for (int i = 0; i < (int)got && i < n; ++i) if (r[i] != w[i]) { at = i; break; }
            const bool ok = put == (size_t)n && closed == 0 && got == (size_t)n && at < 0;
            if (!ok) ++bad;
            fprintf(stderr, "[CMD] sdtest %6d B: wrote %d%s (%.0f KB/s), read %d (%.0f KB/s): %s",
                    n, (int)put, closed == 0 ? "" : " close FAILED",
                    n / 1024.0 / ((t1 - t0 + 1) / 1e6), (int)got,
                    n / 1024.0 / ((t2 - t1 + 1) / 1e6), ok ? "OK" : "BAD");
            if (at >= 0) fprintf(stderr, " - first difference at byte %d: wrote 0x%02x, read 0x%02x", at, w[at], r[at]);
            fprintf(stderr, "\n");
            remove(path);
        }
        rmdir(dir);
        free(w); free(r);
        fprintf(stderr, "[CMD] sdtest: %s\n", bad == 0 ? "all files read back as written"
                                                    : "the card did not keep what was written");
        return;
    }
    if (strcmp(word, "tone") == 0) {
        const int peak = re1t_audio_test_tone(2000);
        fprintf(stderr, "[CMD] tone: 2 s of 440 Hz to the DAC; the game's sound peaked at %d since "
                        "the last check (0 = the game was silent)\n", peak);
        return;
    }
    fprintf(stderr, "[CMD] unknown: %s (room <hex> [camera] | cam <n> | auto [hex] | stop | save <n> | load <n> | saves | sdtest | tone)\n",
            line);
}
