// Offline test of core/diag/present_mode (not the game; 2026-11-07): the tracker's mode changes, failed queries
// counted (each HRESULT reported once) without giving up, giving up only after 10 s and 100 failures in a row, the
// change lines per second, the kept events and their lines, the 60-s line, its reset and the report's copy; Attach with
// no Direct3D 11 swap chain (lines, the log stays on) and Report; the screen programs list; the window background test
// color on a window of our own (set, kept, restored). Writes test_present_mode.log next to the exe and checks its lines.
#include "core/diag/present_mode.h"
#include "core/diag/screen_programs.h"
#include "core/diag/window_background.h"
#include "core/log.h"
#include "core/platform/affinity.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

// The DLL's affinity module is not part of this test: our thread needs no mask here.
namespace core::platform::affinity {
void OurThread(const char *) {}
} // namespace core::platform::affinity

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

namespace pm = core::diag::present_mode;

constexpr long kDisjoint = static_cast<long>(0x887A000BL);
constexpr long kInvalid = static_cast<long>(0x887A0001L);

void TestTracker()
{
    pm::Tracker t;
    int from = 99;
    bool first = true;
    Check(t.Current() == pm::kUnknown, "starts unknown");
    Check(t.Query(true, pm::kComposed, 0, 10, 0, &from, &first) && from == pm::kUnknown && !first,
          "first answer: unknown -> composed");
    Check(!t.Query(true, pm::kComposed, 0, 10, 5, &from, &first), "the same mode again: no change");
    from = 99;
    Check(t.Query(true, pm::kOverlay, 0, 10, 10, &from, &first) && from == pm::kComposed &&
              t.Current() == pm::kOverlay,
          "composed -> overlay");
    // Failures: counted, each HRESULT once, the mode kept, no giving up for a short spell.
    Check(!t.Query(false, pm::kUnknown, kDisjoint, 10, 20, &from, &first) && first, "disjoint: reported the first time");
    Check(!t.Query(false, pm::kUnknown, kDisjoint, 10, 30, &from, &first) && !first, "disjoint again: not again");
    Check(!t.Query(false, pm::kUnknown, kInvalid, 10, 40, &from, &first) && first, "another error: reported");
    Check(t.Current() == pm::kOverlay && !t.GivingUp(), "the mode is kept, not giving up");
    Check(!t.Query(true, pm::kOverlay, 0, 10, 50, &from, &first), "success after the errors: same mode");
    for (int i = 0; i < 200; ++i) {
        t.Query(false, pm::kUnknown, kDisjoint, 10, 100 + i * 10, &from, &first);
    }
    Check(!t.GivingUp(), "200 failures within 2 s: not giving up");
    pm::Tracker u;
    for (int i = 0; i < 50; ++i) {
        u.Query(false, pm::kUnknown, kInvalid, 10, i * 1000, &from, &first);
    }
    Check(!u.GivingUp(), "50 failures over 49 s: not giving up (fewer than 100)");
    for (int i = 50; i < 100; ++i) {
        u.Query(false, pm::kUnknown, kInvalid, 10, i * 1000, &from, &first);
    }
    Check(u.GivingUp(), "100 failures in a row over 99 s: giving up");
    u.Query(true, pm::kComposed, 0, 10, 100000, &from, &first);
    Check(!u.GivingUp(), "a success resets the spell");
    Check(t.Query(true, 7, 0, 10, 60, &from, &first) && std::strcmp(pm::ModeName(7), "value 7") == 0,
          "an unknown value: a change, \"value 7\"");
}

