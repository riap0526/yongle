// The game's own Display options dialog (game_screen 0x0a) with our display settings: the dialog's buttons, layout
// and what they do are the core's (core/options/display_dialog.h); here, Emperor's part (the addresses are in the
// Site declarations of display_options.cpp).
//
// Drawn with the game's own panel, button frames, fonts and the Sound options volume arrows, in the original's place.
//
// How: display_options_draw 0x5ba9c0 is hooked (our layout), and the three places in screen_handle_input that hand
// the dialog's button table (display_options_buttons 0x81ba50, 4 entries) to the game's button functions get ours
// (9 entries, the same 28-byte layout, the core's handlers). Failure: the original table and drawing come back (the
// game's own four buttons; F5 and F7 still go to our filters as before).
#pragma once

namespace emperor::display_options {

// Checks the game's bytes and installs the dialog (logged). False: the game's own dialog stays.
bool Attach();

// Once per main-loop iteration (game thread), before the game's pass: a dialog closed without OK puts the settings
// back.
void OnLoop();

} // namespace emperor::display_options
