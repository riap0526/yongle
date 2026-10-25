// R2a: framed full-screen pictures (docs/resolution_ui/fullscreen_art.md §7.5, r2_screens.md §4.1).
//
// While a front-end screen runs in its 1024x768 frame (emperor::resolution::FrontEnd), the output frames it
// (core/display/framing.h): the picture -- the whole frame, or a loading picture's real area found at run time --
// goes straight from the game's pixels to its place on the screen, over a background this module composes from the
// game's own dialog panel tiles (group 0x20b, read from the player's own China_Interface pack): parchment outside,
// the fret border and padding around the picture. The loading screens' status line (screen_text_gdi) is drawn by the
// output on a dark strip over the picture's bottom instead of with GDI. R2d.1 (docs/resolution_ui/r2d1.md): the
// pictures can also be shown at their own size (display.loading_screen, display.menu_screens; the default), the menus
// with a blurred picture of the player's city around them; black keeps the scaled frame with black bars (R1).
// debug.frame_labels draws the layout in label colours for tools/verify/frame_check.py.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace emperor::fullscreen {

// From emperor_register_settings (DllMain): display.loading_screen and display.menu_screens (R2d.1).
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

// w x h RGB565 pixels: parchment inside the dialog panel's fret border (one 16 px tile wide, the last tiles flush
// with the far edges), from the same tiles as the framed pictures. Used around the China map (R2c,
// games/emperor/china_map.cpp). False when the tiles cannot be read.
bool BorderPicture(int w, int h, std::vector<WORD> *out);

} // namespace emperor::fullscreen
