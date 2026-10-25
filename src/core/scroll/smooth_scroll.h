// Smooth (pixel) scrolling for a camera that a game moves in whole steps. Game-independent: the game layer says
// in which direction the player is scrolling and how fast the game itself would scroll (pixels per millisecond per
// axis); this returns how many pixels to move this frame, accelerating and decelerating with Augustus's speed
// easing (third_party/augustus/src/core/speed.c, AGPL-3.0) so that the cruise speed, and with it the average
// speed while scrolling, is exactly the one given.
#pragma once

extern "C" {
#include "core/speed.h" // Augustus; see third_party/augustus/README.md
}

namespace core::scroll {

class SmoothScroll {
public:
    SmoothScroll();

    // Stops at once (the camera was moved by something else, or hit a map edge).
    void Reset();
    void ResetX();
    void ResetY();

    // Call at most once per frame. dirX/dirY are -1, 0 or 1; pxPerMsX/Y the cruise speed per axis. Writes the whole
    // pixels to move now (the fraction is kept for the next frame).
    void Update(int dirX, int dirY, double pxPerMsX, double pxPerMsY, int *dx, int *dy);

    // True while moving or still slowing down.
    bool Moving() const;

    static constexpr unsigned kEaseMs = 75; // Augustus SCROLL_REGULAR_DECAY_TIME: time to change speed

private:
    speed_type x_{};
    speed_type y_{};
};

} // namespace core::scroll
