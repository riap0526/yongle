// The simulation pace (pace.h). Moved here from the two game layers (games/emperor/emperor.cpp, games/zeus/timing.cpp),
// which had the same logic; each rule is explained where it is applied below
// (owed ticks, falling behind, pauses).
#include "core/sim/pace.h"

#include "core/log.h"
#include "core/settings/settings.h"

#include <mmsystem.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr int kMaxTicksPerFrame = 20;
// Falling behind: a player's log showed frames every 70 ms (the output blocked) and 300+ ticks made up per 10 s --
// three or four ticks per picture, so the city jumped instead of moving. While the smoothed time between frames is
// above kBehindMs, a frame runs at most kBehindTicks ticks and what is owed beyond them is dropped (the game runs
// slower, as the original does when frames are slow); below kCaughtUpMs it catches up again.
constexpr double kBehindMs = 50.0;   // fewer than 20 frames a second
constexpr double kCaughtUpMs = 35.0; // about 28 frames a second
constexpr int kBehindTicks = 2;
// A gap between two frames longer than this is a pause, not slow frames (our windows and the panel, the game's own
// confirmations, loading): its time is not owed. Before, closing a window made up to 20 ticks at once (the city
// jumped).
constexpr DWORD kGapMs = 250;
constexpr DWORD kCappedMs = 16; // 100% with full_speed = capped (Augustus's 100%)

bool g_keepSpeed = true;        // gameplay.keep_speed
bool g_fullSpeedCapped = true;  // gameplay.full_speed
bool g_whileScrolling = true;   // gameplay.simulate_while_scrolling

struct Pace {
    DWORD last = 0;    // our tick clock: the time the ticks run so far were owed at
    bool valid = false;
    int extra = 0;     // ticks owed beyond the game's own one in this frame
    long caughtUp = 0; // extra ticks run (for the rate log)
    int mostInOnePass = 0; // the most ticks one frame ran (its own and the extra ones; for the rate log)
    DWORD lastPass = 0;    // falling behind: the time between frames, smoothed
    double passMs = 0;
    bool behind = false;
    long dropped = 0;      // ticks owed and not run while behind (for the rate log)
    long droppedSpell = 0; // the same, for the current spell of falling behind
    long gaps = 0;         // pauses whose time was not made up (for the rate log)
};
Pace g_pace;

struct DueCall {
    const core::sim::PaceGame *g;
    int (*original)();
    int result;
    core::diag::Restore r;
};

void DueBody(DueCall &c)
{
    const core::sim::PaceGame &g = *c.g;
    g_pace.extra = 0;
    if (g_whileScrolling) {
        c.r.Set(g.edgeScrolling(), 0);
    }
    const DWORD interval = core::sim::TickInterval(g);
    if (interval == 0) {
        g_pace.valid = false;
        c.result = c.original();
        c.r.Undo();
        return;
    }
    const DWORD now = timeGetTime();
    volatile DWORD *lastTick = g.lastTick();
    if (!g_pace.valid) {
        g_pace.last = *lastTick;
        g_pace.valid = true;
    }
    if (g_pace.lastPass != 0) {
        double dt = static_cast<double>(now - g_pace.lastPass);
        dt = dt > 200 ? 200 : dt; // a pause (panel, loading) counts as one slow frame, not as many
        g_pace.passMs = g_pace.passMs * 0.9 + dt * 0.1;
        if (!g_pace.behind && g_pace.passMs > kBehindMs) {
            g_pace.behind = true;
            g_pace.droppedSpell = 0;
            core_log("%s: falling behind (a %s every %.0f ms): at most %d ticks per %s, the rest is dropped (the game "
                     "runs slower instead of jumping)", g.log, g.pass, g_pace.passMs, kBehindTicks, g.pass);
        }
        else if (g_pace.behind && g_pace.passMs < kCaughtUpMs) {
            g_pace.behind = false;
            core_log("%s: no longer behind (a %s every %.0f ms): ticks are made up again; %ld dropped", g.log, g.pass,
                     g_pace.passMs, g_pace.droppedSpell);
        }
    }
    if (g_pace.lastPass != 0 && now - g_pace.lastPass > kGapMs) {
        g_pace.last = now - interval; // after a pause: one tick as soon as allowed, nothing made up
        ++g_pace.gaps;
    }
    g_pace.lastPass = now;
    if (static_cast<int>(now - g_pace.last) < 0) {
        g_pace.last = now - interval; // the clock went back: one tick owed
    }
    const bool owed = now - g_pace.last >= interval;
    // The game's own time test agrees with ours: far in the past when a tick is owed, now when not.
    c.r.Set(lastTick, owed ? now - 100000 : now);
    c.result = c.original();
    c.r.Undo();
    if (c.result > 0) {
        DWORD n = (now - g_pace.last) / interval;
        const DWORD cap = g_pace.behind ? kBehindTicks : kMaxTicksPerFrame;
        if (n > cap) {
            if (g_pace.behind) {
                g_pace.dropped += static_cast<long>(n - cap);
                g_pace.droppedSpell += static_cast<long>(n - cap);
            }
            n = cap;
            g_pace.last = now - (now - g_pace.last) % interval; // more is not made up (Augustus: at most 20)
        }
        else {
            g_pace.last += n * interval;
        }
        g_pace.extra = g.fastForward() ? 0 : static_cast<int>(n) - 1;
        if (g_pace.extra + 1 > g_pace.mostInOnePass) {
            g_pace.mostInOnePass = g_pace.extra + 1;
        }
    }
    else if (owed) {
        g_pace.last = now - interval; // held back (pause, another screen, a dialog, scrolling): one tick when allowed
    }
    *lastTick = g_pace.last;
}

