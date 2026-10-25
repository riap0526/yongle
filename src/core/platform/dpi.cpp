#include "core/platform/dpi.h"

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

namespace {

// DPI_AWARENESS_CONTEXT values (windef.h, Windows 10); declared here so older SDK headers are enough.
using DpiContext = HANDLE;
const DpiContext kUnaware = reinterpret_cast<DpiContext>(-1);
const DpiContext kSystemAware = reinterpret_cast<DpiContext>(-2);
const DpiContext kPerMonitor = reinterpret_cast<DpiContext>(-3);
const DpiContext kPerMonitorV2 = reinterpret_cast<DpiContext>(-4);
const DpiContext kUnawareGdiScaled = reinterpret_cast<DpiContext>(-5);

struct User32Dpi {
    BOOL(WINAPI *SetProcessDpiAwarenessContext)(DpiContext);
    DpiContext(WINAPI *GetThreadDpiAwarenessContext)();
    DpiContext(WINAPI *GetWindowDpiAwarenessContext)(HWND);
    DpiContext(WINAPI *GetDpiAwarenessContextForProcess)(HANDLE);
    BOOL(WINAPI *AreDpiAwarenessContextsEqual)(DpiContext, DpiContext);
    UINT(WINAPI *GetDpiForSystem)();
    UINT(WINAPI *GetDpiForWindow)(HWND);
    BOOL(WINAPI *SetProcessDPIAware)();
    BOOL(WINAPI *IsProcessDPIAware)();
};

template <typename F> void Resolve(HMODULE m, F &fn, const char *name)
{
    fn = m ? reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(m, name))) : nullptr;
}

const User32Dpi &Api()
{
    static const User32Dpi api = [] {
        User32Dpi a{};
        HMODULE u = GetModuleHandleW(L"user32.dll");
        Resolve(u, a.SetProcessDpiAwarenessContext, "SetProcessDpiAwarenessContext");
        Resolve(u, a.GetThreadDpiAwarenessContext, "GetThreadDpiAwarenessContext");
        Resolve(u, a.GetWindowDpiAwarenessContext, "GetWindowDpiAwarenessContext");
        Resolve(u, a.GetDpiAwarenessContextForProcess, "GetDpiAwarenessContextForProcess");
        Resolve(u, a.AreDpiAwarenessContextsEqual, "AreDpiAwarenessContextsEqual");
        Resolve(u, a.GetDpiForSystem, "GetDpiForSystem");
        Resolve(u, a.GetDpiForWindow, "GetDpiForWindow");
        Resolve(u, a.SetProcessDPIAware, "SetProcessDPIAware");
        Resolve(u, a.IsProcessDPIAware, "IsProcessDPIAware");
        return a;
    }();
    return api;
}

const char *ContextName(DpiContext c)
{
    const User32Dpi &a = Api();
    if (!c) {
        return "n/a";
    }
    if (!a.AreDpiAwarenessContextsEqual) {
        return "?";
    }
    static const struct {
        DpiContext ctx;
        const char *name;
    } kNames[] = {{kPerMonitorV2, "per-monitor-v2"},
                  {kPerMonitor, "per-monitor"},
                  {kSystemAware, "system"},
                  {kUnawareGdiScaled, "unaware-gdiscaled"},
                  {kUnaware, "unaware"}};
    for (const auto &n : kNames) {
        if (a.AreDpiAwarenessContextsEqual(c, n.ctx)) {
            return n.name;
        }
    }
    return "other";
}

UINT SystemDpi()
{
    const User32Dpi &a = Api();
    if (a.GetDpiForSystem) {
        return a.GetDpiForSystem();
    }
    HDC dc = GetDC(nullptr);
    UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 0;
    if (dc) {
        ReleaseDC(nullptr, dc);
    }
    return dpi;
}

