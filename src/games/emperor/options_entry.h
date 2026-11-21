// The ways into the options window: the game's
// Options menu in the city becomes General Settings / Video / Audio / Hotkeys (each opens the window at that page), the
// game's own Autosave switch and, in single player, Difficulty (the window's General > Difficulty) and Auto defend;
// the main menu's "Visit Website" becomes "Options".
// Also the File menu's Save city screenshot and Save minimap screenshot (0.2.0 item 6).
#pragma once

#include "core/ui/game_menu_layout.h"

namespace emperor::options_entry {

// Start-up, after options_window, options_original and city_picture: the Options part only with the window, the File
// part only with the screenshots; when neither is available nothing is installed (the game's menus stay). false:
// the Options part is not installed.
bool Attach();

// For the offline test (src/games/emperor/test_restore.cpp): the single-player Options items as Attach makes them
// (returns their count), and an Options menu text as the game is given it (original: the game's lang_get_string).
int SingleItemsForTest(const core::ui::game_menu_layout::MenuItem **items);
const char *MenuTextForTest(int index, const char *(__cdecl *original)(int group, int index));

} // namespace emperor::options_entry
