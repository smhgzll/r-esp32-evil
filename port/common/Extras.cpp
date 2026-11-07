// Extras.cpp - state behind OPTIONS > EXTRAS (decomp GameOptions.cpp,
// RE1_PORT_EXTRAS): text size, invincible, invisible, give item, full health.
//
// Text size and the FPS counter are saved to config.ini ([Port] TextScale /
// MenuTextScale / ShowFps); the cheats last until the game is switched off.
#include "Globals.h"
#include "game/Types.h"
#include "platform/platform.h"

#include "MarniSW.h"
#include "re1_target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" int g_re1portInvisible;
int g_re1portInvisible = 0;          // read by the enemy sight checks (EntityCommon.cpp)
static int s_invincible = 0;

static short MaxHealth(void)
{
    // The game's own start value (GameStart.cpp): 140 for Chris, 96 for Jill.
    return (short)((g_playerEntity.id & 1) * -44 + 140);
}

// ---------------------------------------------------------------------------
// [Port] keys in config.ini: rewrite them in place, keep everything else.
// ---------------------------------------------------------------------------
static void SavePortKey(const char* key, float value)
{
    char path[512], norm[512];
    snprintf(path, sizeof(path), "%s/config.ini", re1t_data_root());
    plat_normalize_path(path, norm, sizeof(norm));

    std::vector<std::string> lines;
    if (FILE* f = fopen(norm, "r")) {
        char buf[512];
        while (fgets(buf, sizeof(buf), f) != NULL) {
            std::string l(buf);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            lines.push_back(l);
        }
        fclose(f);
    }
    char kv[64];
    snprintf(kv, sizeof(kv), "%s=%.2f", key, value);
    const size_t kl = strlen(key);
    int section = -1, sectionEnd = -1;
    bool done = false;
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        const std::string& l = lines[i];
        if (!l.empty() && l[0] == '[') {
            if (section >= 0 && sectionEnd < 0) sectionEnd = (int)i;
            if (strncasecmp(l.c_str(), "[Port]", 6) == 0) section = (int)i;
            continue;
        }
        if (section >= 0 && sectionEnd < 0 && strncasecmp(l.c_str(), key, kl) == 0 &&
            l.size() > kl && l[kl] == '=') {
            lines[i] = kv;
            done = true;
        }
    }
    if (!done) {
        if (section < 0) {
            lines.push_back("");
            lines.push_back("[Port]");
            lines.push_back(kv);
        } else {
            lines.insert(lines.begin() + (sectionEnd < 0 ? (int)lines.size() : sectionEnd), kv);
        }
    }
    if (FILE* f = fopen(norm, "w")) {
        for (const std::string& l : lines) fprintf(f, "%s\n", l.c_str());
        fclose(f);
    }
}

// ---------------------------------------------------------------------------
// Hooks for GameOptions.cpp
// ---------------------------------------------------------------------------
extern "C" float re1port_text_scale(int menuFont)
{
    float msg = 1.0f, menu = 1.0f;
    MarniSW_GetTextScale(&msg, &menu);
    return menuFont ? menu : msg;
}

extern "C" void re1port_set_text_scale(int menuFont, float scale)
{
    float msg = 1.0f, menu = 1.0f;
    MarniSW_GetTextScale(&msg, &menu);
    if (menuFont) menu = scale; else msg = scale;
    MarniSW_SetTextScale(msg, menu);
    SavePortKey(menuFont ? "MenuTextScale" : "TextScale", scale);
}

// which: 0 invincible, 1 invisible, 2 FPS counter (saved as [Port] ShowFps).
extern "C" int re1port_cheat(int which)
{
    if (which == 2) return MarniSW_GetShowFps();
    return which == 0 ? s_invincible : g_re1portInvisible;
}

extern "C" void re1port_set_cheat(int which, int on)
{
    if (which == 2) {
        MarniSW_SetShowFps(on);
        SavePortKey("ShowFps", on ? 1.0f : 0.0f);
        fprintf(stderr, "[RE1] extras: FPS counter %s\n", on ? "on" : "off");
        return;
    }
    if (which == 0) s_invincible = on ? 1 : 0;
    else g_re1portInvisible = on ? 1 : 0;
    fprintf(stderr, "[RE1] extras: invincible %d, invisible %d\n", s_invincible, g_re1portInvisible);
}

extern "C" int re1port_give_item(int id, int qty)
{
    unsigned char* slots = (unsigned char*)g_ItemSlotsPointer;
    if (slots == NULL) return 0;
    const int capacity = ((g_playerEntity.id & 3) == CHAR_JILL) ? 8 : 6;
    for (int i = 0; i < capacity; ++i) {
        if (slots[i * 2] == ITEM_NONE) {
            slots[i * 2] = (unsigned char)id;
            slots[i * 2 + 1] = (unsigned char)qty;
            LoadHeldItemsImages();      // as the debug menu's inventory editor does
            return 1;
        }
    }
    return 0;
}

extern "C" void re1port_full_heal(void)
{
    if (g_playerEntity.health < MaxHealth()) g_playerEntity.health = MaxHealth();
}

// Called once per game tick (GameMain.cpp).
extern "C" void re1port_extras_tick(void)
{
    if (s_invincible && g_playerEntity.health > 0 && g_playerEntity.health < MaxHealth())
        g_playerEntity.health = MaxHealth();
}
