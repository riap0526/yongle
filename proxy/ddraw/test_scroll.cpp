// Offline test of core/scroll (not the game): the smooth scroller built on Augustus's speed easing must reach and
// hold exactly the cruise speed it is given (the game's own average scroll speed), accelerate and stop within the
// easing time, and lose no fraction of a pixel. Simulates frames of a fixed length with Sleep-free fake time: the
// test drives timeGetTime-based code, so it runs in real time (about 3 s).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include "core/scroll/smooth_scroll.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what, double value)
{
    std::printf("%-62s %s (%.2f)\n", what, ok ? "ok" : "FAILED", value);
    if (!ok) {
        ++g_failures;
    }
}

// Runs frames of about frameMs for durationMs; returns the pixels moved along x.
long Run(core::scroll::SmoothScroll &s, int dir, double pxPerMs, int durationMs, int frameMs)
{
    long moved = 0;
    DWORD end = timeGetTime() + durationMs;
    while (static_cast<LONG>(end - timeGetTime()) > 0) {
        int dx = 0, dy = 0;
        s.Update(dir, 0, pxPerMs, pxPerMs, &dx, &dy);
        moved += dx;
        Sleep(frameMs);
    }
    return moved;
}

} // namespace

int main()
{
    timeBeginPeriod(1);
    // 50% scroll speed in Emperor: one 60 px step every 62 ms (+ half a ~1.5 ms loop iteration).
    const double cruise = 60.0 / (62 + 0.75);
    core::scroll::SmoothScroll s;

    long warm = Run(s, 1, cruise, 400, 2); // accelerate (75 ms easing, settled after 4x)
    DWORD t0 = timeGetTime();
    long moved = Run(s, 1, cruise, 1500, 2);
    double ms = timeGetTime() - t0;
    double rate = moved / ms;
    Check(std::fabs(rate / cruise - 1) < 0.03, "cruise speed within 3% of the game's average (ratio)", rate / cruise);
    Check(warm > 0, "moves while accelerating (px)", warm);

    // 5 ms frames versus 1 ms frames: the same speed (time-based, not frame-based).
    t0 = timeGetTime();
    long movedFast = Run(s, 1, cruise, 800, 0);
    double rateFast = movedFast / static_cast<double>(timeGetTime() - t0);
    Check(std::fabs(rateFast / cruise - 1) < 0.03, "same speed at ~1000 frames/s (ratio)", rateFast / cruise);

    // Release: decelerates and stops within 4x the easing time.
    long coast = Run(s, 0, cruise, 400, 2);
    int dx = 0, dy = 0;
    s.Update(0, 0, cruise, cruise, &dx, &dy);
    Check(!s.Moving() && dx == 0, "stopped after release (coasted px)", coast);
    Check(coast > 0 && coast < cruise * 300, "coasts a little, less than 300 ms of cruise (px)", coast);

    // Reverse direction and reset.
    Run(s, -1, cruise, 400, 2);
    t0 = timeGetTime();
    long back = Run(s, -1, cruise, 600, 2);
    double rateBack = -back / static_cast<double>(timeGetTime() - t0);
    Check(std::fabs(rateBack / cruise - 1) < 0.03, "same speed in the negative direction (ratio)", rateBack / cruise);
    s.Reset();
    s.Update(0, 0, cruise, cruise, &dx, &dy);
    Check(dx == 0 && !s.Moving(), "Reset stops at once", dx);

    timeEndPeriod(1);
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
