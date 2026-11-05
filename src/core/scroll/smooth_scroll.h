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

// R5d (docs/resolution_ui/r5_city_zoom.md section 11): a zoom percent moved towards its target as Augustus' zoom.c
// does: speed.c at ZOOM_STEP (2 per 16.67 ms, taking the real elapsed time into account), each delta multiplied by
// (percent / 100 + 1), never past the target.
class SmoothZoom {
public:
    static constexpr int kStep = 2; // Augustus ZOOM_STEP

    // A new target (a wheel notch, the middle button): the speed starts again from now.
    void Start();
    void StartAt(time_millis now);

    // The percent for this frame (at most once per frame). smooth false: the target at once.
    int Step(int percent, int target, bool smooth);
    int StepAt(int percent, int target, bool smooth, time_millis now);

private:
    speed_type speed_{};
};

} // namespace core::scroll