// Tries the next API down only when the newer one is missing or rejects the requested mode; an "access
// denied" means the awareness is already fixed for this process, so nothing further can change it.
void Apply()
{
    const User32Dpi &a = Api();
    if (a.SetProcessDpiAwarenessContext) {
        const struct {
            DpiContext ctx;
            const char *name;
        } kTry[] = {{kPerMonitorV2, "per-monitor-v2"}, {kPerMonitor, "per-monitor"}};
        for (const auto &t : kTry) {
            if (a.SetProcessDpiAwarenessContext(t.ctx)) {
                core_log("DPI: SetProcessDpiAwarenessContext(%s) succeeded", t.name);
                return;
            }
            DWORD err = GetLastError();
            core_log("DPI: SetProcessDpiAwarenessContext(%s) failed (err %lu)", t.name, err);
            if (err == ERROR_ACCESS_DENIED) {
                core_log("DPI: awareness was already set for this process (compatibility setting or manifest); "
                         "leaving it as is");
                return;
            }
        }
    }
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        using SetFn = HRESULT(WINAPI *)(int);
        auto set = reinterpret_cast<SetFn>(reinterpret_cast<void *>(GetProcAddress(shcore, "SetProcessDpiAwareness")));
        if (set) {
            HRESULT hr = set(2); // PROCESS_PER_MONITOR_DPI_AWARE
            core_log("DPI: SetProcessDpiAwareness(per-monitor) -> 0x%08lX", static_cast<unsigned long>(hr));
            if (SUCCEEDED(hr) || hr == E_ACCESSDENIED) {
                return; // shcore stays loaded: it is a system DLL and may be used again by user32
            }
        }
    }
    if (a.SetProcessDPIAware) {
        BOOL ok = a.SetProcessDPIAware();
        core_log("DPI: SetProcessDPIAware() -> %d", ok);
        return;
    }
    core_log("DPI: no DPI awareness API available");
}

std::atomic<bool> g_applied{false};

} // namespace

extern "C" void core_dpi_log_state(const char *when)
{
    const User32Dpi &a = Api();
    DpiContext proc = a.GetDpiAwarenessContextForProcess ? a.GetDpiAwarenessContextForProcess(nullptr) : nullptr;
    DpiContext thread = a.GetThreadDpiAwarenessContext ? a.GetThreadDpiAwarenessContext() : nullptr;
    UINT dpi = SystemDpi();
    core_log("DPI state (%s): process=%s thread=%s IsProcessDPIAware=%d system_dpi=%u (%u%%) screen=%dx%d", when,
             ContextName(proc), ContextName(thread), a.IsProcessDPIAware ? a.IsProcessDPIAware() : -1, dpi,
             dpi * 100 / 96, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

extern "C" void core_dpi_apply(const char *when)
{
    if (g_applied.exchange(true)) {
        return;
    }
    core_log("DPI: applying at %s", when);
    core_dpi_log_state("before");
    Apply();
    core_dpi_log_state("after");
}

extern "C" void core_dpi_log_window(void *hwnd_, const char *when)
{
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (!hwnd || !IsWindow(hwnd)) {
        core_log("window %p (%s): not a window", hwnd_, when);
        return;
    }
    const User32Dpi &a = Api();
    RECT wr{}, cr{};
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    core_log("window %p (%s): rect [%ld,%ld,%ld,%ld] client %ldx%ld at (%ld,%ld) dpi=%u awareness=%s style=0x%lx "
             "monitor [%ld,%ld,%ld,%ld]%s screen=%dx%d",
             hwnd_, when, wr.left, wr.top, wr.right, wr.bottom, cr.right - cr.left, cr.bottom - cr.top, origin.x,
             origin.y, a.GetDpiForWindow ? a.GetDpiForWindow(hwnd) : 0,
             ContextName(a.GetWindowDpiAwarenessContext ? a.GetWindowDpiAwarenessContext(hwnd) : nullptr),
             static_cast<unsigned long>(GetWindowLongW(hwnd, GWL_STYLE)), mi.rcMonitor.left, mi.rcMonitor.top,
             mi.rcMonitor.right, mi.rcMonitor.bottom, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "",
             GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}
