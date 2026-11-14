// The game layer's part of the unattended smoke test (core/diag/autorun.h) beyond the screen, ticks and quit
// (emperor.cpp): the actions "win" and "lose" and the dialog area's offset for "click dialog". Only used when the
// process was started for a smoke test; nothing here runs otherwise. The same actions as Zeus's (games/zeus/smoke.h),
// at Emperor's addresses.
#pragma once

namespace emperor::smoke {

// The actions' names, separated by spaces (core/diag/autorun.h Game::actions).
extern const char kActions[];

// Game thread, in screen_present's detour (core/diag/autorun.h Game::action):
//   win   the game's own cheat "I win again", as when typed into the cheat box: cheat_run(1) sets cheat_win, and the
//         next mission_check takes the mission as won. Only on the city screen, not governing on after a won
//         mission, not already won.
//   lose  what the objectives check writes when the time for the objectives has run out (lose_reason 0x573 and the
//         two words of its message); mission_check takes it on its next two checks. Writes the game's variables
//         (there is no lose cheat). Only on the city screen, not governing on, not already lost.
// false with why when it cannot be done.
bool Action(const char *name, char *why, int size);

// dialog_offset_x / _y: where the game's 640 x 480 dialog area starts on the display mode.
void DialogOffset(int *x, int *y);

} // namespace emperor::smoke
