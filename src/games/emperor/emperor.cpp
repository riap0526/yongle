// Game hooks for Emperor. Addresses, names and evidence: docs/symbols.md; timing analysis: docs/performance.md.
//
//   game_main      0x4754f0  WinMain (stdcall, 4 args): after it returns, the timer resolution is given back.
//   game_loop_step 0x475b00  one main-loop iteration with a frame (counted; drives the periodic rate log).
//   sim_tick       0x5371a0  one simulation tick (counted).
//   screen_present 0x52be40  the game's frame presentation (cdecl, 1 arg). Only hooked for present=display/cap60:
//      the game presents at most every 50 ms (present_accum_ms > 49) unless present_force is set or the argument is 1.
//      When the output thread wants a new frame, the call is made with 1 so the frame the game has just drawn is
//      presented; otherwise it is left alone. Frames the game itself forces (pressed buttons, messages, screen
//      changes) are held on screen for the 50 ms the game's own presentation would have shown them.
//      Nothing in the game reads present_accum_ms or present_force except screen_present itself, and every
//      animation, the simulation and scrolling run on their own timers (performance.md §8), so presenting more
//      often changes no game timing.
//   Screenshots (R2.0, core/capture/screenshot.h): the file name starts with game_screen, e.g. screen_0x14
//      (the list of screens: docs/resolution_ui/r2_screens.md).
//   Smooth scrolling (M4): games/emperor/scroll.cpp.
//   Custom logical resolution (R1): games/emperor/resolution.cpp.
//   Framed full-screen pictures (R2a): games/emperor/fullscreen.cpp.
//   Our settings panel (P1, core/ui/panel.h): while it is open game_loop_step is not called, so the game is paused
//      (no simulation, drawing or input handling); the main loop keeps pumping messages and sleeps 5 ms per
//      iteration. game_main ignores game_loop_step's return value (0x475a4b: call, then jmp past WaitMessage).
//      sim_tick_due sets its last tick to "now" (performance.md §8), so the simulation does not catch up afterwards.
//   Failure (docs/archive/resolution_ui_r2b.md §1): the draw-only frames (P2: the game draws while our panel asks to
//      keep a new resolution) run under core/diag's guard; on an exception the frame is abandoned, g_drawOnly goes
//      back to false and the game simply stays paused while the panel asks. Settings callbacks are guarded by
//      core/settings. Normal frames are the game's own and are not guarded.
#include "games/emperor/emperor.h"

#include "core/capture/screenshot.h"
#include "core/config.h"
#include "core/ddraw/surface_memory.h"
#include "core/diag/environment.h"
#include "core/diag/guard.h"
#include "core/diag/perf.h"
#include "core/diag/report.h"
#include "core/diag/stall.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/output/sdl_output.h"
#include "core/platform/timing.h"
#include "core/settings/settings.h"
#include "core/ui/panel.h"
#include "core/ui/perf_view.h"
#include "core/ui/settings_view.h"
#include "games/emperor/china_map.h"
#include "games/emperor/display_options.h"
#include "games/emperor/fullscreen.h"
#include "games/emperor/resolution.h"
#include "games/emperor/scroll.h"
#include "games/emperor/site.h"
#include "games/emperor/ui_fixes.h"

#include <imgui.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstring>

#ifndef YONGLE_VERSION
#define YONGLE_VERSION "dev" // set by CMakeLists.txt for the DLL; the offline tests build without it
#endif

