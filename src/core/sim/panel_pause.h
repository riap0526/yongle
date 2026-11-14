// The game's loop while our panel (or another of our windows over the game, e.g. Yongle's options window) is open: the
// game is paused -- its loop body does not run, the thread rests 5 ms a time -- except that while the panel asks
// whether to keep a new resolution (core::ui::WantsGameFrames), or while the game's own window of ours wants to be
// drawn over the game, the game draws a frame at most every 16 ms with no simulation tick (the game layer's
// sim_tick_due hook answers 0 while DrawOnly()). Shared by both game layers.
//
// Nothing here knows a game address: the game layer gives its window test, what pausing and resuming do, and the
// frame itself in a PauseGame, every field required.
//
// Failure: a draw-only frame runs under the guard "draw-only frames while the panel asks"; when it trips the frame is
// abandoned (DrawOnly() false again) and the game stays paused while the panel asks.
#pragma once

namespace core::sim::panel_pause {

struct PauseGame {
    using OwnWindow_fn = bool (*)();
    using Paused_fn = void (*)(bool ownWindow);
    using Resumed_fn = void (*)();
    using Frame_fn = int (*)();

    PauseGame(const char *logPrefix, OwnWindow_fn ownWindowOpen, Paused_fn paused, Resumed_fn resumed,
              Frame_fn drawOnlyFrame)
        : logPrefix(logPrefix), ownWindowOpen(ownWindowOpen), paused(paused), resumed(resumed),
          drawOnlyFrame(drawOnlyFrame)
    {
    }

    const char *logPrefix;     // e.g. "emperor", "zeus"
    OwnWindow_fn ownWindowOpen; // a window of ours over the game, drawn in every frame (nullptr is not allowed: a
                               // game without one gives a function that returns false, with the reason)
    Paused_fn paused;          // once when the game pauses (rate log, sounds); ownWindow: the window, not the panel
    Resumed_fn resumed;        // once when it goes on (log, counters, the rate window)
    Frame_fn drawOnlyFrame;    // one frame of the game's (its per-loop work and the original loop body); the game's
                               // result is passed back to the game's caller
};

// From the game's loop hook, first (game is the layer's constant). True when the loop is ours this time (paused, or a
// draw-only frame): the game's caller gets *result. False: the game runs as usual (after a pause, `resumed` was
// called).
bool Loop(const PauseGame &game, int *result);

// True inside a draw-only frame: the game's sim_tick_due hook answers 0.
bool DrawOnly();

} // namespace core::sim::panel_pause
