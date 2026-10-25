// Offline test of the engine DLL's start-up path (not the game). Like Emperor.exe, this exe imports
// DirectDrawCreate from ddraw.dll statically, so the engine ddraw.dll next to it is loaded and initialized before
// the exe's entry point. Checks that, already at main() and before any window exists, the process is DPI aware
// (set by the DLL's exe-entry hook, not by the CreateWindowExA fallback), that a window created afterwards is
// per-monitor aware with the system DPI, and that DirectDrawCreate still works.
// Usage: run test_entry.exe from the directory that contains the engine ddraw.dll.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>

#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what, long value = 0)
{
    std::printf("%-56s %s (%ld)\n", what, ok ? "ok" : "FAILED", value);
    if (!ok) {
        ++g_failures;
    }
}

using Ctx = HANDLE;
const Ctx kPerMonitorV2 = reinterpret_cast<Ctx>(-4);

template <typename F> F Proc(const char *name)
{
    return reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name)));
}

} // namespace

int main()
{
    auto threadCtx = Proc<Ctx(WINAPI *)()>("GetThreadDpiAwarenessContext");
    auto windowCtx = Proc<Ctx(WINAPI *)(HWND)>("GetWindowDpiAwarenessContext");
    auto equal = Proc<BOOL(WINAPI *)(Ctx, Ctx)>("AreDpiAwarenessContextsEqual");
    auto dpiForWindow = Proc<UINT(WINAPI *)(HWND)>("GetDpiForWindow");
    auto dpiForSystem = Proc<UINT(WINAPI *)()>("GetDpiForSystem");
    if (!threadCtx || !windowCtx || !equal || !dpiForWindow || !dpiForSystem) {
        std::printf("Windows 10 1703+ DPI APIs not available; test not applicable\n");
        return 2;
    }

    Check(GetModuleHandleW(L"ddraw.dll") != nullptr, "ddraw.dll loaded before main");
    Check(equal(threadCtx(), kPerMonitorV2) != 0, "process is per-monitor-v2 aware at main (no window yet)");
    Check(IsProcessDPIAware() != 0, "IsProcessDPIAware");

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "test_entry";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "test_entry", WS_OVERLAPPEDWINDOW, 60, 60, 640, 480, nullptr,
                                nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, "CreateWindowExA (through the hook)");
    Check(equal(windowCtx(hwnd), kPerMonitorV2) != 0, "window is per-monitor-v2 aware");
    UINT wdpi = dpiForWindow(hwnd), sdpi = dpiForSystem();
    Check(wdpi == sdpi, "window DPI == system DPI (real, not virtualized 96)", static_cast<long>(wdpi));
    std::printf("system DPI %u = %u%% scaling\n", sdpi, sdpi * 100 / 96);

    IDirectDraw *dd = nullptr;
    HRESULT hr = DirectDrawCreate(nullptr, &dd, nullptr);
    Check(SUCCEEDED(hr) && dd, "DirectDrawCreate through the engine DLL", hr);
    if (dd) {
        Check(dd->Release() == 0, "DirectDraw final Release == 0");
    }
    DestroyWindow(hwnd);
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
