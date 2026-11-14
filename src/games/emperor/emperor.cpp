// Game hooks for Emperor. The addresses are in the Site and Var declarations.
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
//      animation, the simulation and scrolling run on their own timers, so presenting more
//      often changes no game timing.
//   Screenshots (core/capture/screenshot.h): the file name starts with game_screen, e.g. screen_0x14
//      (game_screen as a hex number).
//   Smooth scrolling: games/emperor/scroll.cpp.
//   Custom logical resolution: games/emperor/resolution.cpp.
//   Framed full-screen pictures: games/emperor/fullscreen.cpp.
//   Our settings panel (core/ui/panel.h): while it is open game_loop_step is not called, so the game is paused
//      (no simulation, drawing or input handling); the main loop keeps pumping messages and sleeps 5 ms per
//      iteration. game_main ignores game_loop_step's return value (0x475a4b: call, then jmp past WaitMessage).
//      sim_tick_due sets its last tick to "now", so the simulation does not catch up afterwards.
//   Failure: the draw-only frames (the game draws while our panel asks to
//      keep a new resolution) run under core/sim/panel_pause's guard; on an exception the frame is abandoned, the
//      draw-only flag goes back to false and the game simply stays paused while the panel asks. Settings callbacks are guarded by
//      core/settings. Normal frames are the game's own and are not guarded.
#include "games/emperor/emperor.h"

#include "core/ui/info_window.h"
#include "core/capture/screenshot.h"
#include "core/config.h"
#include "core/ddraw/surface_memory.h"
#include "core/diag/autorun.h"
#include "core/diag/environment.h"
#include "core/diag/guard.h"
#include "core/diag/rate_log.h"
#include "core/diag/perf.h"
#include "core/diag/present_mode.h"
#include "core/diag/report.h"
#include "core/diag/stall.h"
#include "core/display/virtual_display.h"
#include "core/display/window_key.h"
#include "core/log.h"
#include "core/output/sdl_output.h"
#include "core/platform/timing.h"
#include "core/settings/settings.h"
#include "core/sim/pace.h"
#include "core/sim/panel_pause.h"
#include "core/ui/panel.h"
#include "core/ui/perf_view.h"
#include "core/ui/settings_view.h"
#include "games/emperor/china_map.h"
#include "games/emperor/city_picture.h"
#include "games/emperor/display_options.h"
#include "games/emperor/fullscreen.h"
#include "games/emperor/game_menu.h"
#include "games/emperor/nature_sounds.h"
#include "games/emperor/master_volume.h"
#include "games/emperor/keys.h"
#include "games/emperor/notice.h"
#include "games/emperor/options_entry.h"
#include "games/emperor/options_original.h"
#include "games/emperor/options_window.h"
#include "games/emperor/resolution.h"
#include "games/emperor/scroll.h"
#include "games/emperor/sound.h"
#include "games/emperor/intro.h"
#include "games/emperor/zoom.h"
#include "games/emperor/site.h"
#include "games/emperor/smoke.h"
#include "games/emperor/ui_fixes.h"
#include "ini_template.h" // generated from src/games/emperor/emperor_engine.ini (CMakeLists.txt)

#include <imgui.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstring>

#ifndef ENGINE_VERSION
#define ENGINE_VERSION "dev" // set by CMakeLists.txt for the DLL; the offline tests build without it
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
// Simulation pace: sim_tick_due's inputs and what game_frame does around sim_tick.
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
// For canary reports: the city view as viewport_init 0x469820 left it.
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

// Where the time goes (core/diag/perf.h): a game pass is one game_loop_step; the simulation is sim_tick.
// The city's own drawing and our copies are timed in games/emperor/scroll and resolution.
namespace perf = core::diag::perf;
const int kPassPhase = perf::Register("game pass", perf::Thread::Game, true);
const int kSimPhase = perf::Register("simulation (sim_tick)", perf::Thread::Game);

// The simulation pace's view of Emperor (core/sim/pace.h; SimDue_detour below).
core::diag::Feature g_paceFeature{"simulation pace (games/emperor)"};
const core::sim::PaceGame kPace{
    "emperor",
    "pass",
    &g_paceFeature,
    [] { return kSimLastTick.ptr(); },
    [] { return kViewScrolling.ptr(); },
    [] { return *kFastForward != 0; },
    [] { return static_cast<int>(*kGameSpeed); },
    [](int idx) { return reinterpret_cast<const int *>(kTickTable + emperor::g_imageShift)[idx]; },
};

