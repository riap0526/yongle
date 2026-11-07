// Present mode log (core/diag/present_mode.h): the Windows side -- DXGI queries, the monitors, the log lines.
#include "core/diag/present_mode.h"

#include "core/diag/guard.h"
#include "core/diag/screen_programs.h"
#include "core/log.h"
#include "core/platform/affinity.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_3.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace core::diag::present_mode {

namespace {

Feature g_feature{"present mode log (core/diag)"};
std::atomic<int (*)()> g_screen{nullptr};

// Drawing thread only from here on.
Tracker g_tracker;
long g_presentsSinceChange = 0;
double g_ticksPerUs = 0;
char g_lastSetup[512] = "";
char g_lastWindow[512] = "";
char g_lastMonitors[1024] = "";
long long g_dxgiPresents = -1, g_dxgiRefreshes = -1;
HWND g_hwnd = nullptr; // the window of the last present (for the report's line)

// The screen programs: see Programs below.
constexpr DWORD kProgramsMs = 2000;

constexpr HRESULT kDisjoint = static_cast<HRESULT>(0x887A000BL); // DXGI_ERROR_FRAME_STATISTICS_DISJOINT

const char *HrName(HRESULT hr)
{
    switch (static_cast<unsigned long>(hr)) {
    case 0x887A000BUL:
        return "DXGI_ERROR_FRAME_STATISTICS_DISJOINT";
    case 0x887A0001UL:
        return "DXGI_ERROR_INVALID_CALL";
    case 0x887A0004UL:
        return "DXGI_ERROR_UNSUPPORTED";
    case 0x887A0005UL:
        return "DXGI_ERROR_DEVICE_REMOVED";
    case 0x887A0002UL:
        return "DXGI_ERROR_NOT_FOUND";
    case 0x80004002UL:
        return "E_NOINTERFACE";
    default:
        return "error";
    }
}

int Screen()
{
    int (*fn)() = g_screen.load();
    return fn ? fn() : -1;
}

// Whether the window covers its whole monitor (one of the conditions of a direct flip) or not.
const char *Where(HWND hwnd)
{
    RECT w{};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!hwnd || !GetWindowRect(hwnd, &w) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        return "window size not known";
    }
    if (EqualRect(&w, &mi.rcMonitor)) {
        return "covering the monitor";
    }
    return IsIconic(hwnd) ? "minimized" : "windowed";
}

void WhereAndScreen(HWND hwnd, char *out, std::size_t size)
{
    const int s = Screen();
    if (s >= 0) {
        std::snprintf(out, size, "%s, game screen 0x%02x", Where(hwnd), s);
    }
    else {
        std::snprintf(out, size, "%s", Where(hwnd));
    }
}

// The screen programs (core/diag/screen_programs.h) are looked up on a thread of ours: one lookup took 12 ms with 540
// processes on the development machine (2026-11-07), too long for the drawing thread. The drawing thread asks (at start,
// on a mode change, on a report; at most every kProgramsMs) and uses the last answer with its age; the thread logs the
// list whenever it differs from the one before.
SRWLOCK g_programsLock = SRWLOCK_INIT;
char g_programs[512] = "";     // under the lock
DWORD g_programsAt = 0;        // under the lock: when it was looked up (GetTickCount)
bool g_programsKnown = false;  // under the lock
HANDLE g_programsWake = nullptr; // drawing thread: made with the thread
bool g_programsThread = false;   // drawing thread: the thread was tried
DWORD g_programsAsked = 0;       // drawing thread
bool g_programsAskedEver = false;

