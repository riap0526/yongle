// See keys.h. Moved from Yongle's games/emperor/keys.cpp (for Zeus): the model of the
// bindings without the game's addresses; Alt with a character eaten and the game's own posted F6/F7 are new.
#include "core/input/keys.h"

#include "core/diag/guard.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/ui/hotkey.h"

#include <cstdio>
#include <cstring>

namespace core::input::keys {

bool Never()
{
    return false;
}

bool NoOptions()
{
    return false;
}

bool NoEscape(LPARAM)
{
    return false;
}

bool NoCall(unsigned)
{
    return false;
}

namespace {

constexpr int kMaxActions = 96;

// The pause banner with the player's keys (the game's own words: "Game paused ('P' key continues)").
const char *const kPausedOneKey = "Game paused ('%s' key continues)";
const char *const kPausedTwoKeys = "Game paused ('%s' or '%s' key continues)";
const char *const kPausedNoKey = "Game paused";

unsigned RealHeldMods()
{
    auto held = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };
    return (held(VK_CONTROL) ? core::ui::kModCtrl : 0u) | (held(VK_SHIFT) ? core::ui::kModShift : 0u) |
           (held(VK_MENU) ? core::ui::kModAlt : 0u);
}

bool RealHeld(unsigned vk)
{
    return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

const KeysSystem kRealSystem{&RealHeldMods, &RealHeld};
const KeysSystem *g_system = &kRealSystem;

core::diag::Feature g_feature{"key bindings (core/input/keys)"};

const KeysGame *g_game = nullptr;
int g_count = 0;
// The keys in effect (game thread; set by the settings' callbacks), and the defaults (the game's own keys).
core::ui::Hotkey g_keys[kMaxActions][core::ui::kMaxKeys];
core::ui::Hotkey g_defaults[kMaxActions][core::ui::kMaxKeys];
int g_index[kMaxActions]; // the setting of each action
bool g_eatChar = false;   // the character of a key press we handled
bool g_rotate = false;    // the rotate key was pressed: the game's next poll of it says so
unsigned g_pollVk = 0;    // the key the game polls for the rotate action (0: none)

const Def &D(int a)
{
    return g_game->defs[a];
}

int Bound(unsigned vk, unsigned mods)
{
    for (int a = 0; a < g_count; ++a) {
        for (const core::ui::Hotkey &k : g_keys[a]) {
            if (k.vk == vk && k.mods == mods) {
                return a;
            }
        }
    }
    return -1;
}

bool Arrow(unsigned vk)
{
    return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT;
}

// One of the game's own keys (an action's default): kept from the game when it belongs to no action now. A
// character key also with Shift (the game reads 'W' as 'w'). On the editor's screens only the editor's actions count.
bool GameKey(unsigned vk, unsigned mods, bool editor)
{
    if (mods == core::ui::kModCtrl && Arrow(vk)) {
        return true; // Ctrl with the arrows: kept from the game, the keys free for players
    }
    for (int a = 0; a < g_count; ++a) {
        if ((editor && !D(a).editorToo) || D(a).send == Send::Call) {
            continue; // (a Call action's default is ours, not a key the game knows)
        }
        for (const core::ui::Hotkey &k : g_defaults[a]) {
            if (k.vk == vk && (k.mods == mods || (D(a).send == Send::Char && (mods & ~core::ui::kModShift) == 0))) {
                return true;
            }
        }
    }
    return false;
}

bool ModeVk(unsigned vk)
{
    return vk == VK_F6 || vk == VK_F7 || vk == VK_F8;
}

// F6, F7, F8 without modifiers: the games' own display mode keys (F6 800x600, F7 the other mode, F8 centres the
// window).
bool ModeKey(unsigned vk, unsigned mods)
{
    return mods == 0 && ModeVk(vk);
}

long g_fireLogs = 0;

void Fire(int a)
{
    const Def &d = D(a);
    if (++g_fireLogs <= 40) {
        core_log("keys: %s", d.info.key);
    }
    switch (d.send) {
    case Send::KeyDown: {
        const int was = *g_game->ctrlDown;
        *g_game->ctrlDown = d.ctrl ? 1 : 0;
        core::display::SendToGame(WM_KEYDOWN, d.code, 1);
        *g_game->ctrlDown = was;
        break;
    }
    case Send::SysKey: core::display::SendToGame(WM_SYSKEYDOWN, d.code, 1 | (1 << 29)); break;
    case Send::Char: core::display::SendToGame(WM_CHAR, d.code, 1); break;
    case Send::Rotate:
        g_rotate = true;
        core::display::SendToGame(WM_KEYDOWN, d.code, 1);
        break;
    case Send::Options:
        if (!g_game->openOptions()) {
            core_log("keys: the options window is not available");
        }
        break;
    case Send::Call:
        if (!g_game->call(d.code) && g_fireLogs <= 40) {
            core_log("keys: %s did nothing", d.info.key);
        }
        break;
    }
}

struct Call {
    UINT m;
    WPARAM w;
    LPARAM l;
    bool handled;
};

void TranslateNow(void *p)
{
    Call &c = *static_cast<Call *>(p);
    if (c.m == WM_CHAR || c.m == WM_SYSCHAR) {
        // Alt with a character: no game takes it, so Windows would beep; Alt+Space opens the window's menu.
        c.handled = g_eatChar || (c.m == WM_SYSCHAR && c.w != ' ');
        g_eatChar = false;
        return;
    }
    g_eatChar = false;
    const unsigned vk = static_cast<unsigned>(c.w);
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
        return; // the game keeps track of the modifiers itself
    }
    if (c.m == WM_KEYDOWN && c.l == 0 && ModeVk(vk) && g_game->postedModeKeysToGame) {
        return; // the game's own buttons post these to it (a real key press repeats at least once)
    }
    if (vk == VK_ESCAPE && c.m == WM_KEYDOWN) { // closes one of our windows, or opens our pause menu
        c.handled = g_eatChar = g_game->escape(c.l);
        return;
    }
    const unsigned mods = g_system->heldMods();
    const int a = Bound(vk, mods);
    const bool modeKey = ModeKey(vk, mods);
    const bool city = g_game->cityScreen();
    const bool editor = !city && g_game->editorScreen();
    if (!city && !editor) {
        if (a >= 0 && D(a).send == Send::Options) {
            Fire(a); // ours works on every screen
            c.handled = g_eatChar = true;
        }
        else if (modeKey) {
            c.handled = g_eatChar = true; // the game's display mode keys never reach it
        }
        return;
    }
    if (a >= 0 && (city || D(a).editorToo || D(a).send == Send::Options)) {
        Fire(a);
        c.handled = g_eatChar = true;
        return;
    }
    if (modeKey || GameKey(vk, mods, editor)) {
        c.handled = g_eatChar = true;
    }
}

bool Apply(int index, const char *v, bool)
{
    for (int a = 0; a < g_count; ++a) {
        if (g_index[a] == index) {
            core::ui::Hotkey k[core::ui::kMaxKeys] = {};
            core::ui::ParseHotkeyList(v, v + std::strlen(v), k);
            g_keys[a][0] = k[0];
            g_keys[a][1] = k[1];
        }
    }
    return true;
}

} // namespace