// Counters and rate log (game thread only).
long g_loops = 0, g_ticks = 0;
DWORD g_since = 0;
int g_lastSpeed = -1;

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
    char paceNote[128];
    core::sim::TakeNote(kPace, paceNote, sizeof(paceNote));
    long multi = 0, torn = 0;
    core::diag::TakeIntegrity(&multi, &torn);
    static const char *const kPresent[] = {"original", "display", "cap60"};
    double s = ms / 1000.0;
    int mapCols = 0, mapRows = 0;
    emperor::zoom::MapRange(&mapCols, &mapRows); // a log analysis groups the windows by these
    core_log("rates%s: loop %.0f/s, sim ticks %.1f/s, game presents %.1f/s, frames shown %.1f/s over %lu ms "
             "(present=%s, display %d Hz, game speed %d%%, screen %d, city zoom %d%% on map %dx%d %s, timer %u us); "
             "frame check: %ld game frames with 2+ presents, %ld torn%s%s",
             why, g_loops / s, g_ticks / s, presented / s, shown / s, ms, kPresent[core_config_get()->present],
             core::output::RefreshHz(), *kGameSpeed, static_cast<int>(*kGameScreen), emperor::zoom::Percent(), mapCols,
             mapRows, emperor::zoom::AverageShrink() ? "average" : "nearest", core_timer_resolution_us(), multi, torn,
             scroll, paceNote);
    core::diag::LogRateTail(why); // time, time max, intervals, cpu (the same lines in every game)
    g_loops = 0;
    g_ticks = 0;
    g_since = now;
}

int WINAPI GameMain_detour(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    int r = o_GameMain(inst, prev, cmd, show);
    emperor::fullscreen::OnGameEnd();
    core::capture::Flush(15000); // screenshots still being written (e.g. F12 just before Exit)
    LogRates(" (game ended)");
    core_log("emperor: game_main returned %d", r);
    core_timer_end();
    return r;
}

DWORD g_lastLoop = 0;

// While the panel or our options window is open (core/sim/panel_pause, shared with Zeus): the game is paused; while
// the panel asks whether to keep a new resolution, or while the options window is open (it is drawn over each frame),
// the game draws frames (at most ~60 per second) and sim_tick_due says no, so the city does not move on.
void Paused(bool options)
{
    emperor::sound::SetQuiet(true); // no new ambient sound meanwhile, as under the game's own dialogs
    LogRates(options ? " (options window opened: game paused)" : " (panel opened: game paused)");
}

void Resumed()
{
    emperor::sound::SetQuiet(false);
    core_log("emperor: panel or options window closed: game resumed");
    g_since = GetTickCount();
    g_loops = g_ticks = 0;
    g_lastLoop = 0; // the pause is not a frame time
    emperor::scroll::Resume();
}

// One frame drawn while paused (sim_tick_due says no).
int DrawOnlyFrame()
{
    emperor::resolution::OnLoop();
    emperor::fullscreen::OnLoop();
    core::diag::GameFrameBegin();
    const int r = Original(o_LoopStep, kLoopStep)();
    core::diag::GameFrameEnd();
    return r;
}

const core::sim::panel_pause::PauseGame kPause("emperor", &emperor::options_window::IsOpen, &Paused, &Resumed,
                                               &DrawOnlyFrame);

