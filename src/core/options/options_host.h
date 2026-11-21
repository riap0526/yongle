// The options window in the game (Yongle and Pericles): core/options' window drawn with the game layer's Painter over
// the game's last frame while the game is paused, with its Hotkeys window and the pause menu under it. Nothing here
// knows a game address: the game layer gives its drawing, input origin, sounds and pause menu (the PauseGame), its
// window title, its pages, its Hotkeys page (or none) and the keys nobody may choose, in an OptionsGame (every field
// required).
//
// Input: core::display::SetModalInput has one slot; Attach registers it here for the window and the pause menu (the
// pause menu's input goes to core/options/pause_menu while the window is not open).
//
// Failure: input, opening and drawing run under the guard "options window (core/options/options_host)"; when it
// trips the window and the pause menu close, a wait for a key ends, the screen is drawn again and the window stays
// unavailable (the game layer's menus fall back to the game's own dialogs).
//
// Threads: the game thread only (the window procedure, the game loop, screen_present).
#pragma once

#include <vector>

#include "core/options/pause_menu.h"
#include "core/options/window.h"

namespace core::options::options_host {

struct OptionsGame {
    using Pages_fn = std::vector<Page> (*)();
    using Key_fn = bool (*)(unsigned vk, unsigned mods);

    OptionsGame(const pause_menu::PauseGame &pause, const char *title, Pages_fn pages, Pages_fn keyPages,
                Key_fn reservedKey, const char *const (*emergency)[2], int emergencyCount, bool hideListed)
        : pause(pause), title(title), pages(pages), keyPages(keyPages), reservedKey(reservedKey), emergency(emergency),
          emergencyCount(emergencyCount), hideListed(hideListed)
    {
    }

    const pause_menu::PauseGame &pause; // the painter, origin, drawing, redraw, sounds and the pause menu's items
    const char *title;                  // the window's title (e.g. "Yongle options")
    Pages_fn pages;                     // the options window's pages (core/options/setting_rows; rebuilt at each opening)
    Pages_fn keyPages;                  // the Hotkeys window's page, or kNoHotkeys (no Hotkeys button)
    Key_fn reservedKey;                 // a key nobody may choose (the game's own, ours that stay fixed)
    const char *const (*emergency)[2];  // hideListed: the settings ({section, key}) the debug panel keeps all the
    int emergencyCount;                 // same (to change when something goes wrong)
    bool hideListed;                    // the debug panel leaves out what the windows list ("Show all settings")
};

// A game with no Hotkeys page yet.
constexpr OptionsGame::Pages_fn kNoHotkeys = nullptr;

// Game layer, once, after its Painter's art is checked: also attaches the pause menu and registers the modal input.
void Attach(const OptionsGame &game);
bool Available(); // attached and not switched off after an error

// Game thread: opens the window at a page and category (0-based; a category by its name: the page's first when none
// has it), or the Hotkeys window alone. false: not available (the caller falls back to the game's own dialog).
bool Open(int page, int category);
bool OpenCategory(int page, const char *name);
bool OpenHotkeys();

bool IsOpen();       // the window or the pause menu: the game is paused and draws frames only
bool WindowOpen();   // the window (or the Hotkeys window) itself

// For the pause menu (PauseGame::optionsKey): the options key opens the window over the menu (General).
bool PauseOptionsKey(unsigned vk, unsigned mods);

// Game thread, before the game's frame is presented: draws the window (or the pause menu) over it. caller: the game
// address screen_present returns to (PauseGame::canDraw decides where nothing is drawn).
void OnPresent(unsigned long caller);

} // namespace core::options::options_host
