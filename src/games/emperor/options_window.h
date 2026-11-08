// The options window in the game (docs/resolution_ui/original_settings.md §14): core/options' window drawn with the
// game's own drawing functions and art, over the game's last frame, while the game is paused.
#pragma once

namespace emperor::options_window {

bool Attach();   // after resolution's hooks (image_draw); false: not available (the game's own options stay)
bool IsOpen();   // game thread: the game is paused and draws frames only (games/emperor/emperor.cpp)
// Game thread, before the game's frame is presented: draws the window over it. caller: the game address screen_present
// returns to (nothing is drawn on the WM_PAINT frames of a display change).
void OnPresent(unsigned long caller);

} // namespace emperor::options_window
