// A window the game always draws at one place, shown somewhere else (both games' information windows the game opens
// at a fixed place, whose drawing does not all follow the window's position; moved from games/emperor/resolution.cpp
// when Zeus needed it). Nothing here knows a game: the game layer gives its draw_buffer and mouse variables.
//
// Between Begin and End the game draws into a copy of the screen (draw_buffer points at it) in which the window's
// own place (`from`) holds what is on screen where it is shown (`to`), with the mouse moved back by the shift (so
// that hover is worked out in the game's own place of the window); End puts draw_buffer and the mouse back and
// composes: everything outside `from` the call drew is copied back 1:1, `from` is copied to `to`
// (core/display/moved_rect). Everything the game computes stays as in the original.
//
// Under a guard: pass Changes() as the guard's Restore, so that a fault puts draw_buffer and the mouse back; then call
// Abandon (the window is drawn where the game has it from then on).
#pragma once

#include "core/diag/guard.h"
#include "core/display/moved_rect.h"

namespace core::display {

class WindowMove {
public:
    // logPrefix for the log lines ("resolution: "); the timing shows in the perf panel as "info window move (ours)".
    explicit WindowMove(const char *logPrefix) : prefix_(logPrefix) {}

    // The window's rectangle in the game's place (x, y, w, h) shown shifted by (dx, dy). False (nothing changed) when
    // either rectangle is not inside the screen or no copy could be made: the caller then draws the plain way.
    bool Begin(BYTE *volatile *drawBuffer, int pitch, int screenW, int screenH, int x, int y, int w, int h,
               int dx, int dy, volatile int *mouseX, volatile int *mouseY);
    // Puts draw_buffer and the mouse back and shows what was drawn. Nothing when not active.
    void End();
    bool Active() const { return active_; }
    // After a fault inside the drawing: the guard put the changes back (Changes()); nothing is composed.
    void Abandon() { active_ = false; }
    core::diag::Restore &Changes() { return r_; }

private:
    const char *prefix_;
    bool active_ = false;
    Surface16 back_{};
    RECT from_{}, to_{};
    core::diag::Restore r_;
    BYTE *copy_ = nullptr;
    size_t copySize_ = 0;
};

} // namespace core::display