namespace {

using emperor::Install;
using emperor::Matches;
using emperor::Site;

// ---------------------------------------------------------------------------------------------
// Addresses (Emperor.exe 1.0.1.0, image base 0x400000, no relocations)

const Site kGameMain{"game_main", 0x4754f0, {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0x98, 0xca}};
const Site kLoopStep{"game_loop_step", 0x475b00, {0xa1, 0xac, 0xe4, 0x0d, 0x01, 0x85, 0xc0, 0x74}};
const Site kSimTick{"sim_tick", 0x5371a0, {0xe8, 0x0b, 0x18, 0xee, 0xff, 0xe8, 0xe6, 0x27}};
const Site kScreenPresent{"screen_present", 0x52be40, {0xa1, 0xe0, 0x4b, 0x85, 0x00, 0x83, 0xec, 0x20}};
const Site kSimDue{"sim_tick_due", 0x53a130, {0x53, 0x56, 0x57, 0xe8, 0x88, 0xe9, 0xec, 0xff}};

using emperor::GameAddress;
using emperor::Original;
using emperor::Var;

const Var<char> kPresentForce{0x10de034}; // present_force
// Simulation pace (docs/simulation.md): sim_tick_due's inputs and what game_frame does around sim_tick.
const Site kAfterTick{"sim_after_tick", 0x535770, {0x81, 0xec, 0x04, 0x01, 0x00, 0x00, 0x53, 0x56}};
const Var<DWORD> kSimLastTick{0x115f540}; // sim_last_tick_time
const Var<int> kTickCounter{0x115f550};   // game_frame counts each tick here before sim_tick
const Var<int> kViewScrolling{0x10de038}; // view_scrolling
const Var<char> kSimPaused{0x10de0d0};    // the game's pause
const Var<char> kFastForward{0x10de3f4};  // game_frame runs ticks until a right-button release while set
constexpr DWORD kTickTable = 0x855214;    // tick_interval_table (ms, + 2 in sim_tick_due), by (100 - speed) / 10
constexpr DWORD kDueFromFrame = 0x536ba8; // game_frame's return addresses from sim_tick_due and sim_tick
constexpr DWORD kTickFromFrame = 0x536bc5;
const Var<int> kGameSpeed{0x10de0dc};     // game_speed (10..100)
const Var<char> kGameScreen{0x10de045};   // game_screen (1 = city)
// For canary reports (R2b.1): the city view as viewport_init 0x469820 left it (symbols.md).
const Var<int> kScreenW{0x10de01c}, kScreenH{0x10de018};
const Var<int> kSmallTiles{0x13f8e20};    // city_view_small_tiles
const Var<int> kCityClipLeft{0xadf89c}, kCityClipTop{0xadf894}, kCityClipRight{0xadf898}, kCityClipBottom{0xadf890};
const Var<int> kViewCols{0xadf8c4}, kViewRows{0xadf8c0}, kTileW{0xadf8b8}, kTileHalfH{0xadf8ac};

constexpr DWORD kPresentGateMs = 50; // screen_present presents when more than 49 ms have accumulated
constexpr DWORD kRateLogMs = 10000;

// ---------------------------------------------------------------------------------------------
// Hooks

using GameMain_fn = int(WINAPI *)(HINSTANCE, HINSTANCE, LPSTR, int);
using LoopStep_fn = int(__cdecl *)();
using SimTick_fn = void(__cdecl *)();
using ScreenPresent_fn = void(__cdecl *)(int);
using SimDue_fn = int(__cdecl *)();
GameMain_fn o_GameMain;
LoopStep_fn o_LoopStep;
SimTick_fn o_SimTick;
ScreenPresent_fn o_ScreenPresent;
SimDue_fn o_SimDue;

void DescribeForCanary(char *out, size_t size); // below

// Where the time goes (R2b.1-3, core/diag/perf.h): a game pass is one game_loop_step; the simulation is sim_tick.
// The city's own drawing and our copies are timed in games/emperor/scroll and resolution.
namespace perf = core::diag::perf;
const int kPassPhase = perf::Register("game pass", perf::Thread::Game, true);
const int kSimPhase = perf::Register("simulation (sim_tick)", perf::Thread::Game);

// Counters and rate log (game thread only).
long g_loops = 0, g_ticks = 0;
DWORD g_since = 0;
int g_lastSpeed = -1;

// The simulation pace (see SimDue_detour below).
struct Pace {
    DWORD last = 0;   // our tick clock: the time the ticks run so far were owed at
    bool valid = false;
    int extra = 0;    // ticks owed beyond the game's own one in this frame
    long caughtUp = 0; // extra ticks run (for the rate log)
    int mostInOnePass = 0; // the most ticks one pass ran (its own and the extra ones; for the rate log)
    // Falling behind (O1): the time between passes, smoothed; while it is long, few ticks per pass (see DueBody).
    DWORD lastPass = 0;
    double passMs = 0;
    bool behind = false;
    long dropped = 0;       // ticks owed and not run while behind (for the rate log)
    long droppedSpell = 0;  // the same, for the current spell of falling behind
};
Pace g_pace;

void LogRates(const char *why)
{
    DWORD now = GetTickCount();
    DWORD ms = now - g_since;
    if (ms == 0) {
        return;
    }
    long presented = 0, shown = 0;
    core::output::TakeCounts(&presented, &shown);
    if (g_loops > 0) {
        emperor::scroll::SetAverageFrameMs(static_cast<double>(ms) / g_loops);
    }
    char scroll[256];
    emperor::scroll::TakeStats(scroll, sizeof(scroll));
    char paceNote[128] = "";
    if (g_pace.caughtUp > 0) {
        std::snprintf(paceNote, sizeof(paceNote), "; %ld ticks made up (at most %d ticks in one pass)", g_pace.caughtUp,
                      g_pace.mostInOnePass);
        g_pace.caughtUp = 0;
    }
    g_pace.mostInOnePass = 0;
    if (g_pace.dropped > 0) {
        const size_t len = std::strlen(paceNote);
        std::snprintf(paceNote + len, sizeof(paceNote) - len, "; %ld ticks dropped (falling behind)", g_pace.dropped);
        g_pace.dropped = 0;
    }
    long multi = 0, torn = 0;
    core::diag::TakeIntegrity(&multi, &torn);
    static const char *const kPresent[] = {"original", "display", "cap60"};
    double s = ms / 1000.0;
    core_log("rates%s: loop %.0f/s, sim ticks %.1f/s, game presents %.1f/s, frames shown %.1f/s over %lu ms "
             "(present=%s, display %d Hz, game speed %d%%, screen %d, timer %u us); frame check: %ld game frames "
             "with 2+ presents, %ld torn%s%s",
             why, g_loops / s, g_ticks / s, presented / s, shown / s, ms, kPresent[core_config_get()->present],
             core::output::RefreshHz(), *kGameSpeed, static_cast<int>(*kGameScreen), core_timer_resolution_us(),
             multi, torn, scroll, paceNote);
    char times[768], longest[768];
    perf::TakeWindow(times, sizeof(times), longest, sizeof(longest));
    if (times[0]) {
        core_log("time%s (ms): %s", why, times);
        core_log("time max%s (ms): %s", why, longest);
    }
    char intervals[512];
    core::output::TakeIntervals(intervals, sizeof(intervals)); // O1g: how evenly the frames were shown
    if (intervals[0]) {
        core_log("intervals%s (ms): %s", why, intervals);
    }
    char cpu[512];
    core::diag::env::TakeWindow(cpu, sizeof(cpu)); // O1a: processor time actually given to our threads
    if (cpu[0]) {
        core_log("cpu%s: %s", why, cpu);
    }
    g_loops = 0;
    g_ticks = 0;
    g_since = now;
}

int WINAPI GameMain_detour(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    int r = o_GameMain(inst, prev, cmd, show);
    emperor::fullscreen::OnGameEnd();
    core::capture::Flush(15000); // O1: screenshots still being written (e.g. F12 just before Exit)
    LogRates(" (game ended)");
    core_log("emperor: game_main returned %d", r);
    core_timer_end();
    return r;
}

DWORD g_lastLoop = 0;
bool g_paused = false;
bool g_drawOnly = false; // a frame drawn while paused: no simulation tick
DWORD g_lastDrawOnly = 0;
core::diag::Feature g_drawFrames{"draw-only frames while the panel asks (games/emperor)"};

struct DrawOnlyCall {
    int result;
    core::diag::Restore r;
};

// One frame drawn while the panel is open (sim_tick_due says no). g_drawOnly is in c.r: false again if abandoned.
void DrawOnlyFrame(DrawOnlyCall &c)
{
    emperor::resolution::OnLoop();
    emperor::fullscreen::OnLoop();
    c.r.Set(&g_drawOnly, true);
    core::diag::GameFrameBegin();
    c.result = Original(o_LoopStep, kLoopStep)();
    core::diag::GameFrameEnd();
    c.r.Undo();
}

int __cdecl LoopStep_detour()
{
    if (core::ui::IsOpen()) {
        if (!g_paused) {
            g_paused = true;
            LogRates(" (panel opened: game paused)");
        }
        // While the panel asks whether to keep a new resolution, the game draws frames (at most ~60 per second) so
        // the new layout can be seen; sim_tick_due says no, so the city does not move on.
        DWORD now = timeGetTime();
        if (core::ui::WantsGameFrames() && now - g_lastDrawOnly >= 16 && core::diag::Enabled(g_drawFrames)) {
            g_lastDrawOnly = now;
            DrawOnlyCall c{0, {}};
            if (core::diag::Guarded(g_drawFrames, [](void *p) { DrawOnlyFrame(*static_cast<DrawOnlyCall *>(p)); },
                                    &c, &c.r)) {
                return c.result;
            }
            core_log("emperor: no more draw-only frames after an error: the game stays paused while the panel asks");
            return 0;
        }
        Sleep(5);
        return 0;
    }
    if (g_paused) {
        g_paused = false;
        core_log("emperor: panel closed: game resumed");
        g_since = GetTickCount();
        g_loops = g_ticks = 0;
        g_lastLoop = 0; // the pause is not a frame time
        emperor::scroll::Resume();
    }
    ++g_loops;
    DWORD now = timeGetTime();
    if (g_lastLoop != 0) {
        emperor::scroll::OnLoop(static_cast<double>(now - g_lastLoop));
    }
    g_lastLoop = now;
    emperor::resolution::OnLoop();
    emperor::fullscreen::OnLoop();
    emperor::ui_fixes::OnLoop();
    emperor::display_options::OnLoop();
    core::diag::GameFrameBegin(); // frame check (core/diag/report.h): presents within one pass
    core::diag::env::NoteCore(core::diag::env::Thread::Game);
    const std::int64_t t0 = perf::Now();
    // O1: a city pass of a second or more is sampled (core/diag/stall); other screens load files in one pass.
    const bool watched = *kGameScreen == 1;
    if (watched) {
        core::output::MeasureFromNow(); // the output's frames per second, measured in the city (once per change)
    }
    if (watched) {
        core::diag::stall::Begin(core::diag::stall::Thread::Game);
    }
    int r = Original(o_LoopStep, kLoopStep)();
    if (watched) {
        core::diag::stall::End(core::diag::stall::Thread::Game);
    }
    perf::Add(kPassPhase, perf::Now() - t0);
    perf::EndSample(perf::Thread::Game);
    core::diag::GameFrameEnd();
    if (g_since == 0) {
        g_since = GetTickCount();
    }
    if (int report = core::diag::TakeReportRequest()) { // the game's part of a problem report
        char why[40];
        std::snprintf(why, sizeof(why), " (report #%d)", report);
        LogRates(why);
        emperor::scroll::SaveCityBuffer(report);
        char state[256];
        DescribeForCanary(state, sizeof(state));
        core_log("report #%d: %s", report, state);
    }
    int speed = *kGameSpeed;
    if (speed != g_lastSpeed) {
        // A new speed: close the current window so every rate line belongs to one speed.
        if (g_lastSpeed >= 0) {
            LogRates(" (before speed change)");
        }
        core_log("emperor: game speed %d%% (was %d%%)", speed, g_lastSpeed);
        g_lastSpeed = speed;
        g_since = GetTickCount();
        g_loops = g_ticks = 0;
    }
    if (GetTickCount() - g_since >= kRateLogMs) {
        LogRates("");
    }
    return r;
}

// One flag read (nothing that can fault), so not guarded.
// ---------------------------------------------------------------------------------------------
// Simulation pace (docs/simulation.md). The game runs at most one tick per game_frame and sets its tick time to "now"
// when it runs one, so a frame that takes 10 ms stretches every interval to the next frame (80 % at 2560x1440 in a
// large city: 22.6 ticks/s instead of 26.4). As Augustus does (game_speed_get_elapsed_ticks): the time owed is kept,
// and the ticks owed run in the same frame, at most 20, the left-over milliseconds carried to the next frame.
//   sim_tick_due still decides whether a tick may run (pause, screen, scrolling, multiplayer); only the time test is
//   ours: its tick time is set so that its own test agrees with ours, and set back to our clock afterwards.
//   The extra ticks run right after the game's own one, each as game_frame runs one: counter, sim_tick, then
//   sim_after_tick 0x535770 (game_frame calls it after every tick; for the last one it does so itself).
// 100 % (2 ms, "as fast as it can"): capped to 16 ms (Augustus's 100 %) unless gameplay.full_speed = original.
// gameplay.simulate_while_scrolling (default on): view_scrolling is 0 while sim_tick_due decides (the game itself pauses
// while scrolling).

constexpr int kMaxTicksPerFrame = 20;
// Falling behind (O1, docs/optimization.md): a player's log showed passes every 70 ms (the output blocked) and 300+
// ticks made up per 10 s -- three or four ticks per picture, so the city jumped instead of moving. While the smoothed
// time between passes is above kBehindMs, a pass runs at most kBehindTicks ticks and what is owed beyond them is
// dropped (the game runs slower, as the original does when frames are slow); below kCaughtUpMs it catches up again.
constexpr double kBehindMs = 50.0;    // fewer than 20 passes a second
constexpr double kCaughtUpMs = 35.0;  // about 28 passes a second
constexpr int kBehindTicks = 2;
constexpr DWORD kCappedMs = 16;
core::diag::Feature g_paceFeature{"simulation pace (games/emperor)"};
bool g_catchUp = true;
bool g_fullSpeedCapped = true; // gameplay.full_speed (default capped, playtest 2026-10-25)
bool g_simWhileScrolling = true; // gameplay.simulate_while_scrolling (default on, playtest 2026-10-25)


struct DueCall {
    int result;
    core::diag::Restore r;
};

void DueBody(DueCall &c)
{
    g_pace.extra = 0;
    if (g_simWhileScrolling) {
        c.r.Set(kViewScrolling.ptr(), 0);
    }
    int idx = (100 - *kGameSpeed) / 10;
    idx = idx < 0 ? 0 : idx;
    const bool full = idx == 0;
    if (!g_catchUp || idx >= 10 || (full && !g_fullSpeedCapped)) {
        g_pace.valid = false;
        c.result = Original(o_SimDue, kSimDue)();
        c.r.Undo();
        return;
    }
    const DWORD interval = full ? kCappedMs : static_cast<DWORD>(reinterpret_cast<const int *>(
                                                  kTickTable + emperor::g_imageShift)[idx] + 2);
    const DWORD now = timeGetTime();
    if (!g_pace.valid) {
        g_pace.last = *kSimLastTick;
        g_pace.valid = true;
    }
    if (g_pace.lastPass != 0) {
        double dt = static_cast<double>(now - g_pace.lastPass);
        dt = dt > 200 ? 200 : dt; // a pause (panel, loading) counts as one slow pass, not as many
        g_pace.passMs = g_pace.passMs * 0.9 + dt * 0.1;
        if (!g_pace.behind && g_pace.passMs > kBehindMs) {
            g_pace.behind = true;
            g_pace.droppedSpell = 0;
            core_log("emperor: falling behind (a pass every %.0f ms): at most %d ticks per pass, the rest is dropped "
                     "(the game runs slower instead of jumping)", g_pace.passMs, kBehindTicks);
        }
        else if (g_pace.behind && g_pace.passMs < kCaughtUpMs) {
            g_pace.behind = false;
            core_log("emperor: no longer behind (a pass every %.0f ms): ticks are made up again; %ld dropped",
                     g_pace.passMs, g_pace.droppedSpell);
        }
    }
    g_pace.lastPass = now;
    if (static_cast<int>(now - g_pace.last) < 0) {
        g_pace.last = now - interval; // the clock went back: one tick owed
    }
    const bool owed = now - g_pace.last >= interval;
    // The game's own time test agrees with ours: far in the past when a tick is owed, now when not.
    c.r.Set(kSimLastTick.ptr(), owed ? now - 100000 : now);
    c.result = Original(o_SimDue, kSimDue)();
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
        g_pace.extra = *kFastForward ? 0 : static_cast<int>(n) - 1;
        if (g_pace.extra + 1 > g_pace.mostInOnePass) {
            g_pace.mostInOnePass = g_pace.extra + 1;
        }
    }
    else if (owed) {
        g_pace.last = now - interval; // held back (pause, another screen, scrolling): one tick as soon as allowed
    }
    *kSimLastTick = g_pace.last;
}