void SetSystem(const KeysSystem *system)
{
    g_system = system ? system : &kRealSystem;
}

void RegisterSettings(const KeysGame *game)
{
    if (game->count > kMaxActions) {
        core_log("keys: %d actions, at most %d: no key bindings", game->count, kMaxActions);
        core::diag::Disable(g_feature, "too many actions");
        return;
    }
    g_game = game;
    g_count = game->count;
    g_pollVk = 0;
    for (int a = 0; a < g_count; ++a) {
        const Def &d = D(a);
        core::ui::ParseHotkeyList(d.item.defaultValue, d.item.defaultValue + std::strlen(d.item.defaultValue),
                                  g_defaults[a]);
        g_keys[a][0] = g_defaults[a][0];
        g_keys[a][1] = g_defaults[a][1];
        g_index[a] = core::settings::Register(d.item);
        core::settings::OnChange(g_index[a], &Apply);
        if (d.send == Send::Rotate && g_pollVk == 0) {
            g_pollVk = d.code;
        }
    }
}

void Install()
{
    if (!g_game) {
        core_log("keys: no actions registered: the keys are the game's own");
        return;
    }
    core::display::SetKeyTranslator(&Translate);
    core::display::SetKeyStateFilter(&KeyState);
    core_log("keys: %d actions; on the city screens their keys become the game's own (F6-F8 never reach the game)",
             g_count);
}

bool Translate(UINT m, WPARAM w, LPARAM l)
{
    if (!g_game || !core::diag::Enabled(g_feature)) {
        return false;
    }
    Call c{m, w, l, false};
    if (!core::diag::Guarded(g_feature, &TranslateNow, &c)) {
        core_log("keys: the key bindings are off for the rest of the run (an error); the keys are the game's own");
        return false;
    }
    return c.handled;
}

SHORT KeyState(int vk, SHORT real)
{
    if (g_pollVk == 0 || static_cast<unsigned>(vk) != g_pollVk || !core::diag::Enabled(g_feature)) {
        return real;
    }
    const bool pressed = g_rotate;
    g_rotate = false;
    return pressed ? static_cast<SHORT>(0x8001) : 0; // the game rotates on "pressed since the last poll" (0x8001)
}

const char *PauseText()
{
    static char text[128];
    for (int a = 0; a < g_count; ++a) {
        if (std::strcmp(D(a).info.key, "pause") != 0) {
            continue;
        }
        char k1[40] = "", k2[40] = "";
        if (g_keys[a][0].vk != 0) core::ui::FormatHotkeyShown(g_keys[a][0], k1, sizeof(k1));
        if (g_keys[a][1].vk != 0) core::ui::FormatHotkeyShown(g_keys[a][1], k2, sizeof(k2));
        if (!k1[0] && !k2[0]) {
            return kPausedNoKey;
        }
        if (k1[0] && k2[0]) {
            std::snprintf(text, sizeof(text), kPausedTwoKeys, k1, k2);
        }
        else {
            std::snprintf(text, sizeof(text), kPausedOneKey, k1[0] ? k1 : k2);
        }
        return text;
    }
    return nullptr;
}

bool IsOptionsKey(unsigned vk, unsigned mods)
{
    for (int a = 0; a < g_count; ++a) {
        if (D(a).send == Send::Options) {
            for (const core::ui::Hotkey &k : g_keys[a]) {
                if (k.vk != 0 && k.vk == vk && k.mods == mods) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool Held(unsigned vk)
{
    if (!g_game || !core::diag::Enabled(g_feature)) {
        return g_system->held(vk);
    }
    for (int a = 0; a < g_count; ++a) {
        if (D(a).send != Send::KeyDown || D(a).code != vk || D(a).ctrl) {
            continue;
        }
        for (const core::ui::Hotkey &k : g_keys[a]) {
            if (k.vk != 0 && g_system->held(k.vk)) {
                return true;
            }
        }
    }
    return false;
}

int ActionCount()
{
    return g_count;
}

const ActionInfo &Action(int i)
{
    return D(i).info;
}

} // namespace core::input::keys
