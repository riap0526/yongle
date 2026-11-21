// Key bindings (0.2.0 item 7): Emperor's actions for the core's key bindings (core/input/keys: every key the game
// uses in the city, and ours, with up to two keys each; [keys] in emperor_engine.ini, changed in the Hotkeys window).
// On the city screens a key press that belongs to an action becomes the key the game itself expects for it; other
// screens get the keys as they are. F6, F7 and F8 never reach the game; F6 opens our options window by default.
// Also here: the game's own BMP screenshot (Alt+1..5) is not taken.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/input/keys.h"

namespace emperor::keys {

// The Hotkeys window's sections (core's, the same in both games; ActionInfo::section).
using core::input::keys::kArmy;
using core::input::keys::kArrows;
using core::input::keys::kBookmarks;
using core::input::keys::kBuild;
using core::input::keys::kCity;
using core::input::keys::kGlobal;
using core::input::keys::kOverlays;

// From emperor_register_settings (DllMain): one setting per action in [keys].
void RegisterSettings();
// After the game's hooks and the options window: the translator and the key-state filter, and the debug panel's
// note on the game's own keys (games/emperor/options_text). False (logged) when a site does not match (the keys then
// stay the game's own).
bool Attach();

// Game thread: a key bound to the action the game knows as vk (the arrows) is held, for smooth scrolling (which keeps
// scrolling while the key stays down). With the bindings off: the key vk itself.
bool Held(unsigned vk);
// The key (with its modifiers) opens the options window (it closes it again too).
bool IsOptionsKey(unsigned vk, unsigned mods);
// The game's pause banner ("Game paused ('P' key continues)", EmperorText.eng group 13, string 2) with the player's
// pause keys (one, two, or none); nullptr before the settings are registered.
const char *PauseText();

// Offline tests (src/games/emperor/test_restore.cpp): the translator and the key-state filter as installed.
bool TranslateForTest(UINT message, WPARAM wparam, LPARAM lparam);
SHORT KeyStateForTest(int vk, SHORT real);

} // namespace emperor::keys