int __cdecl SimDue_detour()
{
    if (g_drawOnly) {
        return 0;
    }
    if (GameAddress(__builtin_return_address(0)) != kDueFromFrame || !core::diag::Enabled(g_paceFeature)) {
        return Original(o_SimDue, kSimDue)(); // another frame loop (0x5034e0), or switched off after an error
    }
    DueCall c{0, {}};
    if (core::diag::Guarded(g_paceFeature, [](void *p) { DueBody(*static_cast<DueCall *>(p)); }, &c, &c.r)) {
        return c.result;
    }
    g_pace = Pace{};
    core_log("emperor: simulation pace back to the game's own after an error");
    return Original(o_SimDue, kSimDue)();
}

void ExtraTicks(int n)
{
    const char screen = *kGameScreen;
    for (int k = 0; k < n; ++k) {
        if (*kFastForward || *kSimPaused || *kGameScreen != screen) {
            break; // the last tick opened a message, paused the game or changed the screen
        }
        Function<void(__cdecl *)()>(kAfterTick)(); // what game_frame does after each tick
        *kTickCounter = *kTickCounter + 1;
        ++g_ticks;
        ++g_pace.caughtUp;
        o_SimTick();
    }
}

void __cdecl SimTick_detour()
{
    ++g_ticks;
    perf::Scope timed(kSimPhase);
    o_SimTick();
    if (g_pace.extra > 0 && GameAddress(__builtin_return_address(0)) == kTickFromFrame &&
        core::diag::Enabled(g_paceFeature)) {
        int n = g_pace.extra;
        g_pace.extra = 0;
        if (!core::diag::Guarded(g_paceFeature, [](void *p) { ExtraTicks(*static_cast<int *>(p)); }, &n)) {
            g_pace = Pace{};
            core_log("emperor: simulation pace back to the game's own after an error in a tick made up");
        }
    }
}

