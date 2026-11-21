// Key bindings: every key a game uses in its city, and ours, as actions with up to two keys each ([keys] in the
// game's settings file). On the screens the game layer names, a key press that belongs to an action becomes the key
// the game itself expects for it (sent to the game's window procedure); a game key that belongs to no action any more
// is kept from the game; other keys go through. Other screens (saving, naming, lists) get the keys as they are, so
// typing is never changed. F6, F7 and F8 (both games' own display mode keys) never reach the game; F6 opens our
// options window by default. Alt with a character (WM_SYSCHAR) is never passed on, Alt+Space excepted (no game
// handles it, so it only made Windows beep).
//
// Nothing here knows a game: the actions, the screens, key_ctrl's address and the windows come from KeysGame.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/settings/settings.h"

namespace core::input::keys {

// How the game gets an action: the message its own key makes (its window procedure), or ours.
enum class Send {
    KeyDown, // WM_KEYDOWN code (with the game's key_ctrl set as `ctrl`)
    SysKey,  // WM_SYSKEYDOWN code (an Alt combination)
    Char,    // WM_CHAR code (the game reads the character)
    Rotate,  // WM_KEYDOWN code, and the game's poll GetAsyncKeyState(code) says "pressed once" (0x8001)
    Options, // ours: the options window
    Call,    // the game layer's KeysGame::call(code): an action the game has no key for (Pericles's overlays)
};

// The Hotkeys window's sections, the same in both games (their titles in core/options/menu_text).
enum Section { kArrows, kGlobal, kCity, kBuild, kOverlays, kBookmarks, kArmy, kSectionCount };

struct ActionInfo {
    const char *key; // the setting's key in [keys]
    int section;     // its section of the Hotkeys window (Section)
    int textGroup;   // > 0: the name is the game's text (group, index)
    int textIndex;
};

struct Def {
    core::settings::Item item; // its setting in [keys]: name, default keys (the game's own)
    Send send;
    unsigned code;
    bool ctrl;
    ActionInfo info;
    bool editorToo = false; // the action works in the game's map editor too (KeysGame::editorScreen)
};

// What the key bindings need from the game (a constant of the game layer, kept for the run). No member may be
// nullptr; "none" is a function that says no (Never, NoOptions, NoEscape, NoCall).
struct KeysGame {
    KeysGame(const Def *defs, int count, bool (*cityScreen)(), bool (*editorScreen)(), volatile int *ctrlDown,
             bool (*openOptions)(), bool (*escape)(LPARAM lparam), bool (*call)(unsigned code),
             bool postedModeKeysToGame)
        : defs(defs), count(count), cityScreen(cityScreen), editorScreen(editorScreen), ctrlDown(ctrlDown),
          openOptions(openOptions), escape(escape), call(call), postedModeKeysToGame(postedModeKeysToGame)
    {
    }

    const Def *defs; // the actions (count of them)
    int count;
    bool (*cityScreen)();   // every action's keys are translated here (the game's city screens)
    bool (*editorScreen)(); // only the actions marked editorToo are (the map editor's map; never with cityScreen)
    volatile int *ctrlDown; // the game's record of Ctrl (its window procedure reads it with the keys)
    bool (*openOptions)();  // opens our options window (false: not available, logged)
    bool (*escape)(LPARAM lparam); // Esc pressed: true when ours took it (a window closed, our pause menu), false
                                   // when the game gets it
    bool (*call)(unsigned code); // a Send::Call action (its Def::code; game thread, from the window procedure);
                                 // false: it did nothing (logged)
    // The game posts F6/F7 to itself (WM_KEYDOWN, lParam 0; its Display options buttons): those reach the game.
    bool postedModeKeysToGame;
};

// The "none" functions for KeysGame.
bool Never();
bool NoOptions();
bool NoEscape(LPARAM);
bool NoCall(unsigned);

// What the key bindings ask of the system, so the offline test can give a fake one. No member may be nullptr.
struct KeysSystem {
    unsigned (*heldMods)();     // the modifiers held now (core::ui::kMod*; GetKeyState)
    bool (*held)(unsigned vk);  // the key is down now (GetAsyncKeyState)
};
// nullptr: the real one (the default).
void SetSystem(const KeysSystem *system);

// From the game layer's settings registration: one setting per action in [keys]. `game` is kept.
void RegisterSettings(const KeysGame *game);
// After the game's hooks: the translator and the key-state filter (core::display).
void Install();

// Game thread: a key bound to the action the game knows as vk (the arrows) is held, for smooth scrolling (which keeps
// scrolling while the key stays down). With the bindings off (or before they are registered): the key vk itself.
bool Held(unsigned vk);
// The key (with its modifiers) opens the options window (it closes it again too).
bool IsOptionsKey(unsigned vk, unsigned mods);
// The game's pause banner ("Game paused ('P' key continues)", the same words in both games) with the player's pause
// keys (one, two, or none); nullptr before the settings are registered or without a "pause" action.
const char *PauseText();

int ActionCount();
const ActionInfo &Action(int i);

// The translator and the key-state filter as installed (also for the offline tests).
bool Translate(UINT message, WPARAM wparam, LPARAM lparam);
SHORT KeyState(int vk, SHORT real);

} // namespace core::input::keys
