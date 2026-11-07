// The window background test color (core/diag/window_background.h).
#include "core/diag/window_background.h"

#include "core/diag/guard.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace core::diag::window_background {

namespace {

Feature g_feature{"window background test color (core/diag)"};

// Drawing thread only.
HWND g_hwnd = nullptr;          // the window whose class has our brush
ULONG_PTR g_original = 0;       // the class brush before ours
HBRUSH g_brush = nullptr;       // ours, while applied

const char *BrushName(ULONG_PTR b)
{
    return !b                                                          ? "none"
           : b == reinterpret_cast<ULONG_PTR>(GetStockObject(BLACK_BRUSH)) ? "the stock black brush"
                                                                           : "another brush";
}

void Restore()
{
    if (!g_brush) {
        return;
    }
    SetLastError(0);
    const ULONG_PTR was = SetClassLongPtrW(g_hwnd, GCLP_HBRBACKGROUND, static_cast<LONG_PTR>(g_original));
    const DWORD err = GetLastError();
    InvalidateRect(g_hwnd, nullptr, TRUE);
    if (was == 0 && err != 0) {
        core_log("window background: restoring the class brush failed (error %lu); the test brush is kept", err);
        return; // ours stays in use by the class: not deleted
    }
    DeleteObject(g_brush);
    g_brush = nullptr;
    core_log("window background: the class brush is back to %s", BrushName(g_original));
    g_hwnd = nullptr;
}

struct Args {
    HWND hwnd;
    bool on;
};

void UpdateGuarded(void *p)
{
    const auto &a = *static_cast<Args *>(p);
    if (g_brush && (!a.on || a.hwnd != g_hwnd)) {
        Restore();
    }
    if (!a.on || g_brush) {
        return;
    }
    HBRUSH brush = CreateSolidBrush(RGB(255, 0, 255));
    if (!brush) {
        Disable(g_feature, "CreateSolidBrush failed");
        return;
    }
    SetLastError(0);
    const ULONG_PTR was = SetClassLongPtrW(a.hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(brush));
    const DWORD err = GetLastError();
    if (was == 0 && err != 0) {
        DeleteObject(brush);
        char why[96];
        wsprintfA(why, "SetClassLongPtr(GCLP_HBRBACKGROUND) failed, error %lu", err);
        Disable(g_feature, why);
        return;
    }
    g_hwnd = a.hwnd;
    g_original = was;
    g_brush = brush;
    // The next WM_PAINT erases the window (DefWindowProc's WM_ERASEBKGND) with the new brush.
    InvalidateRect(a.hwnd, nullptr, TRUE);
    core_log("window background: test color magenta (RGB 255,0,255) set as the class brush (was %s); flashing "
             "blocks in magenta would be the window's redirection surface",
             BrushName(was));
}

} // namespace

void Update(HWND__ *hwnd, bool on)
{
    if (!hwnd || !Enabled(g_feature)) {
        return;
    }
    if (on == (g_brush != nullptr) && (!on || hwnd == g_hwnd)) {
        return; // nothing to do: the usual case, every pass
    }
    Args a{hwnd, on};
    Guarded(g_feature, &UpdateGuarded, &a);
}

} // namespace core::diag::window_background