void __cdecl ScreenPresent_detour(int param)
{
    emperor::fullscreen::OnPresent();
    if (param == 1 || *kPresentForce != 0) {
        core::output::HoldNextFrame(kPresentGateMs);
    }
    else if (core::output::FrameWanted()) {
        param = 1; // present the frame just drawn: the output thread is ready for it
    }
    o_ScreenPresent(param);
}

bool ApplySmooth(int, const char *v, bool initial)
{
    return initial || emperor::scroll::SetSmooth(std::strcmp(v, "on") == 0);
}

bool ApplyResolution(int, const char *, bool initial)
{
    return initial || emperor::resolution::SettingsChanged(core_config_get());
}

// [ui] info_window (R2b, docs/archive/resolution_ui_r2b.md §3): registered by this layer before the file is loaded.
const char *const kInfoWindow[] = {"original", "centered", "cursor"};
const char *const kFullSpeed[] = {"capped", "original"};
const char *const kFullSpeedLabels[] = {"capped: steady, a step above 90%", "original: as fast as the computer can"};
const char *const kInfoWindowLabels[] = {"original (above or below, as the game does it)",
                                         "centered in the city area", "beside the cursor"};

bool ApplyInfoWindow(int, const char *v, bool)
{
    int i = 0;
    while (i < 2 && std::strcmp(v, kInfoWindow[i]) != 0) {
        ++i;
    }
    emperor::resolution::SetInfoMode(static_cast<emperor::resolution::InfoMode>(i));
    return true;
}

