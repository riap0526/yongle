// Yongle's own texts for players in the options window (a text check scans every string literal here); the texts both
// games share are in core/options/option_text and core/options/menu_text.
#include "games/emperor/options_text.h"

#include "core/ui/hotkey.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace emperor::options_text {
namespace {

#define CHOICES(name, ...) const ChoiceText name[] = {__VA_ARGS__}
#define N(a) static_cast<int>(sizeof(a) / sizeof((a)[0]))

CHOICES(kIntro,
        {"play", "The game's intro movie plays when the game starts."},
        {"skip", "The game starts at the main menu."});

CHOICES(kMenus,
        {"city", "In a fret border, with your city blurred around it. A picture of your own named "
                 "menu_background.png or .bmp in Yongle's folder or the game folder is used instead of the city."},
        {"panel", "In a fret border on the game's own panel pattern."},
        {"framed", "In a fret border, black around it."},
        {"original", "The game's own picture, black around it, as the game shows it on a wide screen."});

CHOICES(kEmpireMap,
        {"scaled", "Larger than its own size (see the scale below), in a fret border, over the background below."},
        {"native", "At its own size, centered, over the background below."});

CHOICES(kEmpireMapBackground,
        {"map", "The same map, blurred and darkened.", "Blurred map"},
        {"city", "Your city as it was when you opened the map, standing still, as if the map were a window over it."});

// Yongle's own: its game's files and folders, the empire map, auto defend.
const Text kTexts[] = {
    {"ui", "intro_movie", "Whether the game plays its intro movie when it starts. If PlayIntroMovie=no is set in "
                          "the game's Emperor.ini, the movie is skipped either way.",
     kIntro, N(kIntro), true},
    {"video", "menu_screens", "What is around the main menu, the choice of emperor and campaign, briefings, high "
                                "scores, victory and defeat.",
     kMenus, N(kMenus)},
    {"video", "empire_map", "How the empire map is shown on a screen larger than the map.", kEmpireMap,
     N(kEmpireMap)},
    {"video", "empire_map_scale", "How large the map is, in percent of its own size. Auto: as large as fits.",
     nullptr, 0},
    {"video", "empire_map_background", "What is around the map.", kEmpireMapBackground, N(kEmpireMapBackground)},
    // The shared text, then what only Emperor has: the army panel's own words for it, and no switch in the game
    // itself (the setting is kept in Emperor.inf and the saved games, and the game goes on using it).
    {"game", "auto_defend", "When enemies come, your soldiers gather and march out to meet them by themselves. Some "
                            "time after the enemies have left, they go back to their forts. It can't be changed while "
                            "enemies are attacking. The "
                            "army panel shows Auto Combat On while it is on. The game itself has no switch for it: "
                            "switch it off before you remove Yongle, or it stays on.",
     nullptr, 0, false, "Auto defend"},
};

#undef CHOICES
#undef N

} // namespace

const char *const kOptionsTitle = "Yongle options";

void Register()
{
    core::options::option_text::SetGameTexts(kTexts, static_cast<int>(sizeof(kTexts) / sizeof(kTexts[0])));
}

// Our own fixed keys (F5, Alt+Enter, Shift+F7) are answered by the core (core/options/hotkeys_page) first.
const char *GameKeyNote(unsigned vk, unsigned mods)
{
    using core::ui::kModAlt;
    using core::ui::kModCtrl;
    if (mods == kModAlt && ((vk >= '1' && vk <= '5') || vk == 'T')) {
        return "The game also uses this key; it no longer reaches the game.";
    }
    if (mods == (kModCtrl | kModAlt) && vk == 'C') {
        return "The game also uses this key; it no longer reaches the game.";
    }
    if ((vk == VK_INSERT || vk == VK_DELETE) && mods == 0) {
        // Only for typing in the game (Insert: insert / overwrite; Delete: deleting, and closing the
        // editor's number pad, screen 0x0e, game_wndproc -> 0x5eec50): hardly ever used, so nothing to tell.
        return nullptr;
    }
    if (vk == VK_F2 || vk == VK_F5 || vk == VK_F6 || vk == 'R') {
        return "The game also reacts to this key itself.";
    }
    if ((mods & (kModCtrl | kModAlt)) == 0) {
        const bool used = (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || (vk >= VK_F1 && vk <= VK_F9) ||
                          vk == VK_SPACE || vk == VK_TAB || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR ||
                          vk == VK_NEXT || vk == VK_NUMPAD2;
        if (used) {
            return "The game also uses this key; it no longer reaches the game.";
        }
    }
    return nullptr;
}

} // namespace emperor::options_text
