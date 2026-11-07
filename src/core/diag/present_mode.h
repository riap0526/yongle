// Present mode log (docs/resolution_ui/sdl_output_comparison.md §8; trial step 2026-11-07, second step 2026-11-07):
// how Windows puts our Direct3D 11 frames on the screen, to follow the black blocks that flash in full screen. Only
// logs; changes nothing in the output.
//
// After each present on Direct3D 11, IDXGISwapChainMedia::GetFrameStatisticsMedia gives CompositionMode (composed,
// overlay, none, composition failure). The trial showed it tells full screen (overlay) from the moments the desktop
// composes (focus changes, Win+PrtSc, the volume flyout); DwmGetCompositionTimingInfo's counts did not change with
// anything (one frame a second), so they are no longer read. Logged:
//   - once per swap chain (when it differs from the last): its setup and the output's hardware composition support;
//     the game window's class and styles; the monitors (count, names, adapters: virtual displays show up there);
//   - the programs known to draw on the screen (core/diag/screen_programs.h) at start-up, and with every mode change;
//   - a line whenever the mode changes (at most 20 a second, the rest counted): old -> new, windowed or covering the
//     monitor, the game's screen number, the screen programs;
//   - every 60 s: our presents, the frames per mode, errors, changes, DXGI's present and refresh counts, the query's
//     cost;
//   - on the report key (Report): the last 64 changes and failures with their age, and the figures since the last
//     60-s line.
// A failed query (DXGI_ERROR_FRAME_STATISTICS_DISJOINT or another error) is counted and its first occurrence logged;
// the log is switched off only when the interface is missing or the query keeps failing. Every entry point runs under
// a feature guard (core/diag/guard.h). Direct3D 9 (blt model, always composed) is not queried. The interfaces got by
// QueryInterface are released at once; SDL's own reference to its swap chain is never touched or kept. Nothing here
// knows a game: the game layer gives its screen number through SetScreenProvider. [debug] present_mode_log = off
// leaves all of it out (the caller does not call in).
#pragma once

#include <cstddef>
#include <cstdint>

struct IDXGISwapChain;
struct HWND__;

namespace core::diag::present_mode {

// Any thread, once: returns the game's current screen number (read only), or -1.
void SetScreenProvider(int (*fn)());

// The drawing thread, after the renderer was made (or made again): swap is the renderer's Direct3D 11 swap chain (SDL
// keeps owning it) or null for another renderer (renderer: its SDL name); syncInterval: the vsync interval in use.
void Attach(IDXGISwapChain *swap, const char *renderer, int syncInterval, HWND__ *hwnd);

// The drawing thread, after each successful present (swap as in Attach; null: nothing is done).
void AfterPresent(IDXGISwapChain *swap, HWND__ *hwnd);

// The drawing thread, on the report key (n: the report's number): the recent changes and the figures so far.
void Report(int n);

// ---------------------------------------------------------------------------------------------
// The bookkeeping without Windows (offline test: proxy/ddraw/test_present_mode.cpp).

enum Mode : int { kUnknown = -1, kComposed = 0, kOverlay = 1, kNone = 2, kCompositionFailure = 3 };
constexpr int kModes = 4;

const char *ModeName(int mode); // "composed", "overlay", "none", "composition failure", "unknown", "value N"

class Tracker {
public:
    static constexpr int kMaxErrorKinds = 8;        // distinct HRESULTs logged once each
    static constexpr unsigned long kFailMs = 10000; // no successful query for this long while presenting: give up
    static constexpr int kFailCalls = 100;          // ... and at least this many failed queries in a row
    static constexpr int kChangeLines = 20;         // change lines per second; the others are counted
    static constexpr unsigned long kPeriodMs = 60000; // the periodic line
    static constexpr int kEvents = 64;              // changes and failures kept for the report
    static constexpr std::size_t kTextSize = 640;   // an event's text (where, screen programs)

    // One query: ok and its mode, or the failure's hr. Returns true when the mode differs from the one before (then
    // *from is the old mode). *firstError: this hr was not seen before (logged by the caller).
    bool Query(bool ok, int mode, long hr, std::int64_t ticks, unsigned long nowMs, int *from, bool *firstError);
    void Present() { ++presents_; }
    // After a change: whether its line may be written (at most kChangeLines in a second).
    bool ChangeLineAllowed(unsigned long nowMs);
    // True when the query should be switched off (failing without a success for kFailMs and kFailCalls).
    bool GivingUp() const { return failRun_ >= kFailCalls && failRunMs_ >= kFailMs; }
    int Current() const { return mode_; }

    // Kept for the report (oldest dropped after kEvents): a change (from -> to) or a failure (hr, from == to ==
    // kUnknown), with its time, the presents since the event before, and a text (where, screen programs).
    void Remember(unsigned long nowMs, int from, int to, long hr, long presentsBefore, const char *text);
    int Remembered() const { return events_ < kEvents ? events_ : kEvents; }
    long RememberedTotal() const { return events_; }
    // Event i (0: the oldest kept) as a line, its age counted back from nowMs.
    void EventLine(int i, unsigned long nowMs, char *out, std::size_t size) const;

    // The periodic line: when force or at least kPeriodMs passed since the last one (the first call only starts).
    // dxgiPresents/dxgiRefreshes: the last good PresentCount / PresentRefreshCount (-1: not known). ticksPerUs: the
    // performance counter's ticks per microsecond. Unless keep, the counters start again after the line.
    bool PeriodDue(unsigned long nowMs) const { return !started_ || nowMs - lastLineMs_ >= kPeriodMs; }
    bool Period(unsigned long nowMs, long long dxgiPresents, long long dxgiRefreshes, double ticksPerUs,
                const char *where, bool force, bool keep, char *out, std::size_t size);

private:
    struct Event {
        unsigned long ms;
        int from, to;
        long hr;
        long presents;
        char text[kTextSize];
    };
    int mode_ = kUnknown;
    long presents_ = 0;
    long perMode_[kModes + 1] = {}; // the last: values outside the enum
    long errors_ = 0;
    long changes_ = 0, notLogged_ = 0;
    long queries_ = 0;
    std::int64_t ticksSum_ = 0, ticksMax_ = 0;
    long seenHr_[kMaxErrorKinds] = {};
    int seenHrCount_ = 0;
    int failRun_ = 0;
    unsigned long failRunStart_ = 0, failRunMs_ = 0;
    unsigned long lastLineMs_ = 0;
    bool started_ = false;
    long long lastDxgiPresents_ = -1, lastDxgiRefreshes_ = -1;
    unsigned long lineWindowMs_ = 0;
    int linesInWindow_ = 0;
    bool lineWindowStarted_ = false;
    Event ring_[kEvents] = {};
    long events_ = 0;
};

} // namespace core::diag::present_mode