// The game's state for a canary report (core/ddraw/surface_memory.h; game thread, reads only).
void DescribeForCanary(char *out, size_t size)
{
    std::snprintf(out, size,
                  "screen 0x%02x, mode %dx%d, city clip (%d,%d)-(%d,%d), view %d x %d (tiles %dx%d%s)",
                  static_cast<unsigned char>(*kGameScreen), *kScreenW, *kScreenH, *kCityClipLeft, *kCityClipTop,
                  *kCityClipRight, *kCityClipBottom, *kViewCols, *kViewRows, *kTileW, *kTileHalfH * 2,
                  *kSmallTiles == 1 ? ", small tiles" : "");
}

// Screenshot file names start with the current screen (game thread).
void ShotTag(char *out, size_t size)
{
    std::snprintf(out, size, "screen_0x%02x", static_cast<unsigned char>(*kGameScreen));
}

// Panel section (drawn on the output thread; reads only).
void InfoSection()
{
    static const char *const kScreens[] = {"front end", "city"};
    SIZE mode = core::display::ModeSize();
    int screen = *kGameScreen;
    ImGui::Text("Emperor: Rise of the Middle Kingdom 1.0.1.0");
    ImGui::Text("Logical resolution: %ld x %ld", mode.cx, mode.cy);
    ImGui::Text("Game speed: %d%%", *kGameSpeed);
    ImGui::Text("Screen: %s (%d, 0x%02x)", screen == 1 ? kScreens[1] : kScreens[0], screen, screen);
    char off[512];
    core::diag::SwitchedOff(off, sizeof(off));
    if (off[0]) {
        ImGui::TextWrapped("Switched off after an error (see ddraw_proxy.log): %s", off);
    }
}

} // namespace