void LookUpGuarded(void *p)
{
    char *last = static_cast<char *>(p);
    char found[512];
    int processes = 0;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    screen_programs::Find(found, sizeof(found), &processes);
    QueryPerformanceCounter(&t1);
    AcquireSRWLockExclusive(&g_programsLock);
    std::memcpy(g_programs, found, sizeof(found));
    g_programsAt = GetTickCount();
    g_programsKnown = true;
    ReleaseSRWLockExclusive(&g_programsLock);
    if (std::strcmp(found, last) != 0) {
        core_log("present mode: screen programs running: %s (%d processes looked at in %.1f ms, on a thread of ours)",
                 found, processes, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        std::memcpy(last, found, sizeof(found));
    }
}

DWORD WINAPI ProgramsThread(void *)
{
    core::platform::affinity::OurThread("present mode programs");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    static char last[512] = "";
    while (Enabled(g_feature)) {
        WaitForSingleObject(g_programsWake, INFINITE);
        Guarded(g_feature, &LookUpGuarded, last);
    }
    return 0;
}

// Drawing thread: asks for a new lookup when the last one was asked for kProgramsMs ago or more, and writes the last
// answer with its age to out.
void Programs(char *out, std::size_t size)
{
    const DWORD now = GetTickCount();
    if (!g_programsThread) {
        g_programsThread = true;
        g_programsWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_programsWake ? CreateThread(nullptr, 0, &ProgramsThread, nullptr, 0, nullptr) : nullptr;
        if (t) {
            CloseHandle(t);
        }
        else {
            core_log("present mode: the screen programs are not looked up (no thread, error %lu)", GetLastError());
            if (g_programsWake) {
                CloseHandle(g_programsWake);
                g_programsWake = nullptr;
            }
        }
    }
    if (!g_programsWake) {
        std::snprintf(out, size, "not looked up");
        return;
    }
    if (!g_programsAskedEver || now - g_programsAsked >= kProgramsMs) {
        g_programsAskedEver = true;
        g_programsAsked = now;
        SetEvent(g_programsWake);
    }
    AcquireSRWLockShared(&g_programsLock);
    if (g_programsKnown) {
        std::snprintf(out, size, "%s, as looked up %.1f s before", g_programs, (now - g_programsAt) / 1000.0);
    }
    else {
        std::snprintf(out, size, "not looked up yet");
    }
    ReleaseSRWLockShared(&g_programsLock);
}
// The monitors: how many, each one's device, monitor and adapter names, size and place, which one is primary and
// which one has the game window (logged when it differs from the last time). Virtual displays (e.g. for streaming)
// show up here with their own adapter names.
struct MonitorWalk {
    HMONITOR game;
    int count;
    std::size_t used;
    char *out;
    std::size_t size;
};

BOOL CALLBACK OneMonitor(HMONITOR m, HDC, LPRECT, LPARAM p)
{
    auto &w = *reinterpret_cast<MonitorWalk *>(p);
    ++w.count;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(m, &mi)) {
        return TRUE;
    }
    // The adapter whose output this is, and the monitor on it.
    wchar_t adapter[128] = L"?";
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (wcscmp(dd.DeviceName, mi.szDevice) == 0) {
            wcsncpy(adapter, dd.DeviceString, 127);
            adapter[127] = 0;
            break;
        }
        dd.cb = sizeof(dd);
    }
    wchar_t monitor[128] = L"?";
    DISPLAY_DEVICEW md{};
    md.cb = sizeof(md);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &md, 0)) {
        wcsncpy(monitor, md.DeviceString, 127);
        monitor[127] = 0;
    }
    const RECT &r = mi.rcMonitor;
    if (w.used + 1 < w.size) {
        const int n = std::snprintf(w.out + w.used, w.size - w.used, "%s%ls \"%ls\" on \"%ls\" %ldx%ld at (%ld,%ld)%s%s",
                                    w.count > 1 ? "; " : "", mi.szDevice, monitor, adapter, r.right - r.left,
                                    r.bottom - r.top, r.left, r.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " primary" : "",
                                    m == w.game ? " (the game's)" : "");
        w.used = n > 0 && w.used + n < w.size ? w.used + n : w.size - 1;
    }
    return TRUE;
}

void LogMonitors(HWND hwnd)
{
    char list[960] = "";
    MonitorWalk w{hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr, 0, 0, list, sizeof(list)};
    EnumDisplayMonitors(nullptr, nullptr, &OneMonitor, reinterpret_cast<LPARAM>(&w));
    char line[1024];
    std::snprintf(line, sizeof(line), "present mode: monitors: %d (%d by GetSystemMetrics): %s", w.count,
                  GetSystemMetrics(SM_CMONITORS), list);
    if (std::strcmp(line, g_lastMonitors) != 0) {
        core_log("%s", line);
        std::memcpy(g_lastMonitors, line, sizeof(line));
    }
}