struct OwedCall {
    int n;
    bool (*allowed)();
    void (*tick)();
};

void OwedBody(void *p)
{
    OwedCall &c = *static_cast<OwedCall *>(p);
    for (int k = 0; k < c.n; ++k) {
        if (!c.allowed()) {
            break;
        }
        ++g_pace.caughtUp;
        c.tick();
    }
}

const char *const kFullSpeed[] = {"capped", "original"};
const char *const kFullSpeedLabels[] = {"capped (steady, a step above 90%)", "original (as fast as the computer can)"};

} // namespace

namespace core::sim {

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "gameplay", "keep_speed", "Keep the game speed",
        "on: ticks owed because a frame took long (large city, high resolution) are made up in the next frame, as "
        "Augustus does, so the game runs at its set speed at any resolution; off: the game's own pace (one tick per "
        "frame at most, slower when frames are slow).",
        Type::Bool, "on", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_keepSpeed = std::strcmp(v, "on") == 0;
        return true;
    });
    i = core::settings::Register(Item{
        "gameplay", "full_speed", "Game speed behavior",
        "Only matters at game speed 100%. capped (default): always the same pace, a step above 90% (90% runs 45 "
        "steps a second, 100% 62; Augustus does the same). original: the game's own 100% -- it runs as fast as your "
        "computer can draw, so it changes with the city and the resolution: in a small city many times faster "
        "than 90%, in a large city on a big screen only a little faster.",
        Type::Choice, "capped", Effect::Live, true, true, 0, 0, 0, nullptr, kFullSpeed, kFullSpeedLabels, 2});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_fullSpeedCapped = std::strcmp(v, "capped") == 0;
        return true;
    });
    i = core::settings::Register(Item{
        "gameplay", "simulate_while_scrolling", "Simulate while scrolling",
        "on (default): the city goes on while the view scrolls (as in Augustus with smooth scrolling); off: it "
        "stands still while scrolling, as in the original.",
        Type::Bool, "on", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_whileScrolling = std::strcmp(v, "on") == 0;
        return true;
    });
}

void TurnOff(const PaceGame &g, const char *why)
{
    g_keepSpeed = false;
    core::diag::Disable(*g.feature, why);
}

DWORD TickInterval(const PaceGame &g)
{
    if (!g_keepSpeed) {
        return 0;
    }
    int idx = (100 - g.speed()) / 10;
    idx = idx < 0 ? 0 : idx;
    if (idx >= 10) {
        return 0;
    }
    if (idx == 0) {
        return g_fullSpeedCapped ? kCappedMs : 0;
    }
    return static_cast<DWORD>(g.tableMs(idx) + 2);
}

int Due(const PaceGame &g, int (*original)())
{
    if (!core::diag::Enabled(*g.feature)) {
        return original(); // switched off after an error, or a site did not match
    }
    DueCall c{&g, original, 0, {}};
    if (core::diag::Guarded(*g.feature, [](void *p) { DueBody(*static_cast<DueCall *>(p)); }, &c, &c.r)) {
        return c.result;
    }
    g_pace = Pace{};
    core_log("%s: simulation pace back to the game's own after an error", g.log);
    return original();
}

void RunOwedTicks(const PaceGame &g, bool (*allowed)(), void (*tick)())
{
    if (g_pace.extra <= 0 || !core::diag::Enabled(*g.feature)) {
        return;
    }
    OwedCall c{g_pace.extra, allowed, tick};
    g_pace.extra = 0;
    if (!core::diag::Guarded(*g.feature, &OwedBody, &c)) {
        g_pace = Pace{};
        core_log("%s: simulation pace back to the game's own after an error in a tick made up", g.log);
    }
}

void TakeNote(const PaceGame &g, char *out, std::size_t size)
{
    std::size_t len = 0;
    out[0] = 0;
    if (g_pace.caughtUp > 0 && len < size) {
        len += static_cast<std::size_t>(std::snprintf(out + len, size - len, "; %ld ticks made up (at most %d ticks in "
                                                      "one %s)", g_pace.caughtUp, g_pace.mostInOnePass, g.pass));
        g_pace.caughtUp = 0;
    }
    g_pace.mostInOnePass = 0;
    if (g_pace.dropped > 0 && len < size) {
        len += static_cast<std::size_t>(std::snprintf(out + len, size - len, "; %ld ticks dropped (falling behind)",
                                                      g_pace.dropped));
        g_pace.dropped = 0;
    }
    if (g_pace.gaps > 0 && len < size) {
        std::snprintf(out + len, size - len, "; %ld pause(s) not made up", g_pace.gaps);
        g_pace.gaps = 0;
    }
}

bool KeepSpeed()
{
    return g_keepSpeed;
}

bool FullSpeedCapped()
{
    return g_fullSpeedCapped;
}

bool SimulateWhileScrolling()
{
    return g_whileScrolling;
}

} // namespace core::sim
