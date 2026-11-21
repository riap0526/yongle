// The Hotkeys window's page for both games: a row of column titles, then each section (core::input::keys::Section)
// with its title and its actions (core::input::keys' table, one Key row each through core/options/setting_rows); our
// five keys first in Global hotkeys. The overlays are named with the game's own words ("Water overlay"), which the
// game layer gives. Also here: the debug panel's note when one of our keys is set to a key in use (our fixed keys
// answered here, the game's own keys by the game layer).
//
// Nothing here knows a game. Threads: the game thread (the page, as setting_rows); KeyNote on the panel's thread.
#pragma once

#include <string>

#include "core/options/window.h"

namespace core::options::hotkeys_page {

struct KeyPageGame {
    // An overlay action's name (its ActionInfo::textGroup > 0, textGroup / textIndex) without " overlay", e.g.
    // "Water"; "" keeps the setting's own label. Not called in a dry run (setting_rows::Dry).
    using OverlayName_fn = std::string (*)(int textGroup, int textIndex);

    explicit KeyPageGame(OverlayName_fn overlayName) : overlayName(overlayName) {}

    OverlayName_fn overlayName;
};

// The page (title "Hotkeys"), from the actions registered now. In a dry run only the settings are collected.
Page Build(const KeyPageGame &game);

// A game's "See Water" as "Water" (its overlay buttons' texts start with "See "); other texts as they are.
std::string OverlayWord(const std::string &gameText);

// The panel's note for a key chosen for one of our key settings: ours first (F5 and Alt+Enter switch window and full
// screen, Shift+F7 the resolution: these stay ours), then the game layer's gameNote (nullptr: none). nullptr: nothing
// to say. The text stays valid until the next call.
using GameKeyNote = const char *(*)(unsigned vk, unsigned mods);
const char *KeyNote(unsigned vk, unsigned mods, GameKeyNote gameNote);

} // namespace core::options::hotkeys_page