// The game window: class, styles, background brush -- whether DWM keeps a redirection surface for it and what it
// would show (logged when it differs from the last time).
void LogWindow(HWND hwnd)
{
    if (!hwnd) {
        return;
    }
    char cls[128] = "";
    GetClassNameA(hwnd, cls, sizeof(cls));
    const LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    const LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
    const ULONG_PTR clsStyle = GetClassLongPtrA(hwnd, GCL_STYLE);
    const HBRUSH brush = reinterpret_cast<HBRUSH>(GetClassLongPtrA(hwnd, GCLP_HBRBACKGROUND));
    const char *brushName = !brush                                         ? "none"
                            : brush == GetStockObject(BLACK_BRUSH)         ? "the stock black brush"
                            : brush == GetStockObject(WHITE_BRUSH)         ? "the stock white brush"
                            : reinterpret_cast<ULONG_PTR>(brush) <= COLOR_MENUHILIGHT + 1 ? "a system color + 1"
                                                                                           : "a brush of its own";
    constexpr LONG kNoRedirection = 0x00200000L; // WS_EX_NOREDIRECTIONBITMAP
    char line[512];
    std::snprintf(line, sizeof(line),
                  "present mode: game window: class \"%s\" (class style 0x%lx, background %s), style 0x%08lx, ex style "
                  "0x%08lx (WS_EX_NOREDIRECTIONBITMAP %s: %s)",
                  cls, static_cast<unsigned long>(clsStyle), brushName, static_cast<unsigned long>(style),
                  static_cast<unsigned long>(ex), (ex & kNoRedirection) ? "set" : "not set",
                  (ex & kNoRedirection) ? "no redirection surface" : "the DWM keeps a redirection surface for it");
    if (std::strcmp(line, g_lastWindow) != 0) {
        core_log("%s", line);
        std::memcpy(g_lastWindow, line, sizeof(line));
    }
}

const char *SwapEffectName(DXGI_SWAP_EFFECT e)
{
    switch (e) {
    case DXGI_SWAP_EFFECT_DISCARD:
        return "DISCARD (blt)";
    case DXGI_SWAP_EFFECT_SEQUENTIAL:
        return "SEQUENTIAL (blt)";
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
        return "FLIP_SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_DISCARD:
        return "FLIP_DISCARD";
    default:
        return "other";
    }
}

void EnsureTicks()
{
    if (g_ticksPerUs == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_ticksPerUs = f.QuadPart / 1e6;
    }
}

struct AttachArgs {
    IDXGISwapChain *swap;
    const char *renderer;
    int sync;
    HWND hwnd;
};

