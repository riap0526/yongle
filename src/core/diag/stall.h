// Stall samples (O1, docs/optimization.md §1.4, 2026-10-28): with the Steam overlay and Direct3D 11 both of our
// threads stopped for about 2.5 s every 10-30 s, the game thread inside the game's own software drawing. To see where
// each thread waits, a watchdog thread looks every 100 ms at what the game thread and the output thread are in:
// when one pass or one output frame has gone on for about a second, both threads are suspended for a moment, their
// registers and the top of their stacks copied, and they run on; the copies are then logged as module+offset (the
// frame-pointer chain and the code addresses on the stack that follow a call). Only logs; at most kReports stalls per
// run. Nothing here knows a game: the game layer marks its passes.
#pragma once

namespace core::diag::stall {

enum class Thread { Game, Output };

// Once, any thread (after the exception log, so addresses in our DLL get their symbols): starts the watchdog.
void Start();

// On the thread itself: the thread to watch as Game or Output (called again: the newer thread).
void RegisterThread(Thread which);

// On the watched thread, around one pass (game) or one frame (output). Cheap (two atomic stores and a tick count).
void Begin(Thread which);
void End(Thread which);

} // namespace core::diag::stall
