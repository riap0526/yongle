// The simulation pace, for games of this family whose sim_tick_due runs at most one tick per frame (Emperor
// and Zeus). The game sets its tick time to "now" when it runs
// a tick, so a slow frame stretches the interval to the next tick. As Augustus does: the time owed is kept, and the
// ticks owed run in the same frame, at most 20, the rest carried over.
//   sim_tick_due still decides whether a tick may run (pause, screen, dialogs, scrolling); only the time test is
//   ours: its tick time is set so that its own test agrees with ours, and set back to our clock afterwards.
//   The extra ticks run right after the game's own one, each as the game's frame runs one (the game layer's tick()).
//   Falling behind (frames slower than 50 ms, smoothed): at most 2 ticks per frame, the rest dropped (the game runs
//   slower instead of jumping); below 35 ms ticks are made up again. A gap above 250 ms is a pause: not made up.
//   100% (2 ms, "as fast as it can"): 16 ms unless gameplay.full_speed = original.
// [gameplay] keep_speed, full_speed, simulate_while_scrolling are registered here, with the same texts in every game.
// The game layer hooks sim_tick_due and sim_tick itself (its addresses, which call site is the frame's) and calls
// Due and RunOwedTicks; what is the game's (its variables, its tick table) comes in a PaceGame. A fault in our part is
// guarded: the values we set are put back, the pace is switched off and the game's own pace goes on. Game thread.
// Nothing here knows a game's addresses.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>

#include "core/diag/guard.h"

namespace core::sim {

// What the pace needs from the game (a constant of the game layer; the functions read the game's memory each time,
// so the offline tests' shifted copy works too).
struct PaceGame {
    const char *log;           // the log lines' prefix, e.g. "zeus"
    const char *pass;          // what the log calls one turn of the game's loop, e.g. "frame"
    core::diag::Feature *feature; // switched off on a fault (the game layer's, so its name says which game)
    volatile DWORD *(*lastTick)(); // sim_last_tick_time: when the game ran its last tick
    volatile int *(*edgeScrolling)(); // what sim_tick_due checks for scrolling at the screen edge (0 while deciding)
    bool (*fastForward)();     // the game runs ticks itself until something ends (no ticks made up meanwhile)
    int (*speed)();            // game speed, percent
    int (*tableMs)(int idx);   // the game's tick table at (100 - speed) / 10, in ms (sim_tick_due adds 2)
};

// From the game layer's register_settings: [gameplay] keep_speed, full_speed, simulate_while_scrolling.
void RegisterSettings();

// The game's own pace from now on (a site the pace needs does not match): keep_speed off, the feature off.
void TurnOff(const PaceGame &g, const char *why);

// The length of one tick as the pace runs it now (0: the game's own pace -- keep_speed off, speed 0, or 100% with
// full_speed = original).
DWORD TickInterval(const PaceGame &g);

// From the game's sim_tick_due detour, for the frame's own call only (the caller checks the return address):
// the pace's answer, or the game's own (original) when the pace is off or failed.
int Due(const PaceGame &g, int (*original)());

// From the game's sim_tick detour, after the game's own tick, for the frame's own call only: the ticks owed beyond it.
// allowed() is asked before each (false: the last tick paused the game, opened a dialog, changed the screen); tick()
// runs one as the game's frame would (what the frame does after a tick, the counter, the tick).
void RunOwedTicks(const PaceGame &g, bool (*allowed)(), void (*tick)());

// For the game's rate line: "; N ticks made up (at most M ticks in one <pass>); N ticks dropped (falling behind);
// N pause(s) not made up" (only the parts that happened), and starts a new window. "" when nothing happened.
void TakeNote(const PaceGame &g, char *out, std::size_t size);

// The settings as they are now (for log lines).
bool KeepSpeed();
bool FullSpeedCapped();
bool SimulateWhileScrolling();

} // namespace core::sim