void AttachGuarded(void *p)
{
    const auto &a = *static_cast<AttachArgs *>(p);
    EnsureTicks();
    LogWindow(a.hwnd);
    LogMonitors(a.hwnd);
    {
        char programs[600];
        Programs(programs, sizeof(programs)); // asks for a lookup: the thread logs the list
    }
    if (!a.swap) {
        core_log("present mode: not queried (renderer %s: only Direct3D 11 is queried)", a.renderer ? a.renderer : "?");
        return;
    }
    // The swap chain's setup.
    IDXGISwapChain1 *sc1 = nullptr;
    DXGI_SWAP_CHAIN_DESC1 d{};
    bool haveDesc = SUCCEEDED(a.swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void **>(&sc1))) &&
                    sc1 && SUCCEEDED(sc1->GetDesc1(&d));
    if (sc1) {
        sc1->Release();
    }
    BOOL full = FALSE;
    if (FAILED(a.swap->GetFullscreenState(&full, nullptr))) {
        full = FALSE;
    }
    const bool tearing = haveDesc && (d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    // The present flags as SDL 3.4.18 sets them (D3D11_UpdatePresentFlags): 0 with vsync; otherwise ALLOW_TEARING
    // when the swap chain has the flag and is not in exclusive full screen, else DO_NOT_WAIT.
    const char *flags = a.sync > 0 ? "0" : tearing && !full ? "DXGI_PRESENT_ALLOW_TEARING" : "DXGI_PRESENT_DO_NOT_WAIT";
    // The output's hardware composition support (whether this monitor and driver can put a window on a plane).
    char hw[128] = "not known (no IDXGIOutput6)";
    IDXGIOutput *out = nullptr;
    if (SUCCEEDED(a.swap->GetContainingOutput(&out)) && out) {
        IDXGIOutput6 *o6 = nullptr;
        if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void **>(&o6))) && o6) {
            UINT f = 0;
            if (SUCCEEDED(o6->CheckHardwareCompositionSupport(&f))) {
                std::snprintf(hw, sizeof(hw), "0x%x (full screen %s, windowed %s, cursor stretched %s)", f,
                              (f & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_FULLSCREEN) ? "yes" : "no",
                              (f & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED) ? "yes" : "no",
                              (f & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_CURSOR_STRETCHED) ? "yes" : "no");
            }
            else {
                std::snprintf(hw, sizeof(hw), "query failed");
            }
            o6->Release();
        }
        out->Release();
    }
    else {
        std::snprintf(hw, sizeof(hw), "not known (no containing output)");
    }
    char line[512];
    if (haveDesc) {
        std::snprintf(line, sizeof(line),
                      "present mode: swap chain %ux%u, swap effect %s, %u buffers, flags 0x%x (ALLOW_TEARING %s), "
                      "scaling %d, alpha mode %d, format %d, exclusive full screen %s; present sync interval %d, "
                      "present flags %s (as SDL sets them); hardware composition support %s",
                      d.Width, d.Height, SwapEffectName(d.SwapEffect), d.BufferCount, d.Flags, tearing ? "yes" : "no",
                      static_cast<int>(d.Scaling), static_cast<int>(d.AlphaMode), static_cast<int>(d.Format),
                      full ? "yes" : "no", a.sync, flags, hw);
    }
    else {
        std::snprintf(line, sizeof(line),
                      "present mode: swap chain description not known; present sync interval %d; hardware composition "
                      "support %s",
                      a.sync, hw);
    }
    if (std::strcmp(line, g_lastSetup) != 0) {
        core_log("%s", line);
        std::memcpy(g_lastSetup, line, sizeof(line));
    }
    // The interface the log needs: missing -> switched off (one line), the game goes on.
    IDXGISwapChainMedia *media = nullptr;
    const HRESULT hr =
        a.swap->QueryInterface(__uuidof(IDXGISwapChainMedia), reinterpret_cast<void **>(&media));
    if (FAILED(hr) || !media) {
        char why[128];
        std::snprintf(why, sizeof(why), "the swap chain has no IDXGISwapChainMedia (%s, 0x%08lx)", HrName(hr),
                      static_cast<unsigned long>(hr));
        Disable(g_feature, why);
        return;
    }
    media->Release();
}

struct PresentArgs {
    IDXGISwapChain *swap;
    HWND hwnd;
};

