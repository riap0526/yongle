// The options window in the game (docs/archive/resolution_ui_original_settings.md §14): core/options' window drawn with the
// game's own drawing functions and art, over the game's last frame, while the game is paused.
#pragma once

namespace emperor::options_window {

bool Attach();   // after resolution's hooks (image_draw); false: not available (the game's own options stay)
bool IsOpen();   // game thread: the game is paused and draws frames only (games/emperor/emperor.cpp)
bool Available(); // attached and not switched off after an error
// Game thread: opens the window at a page and category (0-based). false: not available (the caller falls back to
// the game's own dialog).
bool Open(int page, int category);
bool OpenHotkeys(); // the Hotkeys window alone (the game's Options menu); false: not available
// The pause menu (K4, docs/archive/key_bindings.md section 8): Continue, Replay map, Load / Save / Delete game, Options, Exit to
// main menu, Exit game; the game is paused while it is open. false: not available.
bool OpenPause();
bool OpenCategory(int page, const char *name); // at the category of that name (its first when none has it)
// The pages and categories the menus open (BuildPages' order).
constexpr int kPageGeneral = 0, kPageVideo = 1, kPageAudio = 2;
// Game thread, before the game's frame is presented: draws the window over it. caller: the game address screen_present
// returns to (nothing is drawn on the WM_PAINT frames of a display change).
void OnPresent(unsigned long caller);

} // namespace emperor::options_window
