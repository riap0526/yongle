// See keys.h. Evidence for every key: docs/key_bindings.md section 2 (game_wndproc 0x4766c0, its WM_CHAR table
// 0x477c7c / 0x477bf4, city_alt_keys 0x43cfa0, the R poll 0x5db570; Ghidra and capstone, 2026-11-08).
#include "games/emperor/keys.h"

#include "core/diag/guard.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"
#include "games/emperor/options_text.h"
#include "games/emperor/options_window.h"
#include "games/emperor/site.h"

#include <cstdio>
#include <cstring>

namespace emperor::keys {
namespace {

using core::settings::Effect;
using core::settings::Item;
using core::settings::Type;

// How the game gets an action: the message its own key makes (game_wndproc), or ours.
enum class Send {
    KeyDown, // WM_KEYDOWN code (with key_ctrl_down set as `ctrl`)
    SysKey,  // WM_SYSKEYDOWN code (an Alt combination)
    Char,    // WM_CHAR code (the game reads the character: overlays, categories, speed, pause, Space, M)
    Rotate,  // R: WM_KEYDOWN 'R' (the picture variant, 0xc05810) and the poll GetAsyncKeyState('R') == 0x8001
    Options, // ours: the options window
};

struct Def {
    core::settings::Item item; // its setting in [keys]: name, default keys (the game's own)
    Send send;
    unsigned code;
    bool ctrl;
    ActionInfo info;
};

constexpr int kNoText = 0;
// The overlays (overlay_hotkey 0x4765f0(group, overlay)): the overlay's number is EmperorText.eng's group 14
// ("See ..."), 23-26 group 59's 1-4 (the religions); their names in the window are the game's texts.
const Def kDefs[] = {
    {Item{"keys", "scroll_up", "Scroll up", "", Type::Hotkey, "Up", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_UP, false, {"scroll_up", kArrows, kNoText, 0}},
    {Item{"keys", "scroll_down", "Scroll down", "", Type::Hotkey, "Down", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_DOWN, false, {"scroll_down", kArrows, kNoText, 0}},
    {Item{"keys", "scroll_left", "Scroll left", "", Type::Hotkey, "Left", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_LEFT, false, {"scroll_left", kArrows, kNoText, 0}},
    {Item{"keys", "scroll_right", "Scroll right", "", Type::Hotkey, "Right", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_RIGHT, false, {"scroll_right", kArrows, kNoText, 0}},
    {Item{"keys", "open_options", "Open Options", "", Type::Hotkey, "F6", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Options, 0, false, {"open_options", kGlobal, kNoText, 0}},
    {Item{"keys", "speed_down", "Decrease game speed", "", Type::Hotkey, "LeftBracket", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '[', false, {"speed_down", kCity, kNoText, 0}},
    {Item{"keys", "speed_up", "Increase game speed", "", Type::Hotkey, "RightBracket", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, ']', false, {"speed_up", kCity, kNoText, 0}},
    {Item{"keys", "pause", "Toggle pause", "", Type::Hotkey, "P", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'p', false, {"pause", kCity, kNoText, 0}},
    {Item{"keys", "rotate_map_left", "Rotate map left", "", Type::Hotkey, "PageUp", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_PRIOR, false, {"rotate_map_left", kCity, kNoText, 0}},
    {Item{"keys", "rotate_map_right", "Rotate map right", "", Type::Hotkey, "PageDown", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_NEXT, false, {"rotate_map_right", kCity, kNoText, 0}},
    {Item{"keys", "rotate_map_north", "Rotate map to north", "", Type::Hotkey, "Home", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_HOME, false, {"rotate_map_north", kCity, kNoText, 0}},
    {Item{"keys", "rotate_building", "Rotate building", "", Type::Hotkey, "R", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Rotate, 'R', false, {"rotate_building", kCity, kNoText, 0}},
    {Item{"keys", "hold_preview", "Hold building preview", "", Type::Hotkey, "M", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'm', false, {"hold_preview", kCity, kNoText, 0}},
    {Item{"keys", "help", "Help", "", Type::Hotkey, "F9", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_F9, false, {"help", kCity, kNoText, 0}},
    {Item{"keys", "category_1", "Building menu 1", "", Type::Hotkey, "1", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '1', false, {"category_1", kBuild, kNoText, 0}},
    {Item{"keys", "category_2", "Building menu 2", "", Type::Hotkey, "2", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '2', false, {"category_2", kBuild, kNoText, 0}},
    {Item{"keys", "category_3", "Building menu 3", "", Type::Hotkey, "3", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '3', false, {"category_3", kBuild, kNoText, 0}},
    {Item{"keys", "category_4", "Building menu 4", "", Type::Hotkey, "4", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '4', false, {"category_4", kBuild, kNoText, 0}},
    {Item{"keys", "category_5", "Building menu 5", "", Type::Hotkey, "5", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '5', false, {"category_5", kBuild, kNoText, 0}},
    {Item{"keys", "category_6", "Building menu 6", "", Type::Hotkey, "6", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '6', false, {"category_6", kBuild, kNoText, 0}},
    {Item{"keys", "category_7", "Building menu 7", "", Type::Hotkey, "7", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '7', false, {"category_7", kBuild, kNoText, 0}},
    {Item{"keys", "category_8", "Building menu 8", "", Type::Hotkey, "8", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '8', false, {"category_8", kBuild, kNoText, 0}},
    {Item{"keys", "category_9", "Building menu 9", "", Type::Hotkey, "9", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '9', false, {"category_9", kBuild, kNoText, 0}},
    {Item{"keys", "category_10", "Building menu 10", "", Type::Hotkey, "0", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '0', false, {"category_10", kBuild, kNoText, 0}},
    {Item{"keys", "category_11", "Building menu 11", "", Type::Hotkey, "Minus", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, '-', false, {"category_11", kBuild, kNoText, 0}},
    {Item{"keys", "toggle_overlay", "Toggle current overlay", "", Type::Hotkey, "Space", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, ' ', false, {"toggle_overlay", kOverlays, kNoText, 0}},
    {Item{"keys", "overlay_supplies", "Overlay: supplies", "", Type::Hotkey, "S", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 's', false, {"overlay_supplies", kOverlays, 14, 1}},
    {Item{"keys", "overlay_walkers", "Overlay: walkers", "", Type::Hotkey, "K", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'k', false, {"overlay_walkers", kOverlays, 14, 2}},
    {Item{"keys", "overlay_agriculture", "Overlay: agriculture", "", Type::Hotkey, "G", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'g', false, {"overlay_agriculture", kOverlays, 14, 3}},
    {Item{"keys", "overlay_industry", "Overlay: industry", "", Type::Hotkey, "I", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'i', false, {"overlay_industry", kOverlays, 14, 4}},
    {Item{"keys", "overlay_distribution", "Overlay: distribution", "", Type::Hotkey, "J", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'j', false, {"overlay_distribution", kOverlays, 14, 5}},
    {Item{"keys", "overlay_water", "Overlay: water", "", Type::Hotkey, "W", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'w', false, {"overlay_water", kOverlays, 14, 6}},
    {Item{"keys", "overlay_medicine", "Overlay: medicine", "", Type::Hotkey, "H", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'h', false, {"overlay_medicine", kOverlays, 14, 7}},
    {Item{"keys", "overlay_hazards", "Overlay: hazards", "", Type::Hotkey, "Z", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'z', false, {"overlay_hazards", kOverlays, 14, 8}},
    {Item{"keys", "overlay_unrest", "Overlay: unrest", "", Type::Hotkey, "U", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'u', false, {"overlay_unrest", kOverlays, 14, 9}},
    {Item{"keys", "overlay_tax_income", "Overlay: tax income", "", Type::Hotkey, "X", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'x', false, {"overlay_tax_income", kOverlays, 14, 10}},
    {Item{"keys", "overlay_entertainment", "Overlay: entertainment", "", Type::Hotkey, "E", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'e', false, {"overlay_entertainment", kOverlays, 14, 11}},
    {Item{"keys", "overlay_festivals", "Overlay: festivals", "", Type::Hotkey, "L", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'l', false, {"overlay_festivals", kOverlays, 14, 12}},
    {Item{"keys", "overlay_musicians", "Overlay: musicians", "", Type::Hotkey, "N", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'n', false, {"overlay_musicians", kOverlays, 14, 13}},
    {Item{"keys", "overlay_acrobats", "Overlay: acrobats", "", Type::Hotkey, "O", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'o', false, {"overlay_acrobats", kOverlays, 14, 14}},
    {Item{"keys", "overlay_actors", "Overlay: actors", "", Type::Hotkey, "Semicolon", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, ';', false, {"overlay_actors", kOverlays, 14, 15}},
    {Item{"keys", "overlay_security", "Overlay: security", "", Type::Hotkey, "V", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'v', false, {"overlay_security", kOverlays, 14, 17}},
    {Item{"keys", "overlay_desirability", "Overlay: desirability", "", Type::Hotkey, "D", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'd', false, {"overlay_desirability", kOverlays, 14, 18}},
    {Item{"keys", "overlay_feng_shui", "Overlay: feng shui", "", Type::Hotkey, "F", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'f', false, {"overlay_feng_shui", kOverlays, 14, 19}},
    {Item{"keys", "overlay_problems", "Overlay: problems", "", Type::Hotkey, "Y", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'y', false, {"overlay_problems", kOverlays, 14, 20}},
    {Item{"keys", "overlay_water_table", "Overlay: water table", "", Type::Hotkey, "T", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 't', false, {"overlay_water_table", kOverlays, 14, 21}},
    {Item{"keys", "overlay_ancestral", "Overlay: ancestral", "", Type::Hotkey, "A", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'a', false, {"overlay_ancestral", kOverlays, 59, 1}},
    {Item{"keys", "overlay_daoism", "Overlay: daoism", "", Type::Hotkey, "Q", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'q', false, {"overlay_daoism", kOverlays, 59, 2}},
    {Item{"keys", "overlay_buddhism", "Overlay: buddhism", "", Type::Hotkey, "B", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'b', false, {"overlay_buddhism", kOverlays, 59, 3}},
    {Item{"keys", "overlay_confucianism", "Overlay: confucianism", "", Type::Hotkey, "C", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::Char, 'c', false, {"overlay_confucianism", kOverlays, 59, 4}},
    {Item{"keys", "bookmark_go_1", "Go to bookmark 1", "", Type::Hotkey, "F1", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_F1, false, {"bookmark_go_1", kBookmarks, kNoText, 0}},
    {Item{"keys", "bookmark_go_2", "Go to bookmark 2", "", Type::Hotkey, "F2", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_F2, false, {"bookmark_go_2", kBookmarks, kNoText, 0}},
    {Item{"keys", "bookmark_go_3", "Go to bookmark 3", "", Type::Hotkey, "F3", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_F3, false, {"bookmark_go_3", kBookmarks, kNoText, 0}},
    {Item{"keys", "bookmark_go_4", "Go to bookmark 4", "", Type::Hotkey, "F4", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_F4, false, {"bookmark_go_4", kBookmarks, kNoText, 0}},
    {Item{"keys", "bookmark_set_1", "Set bookmark 1", "", Type::Hotkey, "Ctrl+F1", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_F1, true, {"bookmark_set_1", kBookmarks, kNoText, 0}},
    {Item{"keys", "bookmark_set_2", "Set bookmark 2", "", Type::Hotkey, "Ctrl+F2", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_F2, true, {"bookmark_set_2", kBookmarks, kNoText, 0}},
    {Item{"keys", "bookmark_set_3", "Set bookmark 3", "", Type::Hotkey, "Ctrl+F3", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_F3, true, {"bookmark_set_3", kBookmarks, kNoText, 0}},
    {Item{"keys", "bookmark_set_4", "Set bookmark 4", "", Type::Hotkey, "Ctrl+F4", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, VK_F4, true, {"bookmark_set_4", kBookmarks, kNoText, 0}},
    {Item{"keys", "army_set_1", "Set army group 1", "", Type::Hotkey, "Ctrl+1", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, '1', true, {"army_set_1", kArmy, kNoText, 0}},
    {Item{"keys", "army_set_2", "Set army group 2", "", Type::Hotkey, "Ctrl+2", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, '2', true, {"army_set_2", kArmy, kNoText, 0}},
    {Item{"keys", "army_set_3", "Set army group 3", "", Type::Hotkey, "Ctrl+3", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, '3', true, {"army_set_3", kArmy, kNoText, 0}},
    {Item{"keys", "army_set_4", "Set army group 4", "", Type::Hotkey, "Ctrl+4", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, '4', true, {"army_set_4", kArmy, kNoText, 0}},
    {Item{"keys", "army_set_5", "Set army group 5", "", Type::Hotkey, "Ctrl+5", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, '5', true, {"army_set_5", kArmy, kNoText, 0}},
    {Item{"keys", "army_set_6", "Set army group 6", "", Type::Hotkey, "Ctrl+6", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, '6', true, {"army_set_6", kArmy, kNoText, 0}},
    {Item{"keys", "army_set_7", "Set army group 7", "", Type::Hotkey, "Ctrl+7", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, '7', true, {"army_set_7", kArmy, kNoText, 0}},
    {Item{"keys", "army_set_8", "Set army group 8", "", Type::Hotkey, "Ctrl+8", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, '8', true, {"army_set_8", kArmy, kNoText, 0}},
    {Item{"keys", "army_set_9", "Set army group 9", "", Type::Hotkey, "Ctrl+9", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::KeyDown, '9', true, {"army_set_9", kArmy, kNoText, 0}},
    {Item{"keys", "army_recall_1", "Recall army group 1", "", Type::Hotkey, "Alt+1", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::SysKey, '1', false, {"army_recall_1", kArmy, kNoText, 0}},
    {Item{"keys", "army_recall_2", "Recall army group 2", "", Type::Hotkey, "Alt+2", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::SysKey, '2', false, {"army_recall_2", kArmy, kNoText, 0}},
    {Item{"keys", "army_recall_3", "Recall army group 3", "", Type::Hotkey, "Alt+3", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::SysKey, '3', false, {"army_recall_3", kArmy, kNoText, 0}},
    {Item{"keys", "army_recall_4", "Recall army group 4", "", Type::Hotkey, "Alt+4", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::SysKey, '4', false, {"army_recall_4", kArmy, kNoText, 0}},
    {Item{"keys", "army_recall_5", "Recall army group 5", "", Type::Hotkey, "Alt+5", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::SysKey, '5', false, {"army_recall_5", kArmy, kNoText, 0}},
    {Item{"keys", "army_recall_6", "Recall army group 6", "", Type::Hotkey, "Alt+6", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::SysKey, '6', false, {"army_recall_6", kArmy, kNoText, 0}},
    {Item{"keys", "army_recall_7", "Recall army group 7", "", Type::Hotkey, "Alt+7", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::SysKey, '7', false, {"army_recall_7", kArmy, kNoText, 0}},
    {Item{"keys", "army_recall_8", "Recall army group 8", "", Type::Hotkey, "Alt+8", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::SysKey, '8', false, {"army_recall_8", kArmy, kNoText, 0}},
    {Item{"keys", "army_recall_9", "Recall army group 9", "", Type::Hotkey, "Alt+9", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     Send::SysKey, '9', false, {"army_recall_9", kArmy, kNoText, 0}},
};

constexpr int kCount = static_cast<int>(sizeof(kDefs) / sizeof(kDefs[0]));

// The game's own screenshot (Alt+1..5 in the city, city_alt_keys 0x43d03c: shot1.bmp..shot5.bmp) writes past its
// buffer at our resolutions (0x52ad1a, the K2a test's crash; it assumes the game's own sizes): not run. __cdecl(file
// name), the caller pops (0x43d041 add esp, 4).
const Site kBmpShot{"bmp_screenshot", 0x52abc0, {0x83, 0xec, 0x38, 0x53, 0x33, 0xdb, 0x68, 0x60}};
using BmpShot_fn = void(__cdecl *)(const char *file);
BmpShot_fn o_BmpShot = nullptr;

void __cdecl BmpShot_detour(const char *file)
{
    static long logs = 0;
    if (++logs <= 5) {
        core_log("keys: the game's own screenshot (%s) is not taken: it does not work at our resolutions",
                 file ? file : "?");
    }
}

const Var<char> kGameScreen{0x10de045};
const Var<int> kCtrlDown{0x1156e00}; // key_ctrl_down: game_wndproc's record of Ctrl (Ctrl+F1, Ctrl+1, fast scrolling)

core::diag::Feature g_feature{"key bindings (games/emperor/keys)"};

// The keys in effect (game thread; set by the settings' callbacks), and the defaults (the game's own keys).
core::ui::Hotkey g_keys[kCount][core::ui::kMaxKeys];
core::ui::Hotkey g_defaults[kCount][core::ui::kMaxKeys];
int g_index[kCount]; // the setting of each action
bool g_eatChar = false; // the character of a key press we handled
bool g_rotate = false;  // the rotate key was pressed: the game's next R poll says so

bool CityScreen()
{
    const char s = *kGameScreen;
    return s == 1 || s == 7 || s == 0x20 || s == 0x48 || s == 0x49;
}

unsigned HeldMods()
{
    auto held = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };
    return (held(VK_CONTROL) ? core::ui::kModCtrl : 0u) | (held(VK_SHIFT) ? core::ui::kModShift : 0u) |
           (held(VK_MENU) ? core::ui::kModAlt : 0u);
}

int Bound(unsigned vk, unsigned mods)
{
    for (int a = 0; a < kCount; ++a) {
        for (const core::ui::Hotkey &k : g_keys[a]) {
            if (k.vk == vk && k.mods == mods) {
                return a;
            }
        }
    }
    return -1;
}

// One of the game's own keys (an action's default): kept from the game when it belongs to no action now. A
// character key also with Shift (the game reads 'W' as 'w').
bool GameKey(unsigned vk, unsigned mods)
{
    if (mods == core::ui::kModCtrl && (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)) {
        return true; // the game's fast scrolling (Ctrl with the arrows): kept from the game, the keys free for players
    }
    for (int a = 0; a < kCount; ++a) {
        for (const core::ui::Hotkey &k : g_defaults[a]) {
            if (k.vk == vk && (k.mods == mods || (kDefs[a].send == Send::Char && (mods & ~core::ui::kModShift) == 0))) {
                return true;
            }
        }
    }
    return false;
}

// F6, F7, F8 without modifiers: the game's own display mode keys (game_wndproc: F6 and F7 screen_change_mode, F8
// centres the window).
bool ModeKey(unsigned vk, unsigned mods)
{
    return mods == 0 && (vk == VK_F6 || vk == VK_F7 || vk == VK_F8);
}

long g_fireLogs = 0;

void Fire(int a)
{
    const Def &d = kDefs[a];
    if (++g_fireLogs <= 40) {
        core_log("keys: %s", d.info.key);
    }
    switch (d.send) {
    case Send::KeyDown: {
        const int was = *kCtrlDown;
        *kCtrlDown = d.ctrl ? 1 : 0;
        core::display::SendToGame(WM_KEYDOWN, d.code, 1);
        *kCtrlDown = was;
        break;
    }
    case Send::SysKey: core::display::SendToGame(WM_SYSKEYDOWN, d.code, 1 | (1 << 29)); break;
    case Send::Char: core::display::SendToGame(WM_CHAR, d.code, 1); break;
    case Send::Rotate:
        g_rotate = true;
        core::display::SendToGame(WM_KEYDOWN, d.code, 1);
        break;
    case Send::Options:
        if (!emperor::options_window::Open(emperor::options_window::kPageGeneral, 0)) {
            core_log("keys: the options window is not available");
        }
        break;
    }
}

struct Call {
    UINT m;
    WPARAM w;
    bool handled;
};

void TranslateNow(void *p)
{
    Call &c = *static_cast<Call *>(p);
    if (c.m == WM_CHAR || c.m == WM_SYSCHAR) {
        c.handled = g_eatChar;
        g_eatChar = false;
        return;
    }
    g_eatChar = false;
    const unsigned vk = static_cast<unsigned>(c.w);
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
        return; // the game keeps track of the modifiers itself
    }
    const unsigned mods = HeldMods();
    int a = Bound(vk, mods);
    const bool modeKey = ModeKey(vk, mods);
    if (!CityScreen()) {
        if (a >= 0 && kDefs[a].send == Send::Options) {
            Fire(a); // ours works on every screen
            c.handled = g_eatChar = true;
        }
        else if (modeKey) {
            c.handled = g_eatChar = true; // the game's display mode keys never reach it
        }
        return;
    }
    if (a >= 0) {
        Fire(a);
        c.handled = g_eatChar = true;
        return;
    }
    if (modeKey || GameKey(vk, mods)) {
        c.handled = g_eatChar = true;
    }
}

bool Translate(UINT m, WPARAM w, LPARAM)
{
    if (!core::diag::Enabled(g_feature)) {
        return false;
    }
    Call c{m, w, false};
    if (!core::diag::Guarded(g_feature, &TranslateNow, &c)) {
        core_log("keys: the key bindings are off for the rest of the run (an error); the keys are the game's own");
        return false;
    }
    return c.handled;
}

SHORT KeyState(int vk, SHORT real)
{
    if (vk != 'R' || !core::diag::Enabled(g_feature)) {
        return real;
    }
    const bool pressed = g_rotate;
    g_rotate = false;
    return pressed ? static_cast<SHORT>(0x8001) : 0; // the game rotates on "pressed since the last poll" (0x8001)
}

bool Apply(int index, const char *v, bool)
{
    for (int a = 0; a < kCount; ++a) {
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

void RegisterSettings()
{
    for (int a = 0; a < kCount; ++a) {
        const Def &d = kDefs[a];
        core::ui::ParseHotkeyList(d.item.defaultValue, d.item.defaultValue + std::strlen(d.item.defaultValue), g_defaults[a]);
        g_keys[a][0] = g_defaults[a][0];
        g_keys[a][1] = g_defaults[a][1];
        g_index[a] = core::settings::Register(d.item);
        core::settings::OnChange(g_index[a], &Apply);
    }
}

const char *PauseText()
{
    static char text[128];
    for (int a = 0; a < kCount; ++a) {
        if (std::strcmp(kDefs[a].info.key, "pause") != 0) {
            continue;
        }
        char k1[40] = "", k2[40] = "";
        if (g_keys[a][0].vk != 0) core::ui::FormatHotkeyShown(g_keys[a][0], k1, sizeof(k1));
        if (g_keys[a][1].vk != 0) core::ui::FormatHotkeyShown(g_keys[a][1], k2, sizeof(k2));
        if (!k1[0] && !k2[0]) {
            return emperor::options_text::kPausedNoKey;
        }
        if (k1[0] && k2[0]) {
            std::snprintf(text, sizeof(text), emperor::options_text::kPausedTwoKeys, k1, k2);
        }
        else {
            std::snprintf(text, sizeof(text), emperor::options_text::kPausedOneKey, k1[0] ? k1 : k2);
        }
        return text;
    }
    return nullptr;
}

bool IsOptionsKey(unsigned vk, unsigned mods)
{
    for (int a = 0; a < kCount; ++a) {
        if (kDefs[a].send == Send::Options) {
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
    if (!core::diag::Enabled(g_feature)) {
        return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    }
    for (int a = 0; a < kCount; ++a) {
        if (kDefs[a].send != Send::KeyDown || kDefs[a].code != vk || kDefs[a].ctrl) {
            continue;
        }
        for (const core::ui::Hotkey &k : g_keys[a]) {
            if (k.vk != 0 && (GetAsyncKeyState(static_cast<int>(k.vk)) & 0x8000)) {
                return true;
            }
        }
    }
    return false;
}

bool Attach()
{
    if (Matches(kBmpShot)) {
        Install(kBmpShot, &BmpShot_detour, o_BmpShot);
    }
    else {
        core_log("keys: bmp_screenshot at 0x%lx does not match; the game's own Alt+1..5 screenshot stays",
                 kBmpShot.address);
    }
    core::display::SetKeyTranslator(&Translate);
    core::display::SetKeyStateFilter(&KeyState);
    core_log("keys: %d actions; on the city screens their keys become the game's own (F6-F8 never reach the game)",
             kCount);
    return true;
}

bool TranslateForTest(UINT message, WPARAM wparam, LPARAM lparam)
{
    return Translate(message, wparam, lparam);
}

SHORT KeyStateForTest(int vk, SHORT real)
{
    return KeyState(vk, real);
}

int ActionCount()
{
    return kCount;
}

const ActionInfo &Action(int i)
{
    return kDefs[i].info;
}

} // namespace emperor::keys