int __cdecl LoopStep_detour()
{
    int paused = 0;
    if (core::sim::panel_pause::Loop(kPause, &paused)) {
        return paused;
    }
    if (emperor::game_menu::OnLoop() && emperor::options_window::IsOpen()) {
        return 0; // a confirmation answered No opened the pause menu again: no pass (the game stays paused)
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
    emperor::sound::OnLoop(); // sounds paused while away go on; ambient sounds per second; counts
    core::diag::GameFrameBegin(); // frame check (core/diag/report.h): presents within one pass
    core::diag::env::NoteCore(core::diag::env::Thread::Game);
    const std::int64_t t0 = perf::Now();
    // A city pass of a second or more is sampled (core/diag/stall); other screens load files in one pass.
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

// ---------------------------------------------------------------------------------------------
// The unattended smoke test (core/diag/autorun.h): the game's screen, its tick count and its own way to end.
// game_quit 0x10c7205: 1 makes game_main's loop send WM_CLOSE, which the window procedure takes (game_quit = 3, then
// DefWindowProc and WM_DESTROY's PostQuitMessage): the game's own exit, Emperor.inf written as always, no question
// asked (Ghidra; the same as Zeus's quit_request). /sg=<save> on the command line loads a save on the
// first frame (cmdline_parse 0x474f10, as Zeus's). The steps run in ScreenPresent_detour (below).

const Var<char> kGameQuit{0x10c7205};

int AutorunScreen()
{
    return static_cast<unsigned char>(*kGameScreen);
}

long AutorunTicks()
{
    return *kTickCounter;
}

void AutorunQuit()
{
    *kGameQuit = 1;
}

// game_main calls game_loop_step only when app_paused 0x10de458 is 0 and app_active 0x10de45c is not, and only on a
// pass that dispatched no message or a click (Ghidra).
const Var<int> kAppActive{0x10de45c}, kAppPaused{0x10de458};
// The game's mouse (input_update_mouse) and its scrolling flag, for the smoke test's clicks.
const Var<int> kMouseX{0x10de0a0}, kMouseY{0x10de09c};
const Var<char> kMouseLeftHeld{0x10de06c}; // (view_scrolling: kViewScrolling above)

void AutorunDescribe(char *out, int size)
{
    std::snprintf(out, size, "game_screen 0x%02x, app_active %d, app_paused %d, game_quit %d, game mouse (%d,%d) left "
                             "held %d, view scrolling %d",
                  static_cast<unsigned char>(*kGameScreen), *kAppActive, *kAppPaused, static_cast<int>(*kGameQuit),
                  *kMouseX, *kMouseY, static_cast<int>(*kMouseLeftHeld), *kViewScrolling);
}

// One flag read (nothing that can fault), so not guarded.
// ---------------------------------------------------------------------------------------------
// Simulation pace (core/sim/pace.h): the logic is the core's, shared with every
// game layer; here are Emperor's places. Only game_frame's own calls (return addresses 0x536ba8, 0x536bc5) go through
// it; the extra ticks run right after the game's own one, each as game_frame runs one: sim_after_tick 0x535770
// (game_frame calls it after every tick; for the last one it does so itself), the counter, sim_tick.
// gameplay.simulate_while_scrolling: view_scrolling is 0 while sim_tick_due decides (the game itself pauses while
// scrolling).

int OriginalDue()
{
    return Original(o_SimDue, kSimDue)();
}

int __cdecl SimDue_detour()
{
    if (core::sim::panel_pause::DrawOnly()) {
        return 0;
    }
    if (GameAddress(__builtin_return_address(0)) != kDueFromFrame) {
        return OriginalDue(); // another frame loop (0x5034e0)
    }
    return core::sim::Due(kPace, &OriginalDue);
}

char g_tickScreen = 0; // the screen when the game's own tick ran: the ticks made up stop when it changes

bool ExtraTickAllowed()
{
    return !*kFastForward && !*kSimPaused && *kGameScreen == g_tickScreen; // a message, the pause, another screen
}

void ExtraTick()
{
    Function<void(__cdecl *)()>(kAfterTick)(); // what game_frame does after each tick
    *kTickCounter = *kTickCounter + 1;
    ++g_ticks;
    o_SimTick();
}

void __cdecl SimTick_detour()
{
    ++g_ticks;
    perf::Scope timed(kSimPhase);
    o_SimTick();
    if (GameAddress(__builtin_return_address(0)) == kTickFromFrame) {
        g_tickScreen = *kGameScreen;
        core::sim::RunOwedTicks(kPace, &ExtraTickAllowed, &ExtraTick);
    }
}

void __cdecl ScreenPresent_detour(int param)
{
    emperor::fullscreen::OnPresent();
    emperor::options_window::OnPresent(GameAddress(__builtin_return_address(0))); // over the frame just drawn
    if (param == 1 || *kPresentForce != 0) {
        core::output::HoldNextFrame(kPresentGateMs);
    }
    else if (core::output::FrameWanted()) {
        param = 1; // present the frame just drawn: the output thread is ready for it
    }
    o_ScreenPresent(param);
    // The smoke test's steps, once per frame shown. Not in game_loop_step: on the main menu with no input the game
    // does not call it at all (game_main skips it on every pass that dispatched a message, and WM_PAINT keeps
    // coming), and the menu is drawn only by WM_PAINT's screen_present(1) (seen in the smoke test).
    core::diag::autorun::Frame();
}

bool ApplySmooth(int, const char *v, bool initial)
{
    return initial || emperor::scroll::SetSmooth(std::strcmp(v, "on") == 0);
}

bool ApplyResolution(int, const char *, bool initial)
{
    return initial || emperor::resolution::SettingsChanged(core_config_get());
}

// The key filter (our DirectDraw's window only; installed also without resolution): F5 and Alt+Enter are our window
// mode (core/display/window_key.h; F5 is also what the Display options button "Windowed screen" posts), so
// screen_fullscreen stays 1 and Emperor.inf only ever holds the original's own value; then F6 and Shift+F7 while
// resolution is installed.
bool KeyFilter(UINT message, WPARAM key, LPARAM l)
{
    return core::display::WindowKey(message, key, l, "resolution") || // the log's prefix as before
           emperor::resolution::KeyFilter(message, key, l);
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
        Type::Choice, "cursor", Effect::Live, true, true, 0, 0, 0, nullptr, core::ui::info_window::kChoices,
        core::ui::info_window::kChoiceLabels, core::ui::info_window::kChoiceCount});
    core::ui::info_window::Watch(i, "resolution: "); // the choices and the mode are the core's (shared with Zeus)
    core::sim::RegisterSettings(); // [gameplay] keep_speed, full_speed, simulate_while_scrolling
    emperor::china_map::RegisterSettings();
    emperor::fullscreen::RegisterSettings();
    emperor::master_volume::RegisterSettings(); // first in [sound]: the volume over all
    emperor::sound::RegisterSettings();
    emperor::nature_sounds::RegisterSettings(); // 0.2.0 item 10
    emperor::intro::RegisterSettings();
    emperor::zoom::RegisterSettings();
    emperor::city_picture::RegisterSettings();
    emperor::keys::RegisterSettings(); // 0.2.0 item 7: [keys], one setting per action
    // The settings file this version ships: an older file gets its descriptions (CMake embeds the file).
    core::settings::SetTemplate(emperor::kIniTemplate);
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
        core::sim::TurnOff(kPace, "sim_after_tick does not match");
    }
    if (threaded) {
        Install(kScreenPresent, &ScreenPresent_detour, o_ScreenPresent);
    }
    core_log("emperor: known build; game hooks installed (screen_present %s)",
             threaded ? "hooked: frames follow the display" : "not hooked: the game's own presentation");
    // The smoke test's steps run in screen_present's detour, hooked only with the output thread, which its shots
    // need anyway (main menu 0x00, city 0x01).
    if (threaded) {
        core::diag::autorun::Start({&AutorunScreen, 0x00, 0x01, &AutorunTicks, &AutorunQuit, &AutorunDescribe,
                                    emperor::smoke::kActions, &emperor::smoke::Action, &emperor::smoke::DialogOffset});
    }
    bool scroll = emperor::scroll::Attach(cfg->smooth_scroll != 0);
    bool resolution = emperor::resolution::Attach(cfg);
    core::display::SetKeyFilter(&KeyFilter);
    if (resolution) {
        if (emperor::fullscreen::Attach() && !scroll) {
            // The menus' city background is taken in smooth scrolling's city_render hook (so its absence is logged).
            core_log("fullscreen: no city behind the menus (smooth scrolling's city_render hook is not installed): the "
                     "panel pattern around them, or your own menu_background picture");
        }
        emperor::china_map::Attach(); // needs the emulated DirectDraw's resolutions (the map only fits beyond 1024)
        emperor::display_options::Attach(); // our display settings in the game's Display options
        if (scroll) {
            emperor::zoom::Attach(); // drawn by smooth scrolling's city render, laid out by resolution's viewport
        }
        else {
            core_log("zoom: not installed (smooth scrolling's hooks are not): the city stays at 100%%");
        }
        emperor::city_picture::Attach(scroll); // 0.2.0: the city and minimap pictures (scroll's city_render hook)
        if (threaded) {
            emperor::options_window::Attach(); // drawn in screen_present, which is hooked only then
        }
    }
    emperor::ui_fixes::Attach();
    emperor::notice::Attach(); // 0.2.0 item 6: our messages in the game's notice boxes
    emperor::sound::Attach(); // [sound] background_sound, ambient_sounds; pan within 0..127
    emperor::options_original::Attach(); // the game's own options; a new player's starting speeds
    emperor::master_volume::Attach();    // [sound] master_volume
    emperor::game_menu::Attach();        // Esc and the pause menu (after the window; before the File menu)
    emperor::options_entry::Attach();    // after the window and options_original: the menus open the window
    emperor::keys::Attach();             // 0.2.0 item 7: the key bindings (F6 opens the options window)
    emperor::intro::Attach(); // 0.2.0: [ui] intro_movie (Emperor.ini is not changed)
    // Settings this layer applies when they change in our panel (core/settings/settings.h).
    if (scroll) {
        core::settings::OnChange(core::settings::Find("gameplay", "smooth_scroll"), &ApplySmooth);
    }
    if (resolution) {
        core::settings::OnChange(core::settings::Find("video", "resolution"), &ApplyResolution);
        core::settings::OnChange(core::settings::Find("video", "scale"), &ApplyResolution);
    }
    core::capture::SetTagProvider(&ShotTag);
    core::ddraw::SetStateDescriber(&DescribeForCanary);
    // The present mode log's lines carry game_screen (read on the drawing thread; one byte, reads only).
    core::diag::present_mode::SetScreenProvider([] { return static_cast<int>(static_cast<unsigned char>(*kGameScreen)); });
    core::ui::SetTitle("Yongle " ENGINE_VERSION " debug panel");
    core::ui::AddSection("Game", &InfoSection);
    core::ui::AddSection("Performance", &core::ui::PerfWidget);
}
