// The resolution switching both game layers share (Emperor's and Zeus's): the logical resolution in the game's
// largest mode, the 1024x768 frame of the front-end screens and the way back to the city's resolution, Shift+F7, the
// setting from the panel, and the way back to the game's own size after an error.
//
// The game's largest mode (mode 3 in both games) is made to return our logical resolution: the game layer hooks the
// game's mode-size function and calls OnModeSize after the original. Front-end screens (menus, loading, scores) are
// laid out for a fixed frame of the game's own size, so from a full-screen picture outside the city screens
// (EnterFrontEnd, from the game layer's picture hook) until the next city screen the mode is the game's own size and
// the output shows it framed (core/display/framing). OnLoop switches the mode with the game's own mode change when the
// target and the mode's size differ, at most every 500 ms (the game refuses on a few screens; tried again later).
//
// Nothing here knows a game address: the game layer gives its variables (as core::site::Var, read at each use) and
// callbacks in a SwitchGame, every field required.
//
// Failure: everything here runs under one guard ("resolution switching"); when it trips, mode 3 is the game's own
// size again from the next loop (one mode change of the game's), and the setting is no longer applied. Other parts of
// a game layer whose fallback is the game's own size call ToGameSize.
#pragma once

#include "core/config.h"
#include "core/hook/site.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace core::display::resolution {

// What the game gives. Every field is required (the constructor takes them all).
struct SwitchGame {
    using CityScreen_fn = bool (*)(unsigned char screen);
    using ChangeMode_fn = void (*)();

    SwitchGame(const char *logPrefix, core::site::Var<int> modeWidth, core::site::Var<int> modeHeight,
               core::site::Var<int> screenMode, core::site::Var<unsigned char> gameScreen, CityScreen_fn cityScreen,
               ChangeMode_fn changeMode)
        : logPrefix(logPrefix), modeWidth(modeWidth), modeHeight(modeHeight), screenMode(screenMode),
          gameScreen(gameScreen), cityScreen(cityScreen), changeMode(changeMode)
    {
    }

    const char *logPrefix;                   // the log lines' prefix, e.g. "resolution" or "zeus: resolution"
    core::site::Var<int> modeWidth;          // the game's mode size (what its mode-size function sets)
    core::site::Var<int> modeHeight;
    core::site::Var<int> screenMode;         // the game's mode number; only mode 3 carries our size
    core::site::Var<unsigned char> gameScreen; // the screen shown
    CityScreen_fn cityScreen;                // true for the screens the game draws the city for
    ChangeMode_fn changeMode;                // the game's own mode change to mode 3 (as its F7), with a redraw
};

// Starts switching with the setting from cfg (the game layer has checked and hooked its sites). The game's minimum
// size is the game's own (core_game: native_width x native_height).
void Install(const SwitchGame &game, const core_config *cfg);
bool Installed();

// From the game's mode-size hook, after the original, for mode 3 only: our size into the mode-size variables (the
// game's own comes back if this is abandoned).
void OnModeSize();

// A full-screen picture was drawn (before or after; the game layer's picture hook): outside the city screens the
// 1024x768 frame starts (OnLoop switches). picture: the game's index for the log, or -1 when it has none.
void EnterFrontEnd(int picture);

// Once per loop on the game thread: leaves the front end on a city screen, switches the mode when it differs from the
// target, and after an error goes back to the game's own size.
void OnLoop();

// Shift+F7 (from the game layer's key filter): the next of a few settings until the game ends. True when the key was
// Shift+F7 (the game does not see it).
bool CycleKey(UINT message, WPARAM key, LPARAM l);

// The video.resolution or video.scale setting changed (game thread). False when it cannot be applied (switching was
// switched off after an error).
bool SettingsChanged(const core_config *cfg);

// Switching is off from now on and the game goes back to its own size on the next loop (for other parts of the game
// layer whose fallback is that size).
void ToGameSize(const char *why);
bool SwitchingOn();

// True while a front-end screen runs in its 1024x768 frame (installed only).
bool FrontEnd();

// The city's scale on the screen: screen (or window client) pixels per game pixel at the city's resolution setting,
// the smaller of width and height. The front-end screens are shown at their own size times it.
double CityScale();

// Called after the setting changed (panel, Shift+F7) so the framing of the front-end screens follows the new scale at
// once, also on a screen that is not redrawn (nullptr: none). One listener.
void SetChangedListener(void (*listener)());

// For the offline tests: the state Install sets (without a game layer's hooks), and the front-end flag.
void InstalledForTest(const SwitchGame &game, const core_config *cfg);
void ResetForTest();

} // namespace core::display::resolution
