// Framed full-screen pictures, Emperor's part.
//
// While a front-end screen runs in its 1024x768 frame (emperor::resolution::FrontEnd), the output frames it: the
// core's core/display/framed_screens (shared with Zeus's layer) composes the background from the game's own
// dialog panel tiles (group 0x20b of the player's own China_Interface pack): the panel pattern (parchment) outside,
// the fret border and padding around the picture, or the player's city blurred around the menus. This layer keeps
// what only Emperor has: the loading screens' status line (screen_text_gdi), drawn in a status block under the
// picture in the game's own font (or by the output) instead of with GDI, and the loading pictures' black bands found
// at run time. debug.frame_labels draws the layout in label colours for a layout check.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace emperor::fullscreen {

// From emperor_register_settings (DllMain): display.loading_screen and display.menu_screens.
void RegisterSettings();

// Installs the screen_text_gdi hook. Needs emperor::resolution installed.
bool Attach();

// From resolution's fullscreen_art_load detour, after the picture was loaded and drawn at (x, y) (game thread).
void OnArtLoaded(int index, int x, int y);

// Once per main-loop iteration (game thread): framing on or off, background composed for the current layout.
void OnLoop();

// Before each screen_present (game thread): a city background finished on its worker thread is put in place. The
// menus run in a loop of their own, without game_loop_step, so OnLoop alone would show it only on the next screen.
void OnPresent();

// game_main returned (game thread): the city seen last is saved now if it was not yet (File > Exit game from the city
// never comes back to a front-end screen), written before returning.
void OnGameEnd();

// Whether menu_screens = city shows the city background on
// this front-end screen. The screens before a mission (0x3d mission choice and briefing, 0x35 mission introduction),
// the timed full-screen picture 0x04, the mission summary 0x17 and the editors' screens get parchment: the next
// mission is usually another map, and the editors have nothing to do with the city played last. The player's own
// menu_background picture is shown on every screen.
inline bool CityAround(char screen, bool playerPicture)
{
    if (playerPicture) {
        return true;
    }
    switch (static_cast<unsigned char>(screen)) {
    case 0x3d: case 0x35: case 0x04: case 0x17:                                 // before / between missions
    case 0x10: case 0x15: case 0x18: case 0x1b: case 0x27: case 0x58:           // map editor
    case 0x22: case 0x29: case 0x2b: case 0x2c: case 0x46: case 0x57:           // campaign editor
        return false;
    default:
        return true;
    }
}

} // namespace emperor::fullscreen