void TestPeriod()
{
    pm::Tracker t;
    char line[512];
    int from;
    bool first;
    Check(t.PeriodDue(0) && !t.Period(1000, 10, 10, 1.0, "x", false, false, line, sizeof(line)),
          "the first call only starts");
    Check(!t.PeriodDue(30000), "not due after 29 s");
    for (int i = 0; i < 200; ++i) {
        t.Present();
        t.Query(true, i < 150 ? pm::kComposed : pm::kOverlay, 0, i == 0 ? 40 : 2, 1000 + i * 5, &from, &first);
    }
    t.Query(false, pm::kUnknown, kDisjoint, 2, 1990, &from, &first);
    Check(!t.Period(30000, 110, 110, 1.0, "x", false, false, line, sizeof(line)), "under 60 s: no line");
    // The report's copy: the figures so far, not started again.
    Check(t.Period(30000, 110, 110, 1.0, "x", true, true, line, sizeof(line)) &&
              std::string(line).find("200 presents") != std::string::npos,
          "forced, kept: the figures so far");
    Check(t.PeriodDue(61000), "due after 60 s");
    const bool made = t.Period(61000, 210, 205, 1.0, "covering the monitor, game screen 0x01", false, false, line,
                               sizeof(line));
    std::printf("  %s\n", line);
    const std::string s = line;
    Check(made, "after 60 s: a line");
    Check(s.find("60.0 s: 200 presents;") != std::string::npos, "the line: the span and our presents (kept copy did not reset)");
    Check(s.find("DWM") == std::string::npos, "the line: no DWM counts any more");
    Check(s.find("mode overlay (composed 150, overlay 50, none 0, composition failure 0, other 0, errors 1;") !=
              std::string::npos,
          "the line: the current mode and the frames per mode, errors");
    Check(s.find("changes 2, 0 of them not logged") != std::string::npos, "the line: the changes");
    Check(s.find("DXGI present count +200, present refresh count +195") != std::string::npos, "the line: DXGI counts");
    Check(s.find("query mean 2.2 us, max 40.0 us") != std::string::npos, "the line: the query's cost");
    Check(s.find("game screen 0x01") != std::string::npos, "the line: where and the screen");
    t.Period(121000, -1, -1, 1.0, "windowed", false, false, line, sizeof(line));
    const std::string s2 = line;
    Check(s2.find(": 0 presents") != std::string::npos && s2.find("composed 0, overlay 0") != std::string::npos &&
              s2.find("DXGI counts not known") != std::string::npos,
          "the next line: the counters started again");
}

void TestLinesAndEvents()
{
    pm::Tracker t;
    int from;
    bool first;
    // 25 changes in one second: 20 lines, 5 counted only; the next second starts again.
    int allowed = 0;
    for (int i = 0; i < 25; ++i) {
        if (t.Query(true, i % 2 ? pm::kOverlay : pm::kComposed, 0, 1, 3000 + i, &from, &first)) {
            allowed += t.ChangeLineAllowed(3000 + i);
        }
    }
    Check(allowed == pm::Tracker::kChangeLines, "25 changes in a second: 20 lines allowed");
    Check(t.Query(true, pm::kNone, 0, 1, 4001, &from, &first) && t.ChangeLineAllowed(4001),
          "the next second: lines allowed again");
    char line[512];
    t.Period(0, -1, -1, 1.0, "x", false, false, line, sizeof(line));
    t.Period(0, -1, -1, 1.0, "x", true, false, line, sizeof(line));
    Check(std::string(line).find("changes 26, 5 of them not logged") != std::string::npos,
          "the 60-s line: changes and those not logged");
    // Kept events: the oldest dropped after 64; the lines with their age.
    pm::Tracker e;
    Check(e.Remembered() == 0, "no events at first");
    e.Remember(1000, pm::kUnknown, pm::kOverlay, 0, 3, "covering the monitor; screen programs: none of the list");
    e.Remember(2500, pm::kUnknown, pm::kUnknown, kDisjoint, 7, "windowed");
    e.EventLine(0, 4000, line, sizeof(line));
    std::printf("  %s\n", line);
    Check(std::string(line) == "3.000 s ago: unknown -> overlay after 3 presents; covering the monitor; screen "
                               "programs: none of the list",
          "event line: a change");
    e.EventLine(1, 4000, line, sizeof(line));
    Check(std::string(line) == "1.500 s ago: the query failed (0x887a000b); windowed", "event line: a failure");
    for (int i = 0; i < 70; ++i) {
        e.Remember(5000 + i, pm::kComposed, pm::kOverlay, 0, i, "n");
    }
    Check(e.Remembered() == pm::Tracker::kEvents && e.RememberedTotal() == 72, "72 events: 64 kept");
    e.EventLine(0, 6000, line, sizeof(line));
    Check(std::string(line).find("after 6 presents") != std::string::npos, "the oldest kept is the 9th");
    e.EventLine(63, 6000, line, sizeof(line));
    Check(std::string(line).find("0.931 s ago") != std::string::npos && std::string(line).find("after 69") != std::string::npos,
          "the newest is the last");
}

void TestScreenPrograms()
{
    namespace sp = core::diag::screen_programs;
    Check(sp::Match(L"RadeonSoftware.exe") && std::strcmp(sp::Match(L"radeonsoftware.EXE"), "AMD Software") == 0,
          "match: AMD Software, any case");
    Check(sp::Match(L"PowerToys.AlwaysOnTop.exe") != nullptr && sp::Match(L"sunshine.exe") != nullptr &&
              sp::Match(L"Apollo.exe") != nullptr && sp::Match(L"Discord.exe") != nullptr &&
              sp::Match(L"RTSS.exe") != nullptr && sp::Match(L"obs64.exe") != nullptr &&
              sp::Match(L"GameBar.exe") != nullptr && sp::Match(L"wallpaper64.exe") != nullptr &&
              sp::Match(L"MSIAfterburner.exe") != nullptr && sp::Match(L"NVIDIA Overlay.exe") != nullptr,
          "match: the other programs of the list");
    Check(!sp::Match(L"notepad.exe") && !sp::Match(L"Discord") && !sp::Match(nullptr), "no match: others");
    char out[256];
    int n = 0;
    const bool ok = sp::Find(out, sizeof(out), &n);
    std::printf("  running: %s (%d processes)\n", out, n);
    Check(ok && n > 1 && out[0], "find: the process list is read");
}

