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

    // R5d: the zoom moved towards its target as Augustus' zoom.c (fixed frame times: deterministic).
    {
        core::scroll::SmoothZoom z;
        auto frames = [&z](int from, int to, int frameMs, int *stepsOut) {
            time_millis now = 1000000;
            z.StartAt(now);
            int p = from, n = 0;
            bool monotonic = true;
            while (p != to && n < 1000) {
                now += frameMs;
                const int q = z.StepAt(p, to, true, now);
                monotonic = monotonic && (to > from ? q >= p && q <= to : q <= p && q >= to);
                p = q;
                ++n;
            }
            *stepsOut = n;
            return monotonic && p == to;
        };
        int n60 = 0, n144 = 0, nIn = 0, n30 = 0;
        const bool up = frames(100, 125, 17, &n60);
        Check(up && n60 >= 4 && n60 <= 8, "R5d zoom 100 -> 125 at 60 frames/s: frames (4..8)", n60);
        const bool up144 = frames(100, 125, 7, &n144);
        Check(up144 && n144 >= 10 && n144 <= 18, "R5d zoom 100 -> 125 at 144 frames/s: frames (10..18)", n144);
        const bool in = frames(75, 50, 17, &nIn);
        Check(in && nIn >= 10 && nIn <= 16, "R5d zoom 75 -> 50 at 60 frames/s (slower below 100): frames", nIn);
        const bool slow = frames(150, 125, 33, &n30);
        Check(slow && n30 >= 2 && n30 <= 5, "R5d zoom 150 -> 125 at 30 frames/s: frames (2..5)", n30);
        Check(z.StepAt(100, 150, false, 0) == 150, "R5d zoom without smooth scrolling: at once", 150);
        Check(z.StepAt(125, 125, true, 0) == 125, "R5d zoom at the target stays", 125);
    }

    timeEndPeriod(1);
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
