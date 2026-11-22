// See keys.h. Every key was found in the game's code (game_wndproc 0x4766c0, its WM_CHAR table
// 0x477c7c / 0x477bf4, city_alt_keys 0x43cfa0, the R poll 0x5db570; Ghidra and capstone). The bindings
// themselves are the core's (core/input/keys, shared with Zeus); this file gives it Emperor's actions.
#include "games/emperor/keys.h"

#include "core/input/keys.h"
#include "core/log.h"
#include "core/ui/settings_view.h"
#include "games/emperor/game_menu.h"
#include "games/emperor/options_text.h"
#include "games/emperor/options_window.h"
#include "games/emperor/site.h"

#include <cstring>

namespace emperor::keys {
namespace {

using core::input::keys::Def;
using core::input::keys::Send;
using core::settings::Effect;
using core::settings::Item;
using core::settings::Type;

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

// Alt+1..5's BMP screenshot (bmp_screenshot 0x52abc0) is the resolution's part (games/emperor/resolution): not run
// only while our resolutions are used.

const Var<char> kGameScreen{0x10de045};
const Var<int> kCtrlDown{0x1156e00}; // key_ctrl_down: game_wndproc's record of Ctrl (Ctrl+F1, Ctrl+1, fast scrolling)

bool CityScreen()
{
    const char s = *kGameScreen;
    return s == 1 || s == 7 || s == 0x20 || s == 0x48 || s == 0x49;
}

bool OpenOptions()
{
    return emperor::options_window::Open(emperor::options_window::kPageGeneral, 0);
}

// Emperor's editor is another program (EmperorEdit.exe): never an editor screen here. No Send::Call actions (NoCall):
// every action, the overlays too, is a key of the game's own.
const core::input::keys::KeysGame &Game()
{
    static const core::input::keys::KeysGame game(kDefs, kCount, &CityScreen, &core::input::keys::Never, kCtrlDown.ptr(),
                                                  &OpenOptions, &emperor::game_menu::Escape, &core::input::keys::NoCall,
                                                  false);
    return game;
}

} // namespace

void RegisterSettings()
{
    core::input::keys::RegisterSettings(&Game());
}

const char *PauseText()
{
    return core::input::keys::PauseText();
}

bool IsOptionsKey(unsigned vk, unsigned mods)
{
    return core::input::keys::IsOptionsKey(vk, mods);
}

bool Held(unsigned vk)
{
    return core::input::keys::Held(vk);
}

bool Attach()
{
    core::input::keys::Install();
    // The panel's note for a game key chosen for one of our keys (here, not with a feature that may not attach).
    core::ui::SetGameKeyNote(&emperor::options_text::GameKeyNote);
    return true;
}

bool TranslateForTest(UINT message, WPARAM wparam, LPARAM lparam)
{
    return core::input::keys::Translate(message, wparam, lparam);
}

SHORT KeyStateForTest(int vk, SHORT real)
{
    return core::input::keys::KeyState(vk, real);
}

} // namespace emperor::keys
