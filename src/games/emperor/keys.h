// Key bindings (0.2.0 item 7, docs/key_bindings.md): every key the game uses in the city, and ours, as actions with
// up to two keys each ([keys] in emperor_engine.ini, changed in the Hotkeys window). On the city screens a key press
// that belongs to an action becomes the key the game itself expects for it (sent to the game's window procedure); a
// game key that belongs to no action any more is kept from the game; other keys go through. Other screens (saving,
// naming, lists) get the keys as they are, so typing is never changed. F6, F7 and F8 (the game's own display mode
// keys) never reach the game; F6 opens our options window by default.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace emperor::keys {

enum Section { kArrows, kGlobal, kCity, kBuild, kOverlays, kBookmarks, kArmy, kSectionCount };

struct ActionInfo {
    const char *key;   // the setting's key in [keys]
    Section section;
    int textGroup;     // > 0: the name is the game's text (group, index): the overlays
    int textIndex;
};

// From emperor_register_settings (DllMain): one setting per action in [keys].
void RegisterSettings();
// After the game's hooks and the options window: the translator and the key-state filter. False (logged) when a
// site does not match (the keys then stay the game's own).
bool Attach();

// Game thread: a key bound to the action the game knows as vk (the arrows) is held, for smooth scrolling (which keeps
// scrolling while the key stays down). With the bindings off: the key vk itself.
bool Held(unsigned vk);
// The key (with its modifiers) opens the options window (it closes it again too).
bool IsOptionsKey(unsigned vk, unsigned mods);

int ActionCount();
const ActionInfo &Action(int i);

// Offline tests (proxy/ddraw/test_restore.cpp): the translator and the key-state filter as installed.
bool TranslateForTest(UINT message, WPARAM wparam, LPARAM lparam);
SHORT KeyStateForTest(int vk, SHORT real);

} // namespace emperor::keys