void AfterPresentGuarded(void *p)
{
    const auto &a = *static_cast<PresentArgs *>(p);
    EnsureTicks();
    g_hwnd = a.hwnd;
    g_tracker.Present();
    ++g_presentsSinceChange;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    IDXGISwapChainMedia *media = nullptr;
    HRESULT hr = a.swap->QueryInterface(__uuidof(IDXGISwapChainMedia), reinterpret_cast<void **>(&media));
    DXGI_FRAME_STATISTICS_MEDIA st{};
    if (SUCCEEDED(hr) && media) {
        hr = media->GetFrameStatisticsMedia(&st);
        media->Release();
    }
    QueryPerformanceCounter(&t1);
    const DWORD now = GetTickCount();
    int from = kUnknown;
    bool firstError = false;
    const bool ok = SUCCEEDED(hr);
    if (ok) {
        g_dxgiPresents = st.PresentCount;
        g_dxgiRefreshes = st.PresentRefreshCount;
    }
    if (g_tracker.Query(ok, ok ? static_cast<int>(st.CompositionMode) : kUnknown, static_cast<long>(hr),
                        t1.QuadPart - t0.QuadPart, now, &from, &firstError)) {
        char where[96];
        WhereAndScreen(a.hwnd, where, sizeof(where));
        char programs[600];
        Programs(programs, sizeof(programs));
        char text[Tracker::kTextSize];
        std::snprintf(text, sizeof(text), "%s; screen programs: %s", where, programs);
        g_tracker.Remember(now, from, g_tracker.Current(), 0, g_presentsSinceChange, text);
        if (g_tracker.ChangeLineAllowed(now)) {
            char from_[24];
            std::snprintf(from_, sizeof(from_), "%s", ModeName(from));
            core_log("present mode: %s -> %s (%s; %ld presents since the last change; screen programs: %s)", from_,
                     ModeName(g_tracker.Current()), where, g_presentsSinceChange, programs);
        }
        g_presentsSinceChange = 0;
    }
    if (!ok && firstError) {
        char where[96];
        WhereAndScreen(a.hwnd, where, sizeof(where));
        g_tracker.Remember(now, kUnknown, kUnknown, static_cast<long>(hr), g_presentsSinceChange, where);
        core_log("present mode: the query failed: %s (0x%08lx)%s; counted in the 60-s line, the log goes on",
                 HrName(hr), static_cast<unsigned long>(hr),
                 hr == kDisjoint ? " (the statistics' sequence was broken)" : "");
    }
    if (g_tracker.GivingUp()) {
        Disable(g_feature, "the query kept failing for 10 s");
        return;
    }
    if (!g_tracker.PeriodDue(now)) {
        return;
    }
    char where[96] = "";
    WhereAndScreen(a.hwnd, where, sizeof(where));
    char line[512];
    if (g_tracker.Period(now, g_dxgiPresents, g_dxgiRefreshes, g_ticksPerUs, where, false, false, line, sizeof(line))) {
        core_log("%s", line);
    }
}

void ReportGuarded(void *p)
{
    const int n = *static_cast<int *>(p);
    EnsureTicks();
    const DWORD now = GetTickCount();
    const int kept = g_tracker.Remembered();
    core_log("present mode: report #%d: mode now %s; the last %d of %ld changes and failures (oldest first):", n,
             ModeName(g_tracker.Current()), kept, g_tracker.RememberedTotal());
    for (int i = 0; i < kept; ++i) {
        char line[768];
        g_tracker.EventLine(i, now, line, sizeof(line));
        core_log("present mode: report #%d:   %s", n, line);
    }
    char where[96];
    WhereAndScreen(g_hwnd, where, sizeof(where));
    char line[512];
    // The figures since the last 60-s line, without starting them again (the 60-s lines keep their spacing).
    if (g_tracker.Period(now, g_dxgiPresents, g_dxgiRefreshes, g_ticksPerUs, where, true, true, line, sizeof(line))) {
        core_log("present mode: report #%d: since the last 60-s line: %s", n, line + std::strlen("present mode: "));
    }
    char programs[600];
    Programs(programs, sizeof(programs));
    core_log("present mode: report #%d: screen programs running: %s", n, programs);
}

} // namespace

void SetScreenProvider(int (*fn)())
{
    g_screen = fn;
}

void Attach(IDXGISwapChain *swap, const char *renderer, int syncInterval, HWND__ *hwnd)
{
    if (!Enabled(g_feature)) {
        return;
    }
    AttachArgs a{swap, renderer, syncInterval, hwnd};
    Guarded(g_feature, &AttachGuarded, &a);
}

void AfterPresent(IDXGISwapChain *swap, HWND__ *hwnd)
{
    if (!swap || !Enabled(g_feature)) {
        return;
    }
    PresentArgs a{swap, hwnd};
    Guarded(g_feature, &AfterPresentGuarded, &a);
}

void Report(int n)
{
    if (!Enabled(g_feature)) {
        return;
    }
    Guarded(g_feature, &ReportGuarded, &n);
}

} // namespace core::diag::present_mode