bool LogHas(const char *path, const char *text)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        all.append(buf, n);
    }
    std::fclose(f);
    return all.find(text) != std::string::npos;
}

void TestWindowBackground(HWND w)
{
    namespace wb = core::diag::window_background;
    const ULONG_PTR black = reinterpret_cast<ULONG_PTR>(GetStockObject(BLACK_BRUSH));
    wb::Update(w, false);
    Check(GetClassLongPtrW(w, GCLP_HBRBACKGROUND) == black, "off: the class brush untouched");
    wb::Update(w, true);
    const ULONG_PTR ours = GetClassLongPtrW(w, GCLP_HBRBACKGROUND);
    LOGBRUSH lb{};
    Check(ours != black && GetObjectW(reinterpret_cast<HBRUSH>(ours), sizeof(lb), &lb) == sizeof(lb) &&
              lb.lbColor == RGB(255, 0, 255),
          "on: the class brush is magenta");
    wb::Update(w, true);
    Check(GetClassLongPtrW(w, GCLP_HBRBACKGROUND) == ours, "on again: the same brush (nothing done)");
    wb::Update(w, false);
    Check(GetClassLongPtrW(w, GCLP_HBRBACKGROUND) == black, "off: the stock black brush back");
    wb::Update(nullptr, true);
    Check(GetClassLongPtrW(w, GCLP_HBRBACKGROUND) == black, "no window: nothing done");
}

} // namespace

int main()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    slash[1] = 0;
    char logPath[MAX_PATH];
    std::snprintf(logPath, sizeof(logPath), "%ls%s", exe, "test_present_mode.log");
    DeleteFileA(logPath);
    core_log_open_folder(exe, "test_present_mode.log");

    TestTracker();
    TestPeriod();
    TestLinesAndEvents();
    TestScreenPrograms();

    // Attach with no Direct3D 11 swap chain (Direct3D 9): lines, nothing switched off; AfterPresent does nothing.
    pm::SetScreenProvider([] { return 1; });
    pm::Attach(nullptr, "direct3d", 1, nullptr);
    pm::AfterPresent(nullptr, nullptr);
    // A window of our own: the game window line (class, styles, redirection surface), the monitors.
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = "TestPresentModeClass";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(WS_EX_APPWINDOW, "TestPresentModeClass", "t", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                             wc.hInstance, nullptr);
    pm::Attach(nullptr, "direct3d", 1, w);
    Sleep(1000); // the screen programs are looked up on a thread of the log's own
    pm::Report(1);
    TestWindowBackground(w);
    DestroyWindow(w);
    core_log_close();
    Check(LogHas(logPath, "present mode: not queried (renderer direct3d: only Direct3D 11 is queried)"),
          "log: not queried on Direct3D 9");
    Check(LogHas(logPath, "class \"TestPresentModeClass\" (class style 0x0, background the stock black brush)"),
          "log: the window's class and background");
    Check(LogHas(logPath, "ex style 0x00040000 (WS_EX_NOREDIRECTIONBITMAP not set"), "log: ex style, redirection");
    Check(LogHas(logPath, "present mode: monitors: ") && LogHas(logPath, " (the game's)"),
          "log: the monitors, the window's marked");
    Check(LogHas(logPath, "present mode: screen programs running: ") && LogHas(logPath, "on a thread of ours)"),
          "log: the screen programs at start, from our thread");
    Check(LogHas(logPath, "s before") && LogHas(logPath, "present mode: report #1: screen programs running: "),
          "log: the report's screen programs with their age");
    Check(LogHas(logPath, "present mode: report #1: mode now unknown; the last 0 of 0 changes and failures"),
          "log: the report");
    Check(LogHas(logPath, "present mode: report #1: since the last 60-s line: "), "log: the report's figures");
    Check(LogHas(logPath, "window background: test color magenta (RGB 255,0,255) set as the class brush (was the stock "
                          "black brush)"),
          "log: the test color set");
    Check(LogHas(logPath, "window background: the class brush is back to the stock black brush"),
          "log: the test color restored");
    Check(!LogHas(logPath, "switched off"), "log: nothing switched off");

    std::printf("RESULT: %s (%d failed)\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