namespace emperor {

const Detour *MainDetours(int *count)
{
    static const Detour kDetours[] = {
        {&kLoopStep, reinterpret_cast<void *>(&LoopStep_detour)},
        {&kSimDue, reinterpret_cast<void *>(&SimDue_detour)},
    };
    *count = static_cast<int>(sizeof(kDetours) / sizeof(kDetours[0]));
    return kDetours;
}

} // namespace emperor

extern "C" void emperor_register_settings(void)
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "ui", "info_window", "Building info window",
        "Where the window opens when you right-click a building. original: as the game does it (above or below, "
        "moved with the center of the city area); centered: in the middle of the city area; cursor: beside the "
        "cursor, never past the edges. Applies to the next window you open.",
        Type::Choice, "cursor", Effect::Live, true, true, 0, 0, 0, nullptr, kInfoWindow, kInfoWindowLabels, 3});
    core::settings::OnChange(i, &ApplyInfoWindow);
    i = core::settings::Register(Item{
        "gameplay", "keep_speed", "Keep the game speed",
        "on: ticks owed because a frame took long (large city, high resolution) are made up in the next frame, as "
        "Augustus does, so the game runs at its set speed at any resolution; off: the game's own pace (one tick per "
        "frame at most, slower when frames are slow).",
        Type::Bool, "on", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_catchUp = std::strcmp(v, "on") == 0;
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
        g_simWhileScrolling = std::strcmp(v, "on") == 0;
        return true;
    });
    emperor::china_map::RegisterSettings();
    emperor::fullscreen::RegisterSettings();
}

