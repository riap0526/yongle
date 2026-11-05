#include "core/scroll/smooth_scroll.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

// Clock for Augustus's speed.c (its core/time.h). As in Augustus, the time is fixed once per frame
// (time_set_millis) so that every speed computation in one frame sees the same instant. timeGetTime is the clock
// the game paces itself with; it wraps every 49.7 days, so it is widened to 64 bits.
namespace {

time_millis g_now = 0;
DWORD g_last = 0;
unsigned long long g_high = 0;

time_millis Now()
{
    DWORD t = timeGetTime();
    if (t < g_last) {
        g_high += 0x100000000ull;
    }
    g_last = t;
    return g_high + t;
}

} // namespace

extern "C" time_millis time_get_millis(void)
{
    return g_now;
}

extern "C" void time_set_millis(time_millis millis)
{
    g_now = millis;
}

namespace core::scroll {

namespace {

// speed.c measures speed in pixels per 16.67 ms "frame" (its FRAME_TIME) and scales by the real elapsed time.
constexpr double kAugustusFrameMs = 16.67;

} // namespace

SmoothScroll::SmoothScroll()
{
    time_set_millis(Now());
    speed_clear(&x_);
    speed_clear(&y_);
}

void SmoothScroll::Reset()
{
    ResetX();
    ResetY();
}

void SmoothScroll::ResetX()
{
    time_set_millis(Now());
    speed_clear(&x_);
}

void SmoothScroll::ResetY()
{
    time_set_millis(Now());
    speed_clear(&y_);
}

void SmoothScroll::Update(int dirX, int dirY, double pxPerMsX, double pxPerMsY, int *dx, int *dy)
{
    time_set_millis(Now());
    speed_set_target(&x_, dirX * pxPerMsX * kAugustusFrameMs, kEaseMs, 1);
    speed_set_target(&y_, dirY * pxPerMsY * kAugustusFrameMs, kEaseMs, 1);
    *dx = speed_get_delta(&x_);
    *dy = speed_get_delta(&y_);
}

bool SmoothScroll::Moving() const
{
    return x_.current_speed != 0 || x_.desired_speed != 0 || y_.current_speed != 0 || y_.desired_speed != 0;
}

void SmoothZoom::Start()
{
    StartAt(Now());
}

void SmoothZoom::StartAt(time_millis now)
{
    time_set_millis(now);
    speed_clear(&speed_);
    speed_set_target(&speed_, kStep, SPEED_CHANGE_IMMEDIATE, 1);
}

int SmoothZoom::Step(int percent, int target, bool smooth)
{
    return StepAt(percent, target, smooth, Now());
}

int SmoothZoom::StepAt(int percent, int target, bool smooth, time_millis now)
{
    if (!smooth || percent == target) {
        return target;
    }
    time_set_millis(now);
    const int delta = speed_get_delta(&speed_) * (percent / 100 + 1);
    if (percent < target) {
        return percent + delta >= target ? target : percent + delta;
    }
    return percent - delta <= target ? target : percent - delta;
}

} // namespace core::scroll