extern "C" int emperor_editor_exe(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return 0;
    }
    const char *name = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
    }
    return lstrcmpiA(name, "EmperorEdit.exe") == 0;
}

extern "C" void emperor_attach(void)
{
    const core_config *cfg = core_config_get();
    bool threaded = cfg->backend == CORE_BACKEND_EMULATED && cfg->present != CORE_PRESENT_ORIGINAL;
    const Site *sites[] = {&kGameMain, &kLoopStep, &kSimTick, &kScreenPresent, &kSimDue};
    for (const Site *s : sites) {
        if (!Matches(*s)) {
            core_log("emperor: %s at 0x%lx does not match the known Emperor.exe; no game hooks installed%s", s->name,
                     s->address, threaded ? " (present falls back to the game's own ~20 FPS)" : "");
            return;
        }
    }
    Install(kGameMain, &GameMain_detour, o_GameMain);
    Install(kLoopStep, &LoopStep_detour, o_LoopStep);
    Install(kSimTick, &SimTick_detour, o_SimTick);
    Install(kSimDue, &SimDue_detour, o_SimDue);
    if (!Matches(kAfterTick)) {
        core_log("emperor: %s at 0x%lx does not match: the game's own simulation pace", kAfterTick.name,
                 kAfterTick.address);
        g_catchUp = false;
        core::diag::Disable(g_paceFeature, "sim_after_tick does not match");
    }
    if (threaded) {
        Install(kScreenPresent, &ScreenPresent_detour, o_ScreenPresent);
    }
    core_log("emperor: known build; game hooks installed (screen_present %s)",
             threaded ? "hooked: frames follow the display" : "not hooked: the game's own presentation");
    bool scroll = emperor::scroll::Attach(cfg->smooth_scroll != 0);
    bool resolution = emperor::resolution::Attach(cfg);
    if (resolution) {
        emperor::fullscreen::Attach();
        emperor::china_map::Attach(); // needs the emulated DirectDraw's resolutions (the map only fits beyond 1024)
        emperor::display_options::Attach(); // R3: our display settings in the game's Display options
    }
    emperor::ui_fixes::Attach();
    // Settings this layer applies when they change in our panel (core/settings/settings.h).
    if (scroll) {
        core::settings::OnChange(core::settings::Find("gameplay", "smooth_scroll"), &ApplySmooth);
    }
    if (resolution) {
        core::settings::OnChange(core::settings::Find("display", "resolution"), &ApplyResolution);
        core::settings::OnChange(core::settings::Find("display", "scale"), &ApplyResolution);
    }
    core::capture::SetTagProvider(&ShotTag);
    core::ddraw::SetStateDescriber(&DescribeForCanary);
    core::ui::SetTitle("Yongle " YONGLE_VERSION);
    core::ui::AddSection("Game", &InfoSection);
    core::ui::AddSection("Performance", &core::ui::PerfWidget);
}
