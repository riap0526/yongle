#include "core/display/virtual_display.h"

#include "core/capture/screenshot.h"
#include "core/diag/guard.h"
#include "core/diag/report.h"
#include "core/display/framing.h"
#include "core/hook/hook.h"
#include "core/log.h"
#include "core/config.h"
#include "core/output/sdl_output.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"
#include "core/ui/panel.h"

#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace core::display {

namespace {

// State. Written on the game thread (DirectDraw calls); read by the hooks, which only act for the exe's own calls.
HWND g_hwnd = nullptr;
WNDPROC g_prevProc = nullptr;
bool g_outputOk = false;
volatile bool g_active = false;
SIZE g_mode{0, 0};

BYTE *g_exeBase = nullptr;
SIZE_T g_exeSize = 0;

bool FromExe(void *caller)
{
    auto *p = static_cast<BYTE *>(caller);
    return p >= g_exeBase && p < g_exeBase + g_exeSize;
}

// Originals (trampolines) of the hooked user32 functions; our own calls use them directly.
using GetSystemMetrics_fn = int(WINAPI *)(int);
using GetCursorPos_fn = BOOL(WINAPI *)(LPPOINT);
using PointConv_fn = BOOL(WINAPI *)(HWND, LPPOINT);
using GetRect_fn = BOOL(WINAPI *)(HWND, LPRECT);
using WindowFromPoint_fn = HWND(WINAPI *)(POINT);
using SetWindowPos_fn = BOOL(WINAPI *)(HWND, HWND, int, int, int, int, UINT);
using MoveWindow_fn = BOOL(WINAPI *)(HWND, int, int, int, int, BOOL);
using KeyState_fn = SHORT(WINAPI *)(int);
using SetWindowLong_fn = LONG(WINAPI *)(HWND, int, LONG);
using SetCursor_fn = HCURSOR(WINAPI *)(HCURSOR);
GetSystemMetrics_fn o_GetSystemMetrics = GetSystemMetrics;
GetCursorPos_fn o_GetCursorPos = GetCursorPos;
PointConv_fn o_ClientToScreen = ClientToScreen;
PointConv_fn o_ScreenToClient = ScreenToClient;
GetRect_fn o_GetClientRect = GetClientRect;
GetRect_fn o_GetWindowRect = GetWindowRect;
WindowFromPoint_fn o_WindowFromPoint = WindowFromPoint;
SetWindowPos_fn o_SetWindowPos = SetWindowPos;
MoveWindow_fn o_MoveWindow = MoveWindow;
KeyState_fn o_GetAsyncKeyState = GetAsyncKeyState;
KeyState_fn o_GetKeyState = GetKeyState;
SetWindowLong_fn o_SetWindowLongA = SetWindowLongA;
SetCursor_fn o_SetCursor = SetCursor;

// Our own calls on the game window (placing it, keeping it on the monitor) go to user32 itself, past any Windows
// compatibility shim in front of it: GOG installs Emperor with the DWM8And16BitMitigation layer, and through its
// SetWindowPos (AcGenral.dll) the window stayed 30 px above the monitor, with the taskbar over the game (0.1.1). The
// exe's own calls still pass the shim (they are hooked where they arrive). Without a shim these are the trampolines.
SetWindowPos_fn own_SetWindowPos = SetWindowPos;
MoveWindow_fn own_MoveWindow = MoveWindow;
SetWindowLong_fn own_SetWindowLongA = SetWindowLongA;

// R2e: windowed (a window that can be moved and sized) or covering the monitor. The game is not told: it stays in
// its fullscreen mode; only the window's style and place change, and the output follows the client area.
bool g_windowed = false;
SIZE g_settled{0, 0};    // the client size when the window last came to rest (what the layout uses)
bool g_inSizeMove = false;
constexpr int kMinClientW = 1024, kMinClientH = 768; // the smallest layout
constexpr int kEdgeBand = 8; // windowed: this many pixels (at 96 DPI) inside the client edge count as the edge

// The edge band in the window's pixels (per-monitor DPI aware: 8 px at 100 %, 16 at 200 %).
int EdgeBand()
{
    using GetDpiForWindow_fn = UINT(WINAPI *)(HWND);
    static GetDpiForWindow_fn dpiFor = reinterpret_cast<GetDpiForWindow_fn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    const UINT dpi = dpiFor && g_hwnd ? dpiFor(g_hwnd) : 96;
    return static_cast<int>(kEdgeBand * (dpi ? dpi : 96) / 96);
}

POINT g_panelCursor{0, 0}; // the mode position the game last saw before our panel opened

// The frame the game last presented (game thread): what the screenshot hotkey saves. The memory belongs to the
// presenting surface; FrameGone clears it before that is freed.
const void *g_lastPixels = nullptr;
int g_lastPitch = 0, g_lastW = 0, g_lastH = 0;

long g_cursorLogs = 0, g_messageLogs = 0, g_placementLogs = 0;
constexpr long kMapLogs = 8;

// Where the mode's picture lies inside the real client area: the largest rectangle of the mode's aspect, centred.
RECT Viewport()
{
    RECT c{0, 0, 0, 0};
    if (g_hwnd) {
        o_GetClientRect(g_hwnd, &c);
    }
    LONG cw = c.right - c.left, ch = c.bottom - c.top, mw = g_mode.cx, mh = g_mode.cy;
    if (cw <= 0 || ch <= 0 || mw <= 0 || mh <= 0) {
        return RECT{0, 0, mw, mh};
    }
    LONG w, h;
    if (static_cast<long long>(cw) * mh <= static_cast<long long>(ch) * mw) {
        w = cw;
        h = static_cast<LONG>(static_cast<long long>(cw) * mh / mw);
    }
    else {
        h = ch;
        w = static_cast<LONG>(static_cast<long long>(ch) * mw / mh);
    }
    LONG x = (cw - w) / 2, y = (ch - h) / 2;
    return RECT{x, y, x + w, y + h};
}

// Framed full-screen picture (set by the game layer, game thread): where the picture box of the mode lies on the
// real client area, in the same layout the output draws (core/display/framing.h). False when not framed.
core::output::Framing g_framing{};

struct PictureOnScreen {
    double x, y, w, h; // real client pixels
    RECT box;          // the picture in mode pixels
};

bool FramedPicture(PictureOnScreen *out)
{
    const RECT &b = g_framing.picture;
    if (!g_framing.on || !g_hwnd || b.right <= b.left || b.bottom <= b.top) {
        return false;
    }
    RECT c{0, 0, 0, 0};
    o_GetClientRect(g_hwnd, &c);
    FrameLayout l{};
    const int ph = b.bottom - b.top, bh = ph + (g_framing.below > 0 ? g_framing.below : 0);
    if (!ComputeFrameLayout(c.right - c.left, c.bottom - c.top, b.right - b.left, bh, &l, g_framing.native)) {
        return false;
    }
    *out = PictureOnScreen{l.picX, l.picY, l.picW, l.picH * ph / bh, b}; // the picture: the block's top part
    return true;
}

bool OnFramedPicture(POINT client)
{
    PictureOnScreen p{};
    return !FramedPicture(&p) || (client.x >= p.x && client.x < p.x + p.w && client.y >= p.y && client.y < p.y + p.h);
}

POINT ToVirtual(POINT client)
{
    PictureOnScreen p{};
    if (FramedPicture(&p)) { // through the picture's place on the screen, clamped to the picture
        const LONG bw = p.box.right - p.box.left, bh = p.box.bottom - p.box.top;
        LONG x = p.box.left + static_cast<LONG>(std::floor((client.x - p.x) * bw / p.w));
        LONG y = p.box.top + static_cast<LONG>(std::floor((client.y - p.y) * bh / p.h));
        x = x < p.box.left ? p.box.left : x >= p.box.right ? p.box.right - 1 : x;
        y = y < p.box.top ? p.box.top : y >= p.box.bottom ? p.box.bottom - 1 : y;
        return POINT{x, y};
    }
    RECT v = Viewport();
    LONG vw = v.right - v.left, vh = v.bottom - v.top;
    if (vw <= 0 || vh <= 0) {
        return client;
    }
    LONG x = static_cast<LONG>((static_cast<long long>(client.x - v.left) * g_mode.cx) / vw);
    LONG y = static_cast<LONG>((static_cast<long long>(client.y - v.top) * g_mode.cy) / vh);
    if (client.x < v.left) x = 0;
    if (client.y < v.top) y = 0;
    if (x >= g_mode.cx) x = g_mode.cx - 1;
    if (y >= g_mode.cy) y = g_mode.cy - 1;
    return POINT{x, y};
}

POINT ToRealClient(POINT virt)
{
    PictureOnScreen p{};
    if (FramedPicture(&p)) {
        const double bw = p.box.right - p.box.left, bh = p.box.bottom - p.box.top;
        return POINT{static_cast<LONG>(p.x + (virt.x - p.box.left + 0.5) * p.w / bw),
                     static_cast<LONG>(p.y + (virt.y - p.box.top + 0.5) * p.h / bh)};
    }
    RECT v = Viewport();
    LONG vw = v.right - v.left, vh = v.bottom - v.top;
    return POINT{v.left + static_cast<LONG>((static_cast<long long>(virt.x) * vw + vw / 2) / g_mode.cx),
                 v.top + static_cast<LONG>((static_cast<long long>(virt.y) * vh + vh / 2) / g_mode.cy)};
}

bool Acting(void *caller, HWND hwnd)
{
    return g_active && FromExe(caller) && (!hwnd || hwnd == g_hwnd);
}

// ---------------------------------------------------------------------------------------------
// Hooks

int WINAPI GetSystemMetrics_detour(int index)
{
    if (Acting(__builtin_return_address(0), nullptr)) {
        if (index == SM_CXSCREEN) return g_mode.cx;
        if (index == SM_CYSCREEN) return g_mode.cy;
    }
    return o_GetSystemMetrics(index);
}

BOOL WINAPI GetCursorPos_detour(LPPOINT pt)
{
    void *caller = __builtin_return_address(0);
    if (pt && Acting(caller, nullptr) && core::ui::IsOpen()) {
        // The cursor is on our panel: the game sees it where it was when the panel opened, kept 2 pixels away from
        // the edges (the game may draw frames while the panel asks to keep a new resolution: no edge scrolling).
        LONG x = g_panelCursor.x, y = g_panelCursor.y;
        *pt = POINT{x < 2 ? 2 : x > g_mode.cx - 3 ? g_mode.cx - 3 : x, y < 2 ? 2 : y > g_mode.cy - 3 ? g_mode.cy - 3 : y};
        return TRUE;
    }
    BOOL ok = o_GetCursorPos(pt);
    if (ok && pt && Acting(caller, nullptr)) {
        POINT real = *pt, c = *pt;
        o_ScreenToClient(g_hwnd, &c);
        *pt = ToVirtual(c);
        RECT client{};
        if (g_windowed && o_GetClientRect(g_hwnd, &client) && !PtInRect(&client, c)) {
            // Outside the window (on another program, on the desktop, on the window's border): kept off the edges,
            // so the city does not scroll because the cursor left the window.
            pt->x = pt->x < 2 ? 2 : pt->x > g_mode.cx - 3 ? g_mode.cx - 3 : pt->x;
            pt->y = pt->y < 2 ? 2 : pt->y > g_mode.cy - 3 ? g_mode.cy - 3 : pt->y;
        }
        else if (g_windowed) {
            // Inside, near an edge: the game's edge (it scrolls only on the mode's outermost pixel, which in a
            // window is one pixel next to the border that sizes it), so edge scrolling works in a window.
            const int band = EdgeBand();
            if (c.x < band) pt->x = 0;
            else if (c.x >= client.right - band) pt->x = g_mode.cx - 1;
            if (c.y < band) pt->y = 0;
            else if (c.y >= client.bottom - band) pt->y = g_mode.cy - 1;
        }
        if (++g_cursorLogs <= kMapLogs) {
            char where[MAX_PATH + 40];
            core_log("display: GetCursorPos screen (%ld,%ld) -> mode (%ld,%ld)  caller=%s", real.x, real.y, pt->x,
                     pt->y, core_describe_address(caller, where, sizeof(where)));
        }
    }
    return ok;
}

// While our panel is open no key is down for the game (keys pressed for the panel do not reach it).
SHORT WINAPI GetAsyncKeyState_detour(int vk)
{
    if (core::ui::IsOpen() && Acting(__builtin_return_address(0), nullptr)) {
        return 0;
    }
    return o_GetAsyncKeyState(vk);
}

SHORT WINAPI GetKeyState_detour(int vk)
{
    if (core::ui::IsOpen() && Acting(__builtin_return_address(0), nullptr)) {
        return 0;
    }
    return o_GetKeyState(vk);
}

BOOL WINAPI ClientToScreen_detour(HWND hwnd, LPPOINT pt)
{
    if (Acting(__builtin_return_address(0), hwnd) && hwnd) {
        return pt != nullptr; // the emulated fullscreen window sits at the origin of the emulated screen
    }
    return o_ClientToScreen(hwnd, pt);
}

BOOL WINAPI ScreenToClient_detour(HWND hwnd, LPPOINT pt)
{
    if (Acting(__builtin_return_address(0), hwnd) && hwnd) {
        return pt != nullptr;
    }
    return o_ScreenToClient(hwnd, pt);
}

BOOL WINAPI GetClientRect_detour(HWND hwnd, LPRECT r)
{
    if (Acting(__builtin_return_address(0), hwnd) && hwnd && r) {
        *r = RECT{0, 0, g_mode.cx, g_mode.cy};
        return TRUE;
    }
    return o_GetClientRect(hwnd, r);
}

BOOL WINAPI GetWindowRect_detour(HWND hwnd, LPRECT r)
{
    if (Acting(__builtin_return_address(0), hwnd) && hwnd && r) {
        *r = RECT{0, 0, g_mode.cx, g_mode.cy};
        return TRUE;
    }
    return o_GetWindowRect(hwnd, r);
}

HWND WINAPI WindowFromPoint_detour(POINT pt)
{
    if (Acting(__builtin_return_address(0), nullptr)) {
        POINT real = ToRealClient(pt);
        o_ClientToScreen(g_hwnd, &real);
        return o_WindowFromPoint(real);
    }
    return o_WindowFromPoint(pt);
}

// Placement of the game window by the exe. On a mode change the game sizes its window to the mode
// (screen_set_mode: SetWindowPos(hwnd, 0, 0,0, w,h, SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)), which with a real
// mode change is the whole screen. Here the screen keeps its size, so once the output is attached every size or
// position the exe asks for becomes the monitor rectangle; the other flags (z-order, activation) pass unchanged.
RECT MonitorRect(HWND hwnd)
{
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

void LogPlacement(const char *api, int x, int y, int w, int h, const RECT &m, void *caller)
{
    if (++g_placementLogs <= kMapLogs) {
        char where[MAX_PATH + 40];
        core_log("display: %s(game window, %d,%d %dx%d) -> kept covering the monitor [%ld,%ld,%ld,%ld]  caller=%s",
                 api, x, y, w, h, m.left, m.top, m.right, m.bottom, core_describe_address(caller, where, sizeof(where)));
    }
}

BOOL WINAPI SetWindowPos_detour(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    void *caller = __builtin_return_address(0);
    if (g_windowed && g_hwnd && hwnd == g_hwnd && FromExe(caller)) {
        flags |= SWP_NOSIZE | SWP_NOMOVE; // windowed: the window stays where the player put it
        if (after == HWND_TOPMOST) {
            flags |= SWP_NOZORDER;
        }
        return own_SetWindowPos(hwnd, after, x, y, w, h, flags);
    }
    if (g_hwnd && hwnd == g_hwnd && FromExe(caller) && (flags & (SWP_NOSIZE | SWP_NOMOVE)) != (SWP_NOSIZE | SWP_NOMOVE)) {
        RECT m = MonitorRect(hwnd);
        LogPlacement("SetWindowPos", x, y, w, h, m, caller);
        x = m.left;
        y = m.top;
        w = m.right - m.left;
        h = m.bottom - m.top;
        flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
        return own_SetWindowPos(hwnd, after, x, y, w, h, flags);
    }
    return o_SetWindowPos(hwnd, after, x, y, w, h, flags);
}

BOOL WINAPI MoveWindow_detour(HWND hwnd, int x, int y, int w, int h, BOOL repaint)
{
    void *caller = __builtin_return_address(0);
    if (g_windowed && g_hwnd && hwnd == g_hwnd && FromExe(caller)) {
        return TRUE; // windowed: kept
    }
    if (g_hwnd && hwnd == g_hwnd && FromExe(caller)) {
        RECT m = MonitorRect(hwnd);
        LogPlacement("MoveWindow", x, y, w, h, m, caller);
        x = m.left;
        y = m.top;
        w = m.right - m.left;
        h = m.bottom - m.top;
        return own_MoveWindow(hwnd, x, y, w, h, repaint);
    }
    return o_MoveWindow(hwnd, x, y, w, h, repaint);
}

// The game sets its own cursor every frame (cursor_set_kind); windowed, over the window's border and caption that
// would replace the sizing and normal arrows Windows shows there (they flickered and the border was hard to catch),
// so the exe's SetCursor is not applied while the cursor is outside the client area.
HCURSOR WINAPI SetCursor_detour(HCURSOR cursor)
{
    if (g_windowed && g_hwnd && FromExe(__builtin_return_address(0))) {
        POINT p{};
        RECT client{};
        if (o_GetCursorPos(&p) && o_ScreenToClient(g_hwnd, &p) && o_GetClientRect(g_hwnd, &client) &&
            !PtInRect(&client, p)) {
            return GetCursor();
        }
    }
    return o_SetCursor(cursor);
}

// The game sets its window's style on every mode change (dd_init: popup, no border); while windowed that would take
// the frame away, so the exe's style changes are not applied then.
LONG WINAPI SetWindowLongA_detour(HWND hwnd, int index, LONG value)
{
    if (g_windowed && g_hwnd && hwnd == g_hwnd && index == GWL_STYLE && FromExe(__builtin_return_address(0))) {
        return GetWindowLongA(hwnd, GWL_STYLE);
    }
    return o_SetWindowLongA(hwnd, index, value);
}

// ---------------------------------------------------------------------------------------------
// R2e: the window's two forms

constexpr LONG kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

RECT WorkArea(HWND hwnd)
{
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcWork;
}

// The outer rectangle for a client of cw x ch with the window's current styles.
RECT OuterFor(int x, int y, int cw, int ch)
{
    RECT r{0, 0, cw, ch};
    AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongA(g_hwnd, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongA(g_hwnd, GWL_EXSTYLE)));
    return RECT{x, y, x + (r.right - r.left), y + (r.bottom - r.top)};
}

void Settle(bool save);

// display.window_lock_cursor: windowed, the cursor is clipped to the client area while the window is the active,
// visible one; released otherwise (ClipCursor is system-wide, so it must never outlive that).
bool g_clipped = false;

void UpdateClip()
{
    const bool want = g_windowed && core_config_get()->window_lock_cursor && g_hwnd && !g_inSizeMove &&
                      GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd);
    if (want) {
        RECT c{};
        o_GetClientRect(g_hwnd, &c);
        POINT tl{c.left, c.top}, br{c.right, c.bottom};
        o_ClientToScreen(g_hwnd, &tl);
        o_ClientToScreen(g_hwnd, &br);
        RECT r{tl.x, tl.y, br.x, br.y};
        ClipCursor(&r);
        if (!g_clipped) {
            core_log("display: cursor kept inside the window [%ld,%ld,%ld,%ld]", r.left, r.top, r.right, r.bottom);
        }
        g_clipped = true;
    }
    else if (g_clipped) {
        ClipCursor(nullptr);
        g_clipped = false;
        core_log("display: cursor free again");
    }
}

void PlaceWindowed()
{
    const core_config *cfg = core_config_get();
    const RECT work = WorkArea(g_hwnd);
    const int ww = work.right - work.left, wh = work.bottom - work.top;
    int cw = cfg->window_w > 0 ? cfg->window_w : ww * 3 / 4, ch = cfg->window_h > 0 ? cfg->window_h : wh * 3 / 4;
    cw = cw < kMinClientW ? kMinClientW : cw;
    ch = ch < kMinClientH ? kMinClientH : ch;
    own_SetWindowLongA(g_hwnd, GWL_STYLE, (GetWindowLongA(g_hwnd, GWL_STYLE) & ~WS_POPUP) | kFrameStyles | WS_VISIBLE);
    RECT r = OuterFor(0, 0, cw, ch);
    const int ow = r.right - r.left, oh = r.bottom - r.top;
    int x = cfg->window_x != CORE_WINDOW_CENTER ? cfg->window_x : work.left + (ww - ow) / 2;
    int y = cfg->window_y != CORE_WINDOW_CENTER ? cfg->window_y : work.top + (wh - oh) / 2;
    if (cfg->window_x == CORE_WINDOW_CENTER || cfg->window_y == CORE_WINDOW_CENTER) {
        x = x < work.left ? work.left : x; // a window larger than the work area starts at its top left
        y = y < work.top ? work.top : y;
    }
    own_SetWindowPos(g_hwnd, HWND_NOTOPMOST, x, y, ow, oh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    Settle(false);
    core_log("display: windowed, client %dx%d at (%d,%d)%s", cw, ch, x, y,
             cfg->window_w > 0 ? "" : " (automatic size: three quarters of the monitor's work area)");
}

// Full screen: the window must cover its monitor exactly (otherwise Windows keeps the taskbar above it). Logs where
// it really is when not (0.1.1: a compatibility shim had moved it 30 px up).
void CheckCovering(const char *when)
{
    if (g_windowed || !g_hwnd || IsIconic(g_hwnd)) {
        return;
    }
    RECT wr{};
    o_GetWindowRect(g_hwnd, &wr);
    const RECT m = MonitorRect(g_hwnd);
    if (!EqualRect(&wr, &m)) {
        core_log("display: %s the window is at [%ld,%ld,%ld,%ld], not on the monitor [%ld,%ld,%ld,%ld]", when, wr.left,
                 wr.top, wr.right, wr.bottom, m.left, m.top, m.right, m.bottom);
    }
}

void PlaceFullscreen()
{
    own_SetWindowLongA(g_hwnd, GWL_STYLE, (GetWindowLongA(g_hwnd, GWL_STYLE) & ~kFrameStyles) | WS_POPUP | WS_VISIBLE);
    if (IsZoomed(g_hwnd) || IsIconic(g_hwnd)) {
        ShowWindow(g_hwnd, SW_RESTORE);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    own_SetWindowPos(g_hwnd, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    g_settled = SIZE{0, 0};
    core_log("display: fullscreen (covering the monitor [%ld,%ld,%ld,%ld])", mi.rcMonitor.left, mi.rcMonitor.top,
             mi.rcMonitor.right, mi.rcMonitor.bottom);
    CheckCovering("after going fullscreen");
}

// The window came to rest (a move or size ended, maximised, restored): its client size is what the layout uses from
// now on; with save, its normal place is written to the settings file.
void Settle(bool save)
{
    if (!g_windowed || IsIconic(g_hwnd)) {
        return;
    }
    RECT c{};
    o_GetClientRect(g_hwnd, &c);
    if (c.right > 0 && c.bottom > 0) {
        g_settled = SIZE{c.right, c.bottom};
    }
    if (!save || IsZoomed(g_hwnd)) {
        return;
    }
    RECT w{};
    o_GetWindowRect(g_hwnd, &w);
    char v[16];
    struct {
        const char *key;
        long value;
    } items[] = {{"window_width", c.right}, {"window_height", c.bottom}, {"window_x", w.left}, {"window_y", w.top}};
    for (const auto &it : items) {
        std::snprintf(v, sizeof(v), "%ld", it.value);
        core::settings::Request(core::settings::Find("display", it.key), v, true);
    }
    core_log("display: window at (%ld,%ld), client %ldx%ld (saved)", w.left, w.top, c.right, c.bottom);
}

// ---------------------------------------------------------------------------------------------
// Window subclass: mouse message coordinates into the mode

bool IsClientMouseMessage(UINT m)
{
    return (m >= WM_MOUSEMOVE && m <= WM_MBUTTONDBLCLK) || (m >= WM_XBUTTONDOWN && m <= WM_XBUTTONDBLCLK);
}

KeyFilter g_keyFilter = nullptr;
bool g_filteredSysChar = false; // a WM_SYSKEYDOWN was taken by the filter: its WM_SYSCHAR is taken too

bool IsKeyMessage(UINT m)
{
    return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR ||
           m == WM_SYSCHAR || m == WM_DEADCHAR || m == WM_SYSDEADCHAR;
}

// Mouse buttons pressed on our panel: their release is kept from the game even when the panel closed in between
// (e.g. by its Close button), so the game never sees a release without a press.
UINT g_panelButtons = 0;

UINT ButtonBit(UINT m)
{
    switch (m) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return 2;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return 4;
    default: return 0;
    }
}

void TogglePanel()
{
    if (!core::ui::IsOpen() && g_active) {
        POINT p{};
        o_GetCursorPos(&p);
        o_ScreenToClient(g_hwnd, &p);
        g_panelCursor = ToVirtual(p);
    }
    core::ui::Toggle();
}

WPARAM g_hotkeyUp = 0;     // the panel hotkey was pressed: its release is kept from the game too
bool g_hotkeyChar = false; // ... and the character TranslateMessage makes of it (letter, digit or Space hotkeys)

bool IsHotkey(UINT m, WPARAM w, unsigned vk, unsigned keyMods)
{
    if ((m != WM_KEYDOWN && m != WM_SYSKEYDOWN) || w != vk) {
        return false;
    }
    unsigned mods = ((o_GetKeyState(VK_CONTROL) & 0x8000) ? core::ui::kModCtrl : 0) |
                    ((o_GetKeyState(VK_SHIFT) & 0x8000) ? core::ui::kModShift : 0) |
                    ((o_GetKeyState(VK_MENU) & 0x8000) ? core::ui::kModAlt : 0);
    return mods == keyMods;
}

bool IsPanelHotkey(UINT m, WPARAM w)
{
    const core_config *cfg = core_config_get();
    return IsHotkey(m, w, cfg->panel_key_vk, cfg->panel_key_mods);
}

// The screenshot hotkey (when it is not also the panel's): saves the last presented frame, also while the panel is
// open (the game is paused then, so that is the picture under the panel).
bool IsScreenshotHotkey(UINT m, WPARAM w)
{
    const core_config *cfg = core_config_get();
    return !(cfg->screenshot_key_vk == cfg->panel_key_vk && cfg->screenshot_key_mods == cfg->panel_key_mods) &&
           IsHotkey(m, w, cfg->screenshot_key_vk, cfg->screenshot_key_mods);
}

// The report key (debug.report_key, default F12; R2b.1, core/diag/report.h), when it is not the panel's or the
// screenshot's.
bool IsReportHotkey(UINT m, WPARAM w)
{
    const core_config *cfg = core_config_get();
    return cfg->report_key_vk != 0 &&
           !(cfg->report_key_vk == cfg->panel_key_vk && cfg->report_key_mods == cfg->panel_key_mods) &&
           !(cfg->report_key_vk == cfg->screenshot_key_vk && cfg->report_key_mods == cfg->screenshot_key_mods) &&
           IsHotkey(m, w, cfg->report_key_vk, cfg->report_key_mods);
}

// Our panel (core/ui/panel.h): its hotkey (ui.panel_key, default Insert) opens and closes it; while it is open the
// game window's input goes to the panel instead (mouse coordinates in real client pixels). The screenshot hotkey
// (debug.screenshot_key, default F11) is handled here too, open or not. Returns true when the message is kept from
// the game.
bool PanelMessage(UINT m, WPARAM w, LPARAM l)
{
    if (IsPanelHotkey(m, w)) {
        if (!(l & 0x40000000)) { // not an auto-repeat
            TogglePanel();
        }
        g_hotkeyUp = w;
        g_hotkeyChar = true;
        return true;
    }
    if (IsScreenshotHotkey(m, w)) {
        if (!(l & 0x40000000)) {
            // With the output thread (display/cap60) the output sees the key itself and saves from the frame it
            // shows (core/output); here only for present = original.
            if (!core::output::Threaded()) {
                static core::diag::Feature screenshot{"screenshot (core/capture)"};
                core::diag::Guarded(screenshot,
                                    [](void *) { core::capture::Save(g_lastPixels, g_lastPitch, g_lastW, g_lastH); },
                                    nullptr);
            }
        }
        g_hotkeyUp = w;
        g_hotkeyChar = true;
        return true;
    }
    if (IsReportHotkey(m, w)) {
        if (!(l & 0x40000000) && !core::output::Threaded()) { // with the output thread, the output sees the key
            static core::diag::Feature report{"report key (core/display)"};
            core::diag::Guarded(report,
                                [](void *) {
                                    char label[24];
                                    std::snprintf(label, sizeof(label), "report%d",
                                                  core::diag::BeginReport("report key"));
                                    core::capture::Save(g_lastPixels, g_lastPitch, g_lastW, g_lastH, label);
                                },
                                nullptr);
        }
        g_hotkeyUp = w;
        g_hotkeyChar = true;
        return true;
    }
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        g_hotkeyChar = false; // another key: its character is its own
    }
    if ((m == WM_CHAR || m == WM_SYSCHAR) && g_hotkeyChar) {
        g_hotkeyChar = false;
        return true;
    }
    if ((m == WM_KEYUP || m == WM_SYSKEYUP) && g_hotkeyUp && w == g_hotkeyUp) {
        g_hotkeyUp = 0;
        return true;
    }
    bool open = core::ui::IsOpen();
    UINT bit = ButtonBit(m);
    bool up = m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP;
    if (!open) {
        if (up && (g_panelButtons & bit)) {
            g_panelButtons &= ~bit;
            return true;
        }
        return false;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        TogglePanel();
        return true;
    }
    if ((m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && w == VK_F4) {
        return false; // Alt+F4 still closes the game
    }
    if (IsKeyMessage(m)) {
        core::ui::QueueMessage(m, w, l);
        return true;
    }
    if (IsClientMouseMessage(m)) {
        if (bit) {
            g_panelButtons = up ? g_panelButtons & ~bit : g_panelButtons | bit;
        }
        core::ui::QueueMessage(m, w, l);
        return true;
    }
    if (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL) {
        core::ui::QueueMessage(m, w, l);
        return true;
    }
    return false;
}

// A framed full-screen picture: presses and the wheel on the background around it are kept from the game (a click
// on the parchment does nothing). A release reaches the game only when its press did, wherever it happens.
// Movement is mapped and clamped to the picture's edge (ToVirtual), so the game's own cursor, a system cursor
// (cursor_set_kind), just keeps its shape over the background.
UINT g_gameButtons = 0; // buttons whose press reached the game

bool BackgroundClick(UINT m, POINT client)
{
    UINT bit = ButtonBit(m);
    bool up = m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP;
    bool on = OnFramedPicture(client);
    if (up) {
        bool passed = (g_gameButtons & bit) != 0 || on;
        g_gameButtons &= ~bit;
        return !passed;
    }
    if (bit) { // a press or double click
        if (!on) {
            return true;
        }
        g_gameButtons |= bit;
        return false;
    }
    if (m == WM_XBUTTONDOWN || m == WM_XBUTTONUP || m == WM_XBUTTONDBLCLK || m == WM_MOUSEWHEEL ||
        m == WM_MOUSEHWHEEL) {
        return !on;
    }
    return false; // movement
}

// Settings changes (core/settings) are carried out on the game thread: a registered message to the game window wakes
// it, also while the game is paused (its main loop keeps handling messages).
UINT g_settingsMessage = 0;

void WakeForSettings()
{
    if (g_hwnd && g_settingsMessage) {
        PostMessageA(g_hwnd, g_settingsMessage, 0, 0);
    }
}

// Full screen: something other than our placement moved or sized the window off its monitor: it is put back. With
// GOG's compatibility layer that happens after every mode change of the game (the shim moves the window 30 px up
// once more), so there is no limit per run; only more than kKeepsPerSecond in one second stop it for the rest of
// that second, so a tool that keeps moving the window cannot make the two fight. The log has the first kLogged and
// then every 100th.
void KeepOnMonitor(const WINDOWPOS *wp)
{
    static bool busy = false;
    static long keeps = 0;
    static DWORD second = 0;
    static int inSecond = 0;
    constexpr int kKeepsPerSecond = 10;
    constexpr long kLogged = 5;
    if (busy || !wp || (wp->flags & SWP_NOMOVE && wp->flags & SWP_NOSIZE) || IsIconic(g_hwnd)) {
        return;
    }
    RECT wr{};
    o_GetWindowRect(g_hwnd, &wr);
    const RECT mon = MonitorRect(g_hwnd);
    if (EqualRect(&wr, &mon)) {
        return;
    }
    const DWORD now = GetTickCount();
    if (now - second >= 1000) {
        second = now;
        inSecond = 0;
    }
    if (++inSecond > kKeepsPerSecond) {
        if (inSecond == kKeepsPerSecond + 1) {
            core_log("display: the window keeps being moved off the monitor; left alone for the rest of this second");
        }
        return;
    }
    ++keeps;
    if (keeps <= kLogged || keeps % 100 == 0) {
        core_log("display: the window was moved to [%ld,%ld,%ld,%ld] in full screen; put back on the monitor "
                 "[%ld,%ld,%ld,%ld] (%ld so far)%s",
                 wr.left, wr.top, wr.right, wr.bottom, mon.left, mon.top, mon.right, mon.bottom, keeps,
                 keeps == kLogged ? "; from now on only every 100th is logged" : "");
    }
    busy = true;
    own_SetWindowPos(g_hwnd, nullptr, mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    busy = false;
}

// O1 (2026-10-28): changes of the display made outside the game (Windows' resolution or scale, the window moved
// to another monitor) were not in the log. The window event lines (core/diag/report, at most 400) were used up by
// the WM_SIZE of a window being dragged and by the WM_DISPLAYCHANGE/WM_SIZE our emulated modes send (SetMode,
// ClearMode; logged there already): those are no longer passed on, and Windows' own changes get lines of their own.
int g_ownMessages = 0;          // > 0 while SetMode/ClearMode send the game their WM_DISPLAYCHANGE and WM_SIZE
HMONITOR g_lastMonitor = nullptr;
int g_displayLines = 0;
constexpr int kDisplayLines = 100;

void DescribeMonitor(HMONITOR mon, char *out, size_t size)
{
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoA(mon, &mi)) {
        std::snprintf(out, size, "(unknown)");
        return;
    }
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    const bool hz = EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) != 0;
    std::snprintf(out, size, "%s [%ld,%ld,%ld,%ld]%s, %lu Hz", mi.szDevice, mi.rcMonitor.left, mi.rcMonitor.top,
                  mi.rcMonitor.right, mi.rcMonitor.bottom, mi.dwFlags & MONITORINFOF_PRIMARY ? " (primary)" : "",
                  hz ? dm.dmDisplayFrequency : 0ul);
}

void LogDisplayEvent(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    if (g_displayLines >= kDisplayLines) {
        return;
    }
    char mon[160];
    if (m == WM_DISPLAYCHANGE && g_ownMessages == 0) {
        ++g_displayLines;
        DescribeMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), mon, sizeof(mon));
        core_log("display: Windows changed the display: WM_DISPLAYCHANGE %dx%d, %u bits; the window's monitor %s",
                 static_cast<int>(LOWORD(l)), static_cast<int>(HIWORD(l)), static_cast<unsigned>(w), mon);
    }
    else if (m == WM_DPICHANGED) {
        ++g_displayLines;
        const RECT *r = reinterpret_cast<const RECT *>(l);
        core_log("display: WM_DPICHANGED %u dpi (%u%%); Windows suggests the window at [%ld,%ld,%ld,%ld]",
                 static_cast<unsigned>(LOWORD(w)), static_cast<unsigned>(LOWORD(w)) * 100 / 96, r->left, r->top,
                 r->right, r->bottom);
    }
    if (m == WM_WINDOWPOSCHANGED || m == WM_DISPLAYCHANGE || m == WM_DPICHANGED) {
        HMONITOR now = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (now != g_lastMonitor) {
            char was[160];
            DescribeMonitor(now, mon, sizeof(mon));
            DescribeMonitor(g_lastMonitor, was, sizeof(was));
            ++g_displayLines;
            core_log("display: the window is now on monitor %s%s%s", mon, g_lastMonitor ? "; was " : "",
                     g_lastMonitor ? was : "");
            g_lastMonitor = now;
        }
    }
}

LRESULT CALLBACK SubclassProc(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    if (m == g_settingsMessage && m != 0 && hwnd == g_hwnd) {
        core::settings::RunPending();
        return 0;
    }
    if (hwnd == g_hwnd) {
        const bool ours = g_ownMessages > 0 && (m == WM_DISPLAYCHANGE || m == WM_SIZE);
        if (!ours && !(m == WM_SIZE && g_inSizeMove)) {
            core::diag::LogWindowEvent(m, w, l); // alt-tab, focus, minimise (R2b.1)
        }
        LogDisplayEvent(hwnd, m, w, l);
    }
    if (hwnd == g_hwnd && !g_windowed && m == WM_WINDOWPOSCHANGED) {
        KeepOnMonitor(reinterpret_cast<const WINDOWPOS *>(l));
    }
    if (hwnd == g_hwnd && g_windowed) {
        if (m == WM_GETMINMAXINFO) {
            RECT r = OuterFor(0, 0, kMinClientW, kMinClientH);
            auto *mm = reinterpret_cast<MINMAXINFO *>(l);
            mm->ptMinTrackSize.x = r.right - r.left;
            mm->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        if (m == WM_ENTERSIZEMOVE) {
            g_inSizeMove = true;
            core::output::SetSizing(true);
        }
        else if (m == WM_EXITSIZEMOVE) {
            g_inSizeMove = false;
            core::output::SetSizing(false);
            Settle(true);
        }
        else if (m == WM_SIZE && !g_inSizeMove && (w == SIZE_MAXIMIZED || w == SIZE_RESTORED)) {
            Settle(false); // maximised or restored with the buttons (no size-move loop around it)
        }
        if (m == WM_ACTIVATE || m == WM_ACTIVATEAPP || m == WM_KILLFOCUS || m == WM_SETFOCUS || m == WM_SIZE ||
            m == WM_MOVE || m == WM_ENTERSIZEMOVE || m == WM_EXITSIZEMOVE) {
            UpdateClip();
        }
    }
    else if (hwnd == g_hwnd && g_clipped) {
        UpdateClip(); // fullscreen again: released
    }
    if (hwnd == g_hwnd && PanelMessage(m, w, l)) {
        return 0;
    }
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && hwnd == g_hwnd && g_keyFilter && g_keyFilter(m, w, l)) {
        g_filteredSysChar = m == WM_SYSKEYDOWN; // its WM_SYSCHAR would make Windows beep (no menu takes it)
        return 0;
    }
    if (m == WM_SYSCHAR && hwnd == g_hwnd && g_filteredSysChar) {
        g_filteredSysChar = false;
        return 0;
    }
    if (g_active && hwnd == g_hwnd) {
        bool client = IsClientMouseMessage(m);
        bool screen = m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL;
        if (client || screen) {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            POINT in = p;
            if (screen) {
                o_ScreenToClient(hwnd, &p);
            }
            if (BackgroundClick(m, p)) {
                return 0;
            }
            p = ToVirtual(p);
            l = MAKELPARAM(static_cast<WORD>(p.x), static_cast<WORD>(p.y));
            if (++g_messageLogs <= kMapLogs) {
                core_log("display: mouse message 0x%04x (%ld,%ld) -> mode (%ld,%ld)", m, in.x, in.y, p.x, p.y);
            }
        }
    }
    return CallWindowProcA(g_prevProc, hwnd, m, w, l);
}

// Like a real exclusive-mode window, cover the whole monitor (at its native resolution); windowed: our window.
void CoverMonitor(HWND hwnd)
{
    if (g_windowed) {
        static bool placed = false;
        if (!placed) {
            placed = true;
            PlaceWindowed();
        }
        return;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wr{};
    o_GetWindowRect(hwnd, &wr);
    if (!EqualRect(&wr, &mi.rcMonitor)) {
        own_SetWindowPos(hwnd, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
        core_log("display: window moved from [%ld,%ld,%ld,%ld] to cover monitor [%ld,%ld,%ld,%ld]", wr.left, wr.top,
                 wr.right, wr.bottom, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);
        CheckCovering("after moving it");
    }
}

template <typename F> void Hook(const char *name, F detour, F &original, F *own = nullptr)
{
    core_hook_install_api(name, L"user32.dll", name, reinterpret_cast<void *>(detour),
                          reinterpret_cast<void **>(&original));
    if (own) {
        // Behind a shim, user32's own function is not hooked: call it directly. Otherwise the trampoline.
        void *real = core_real_export(L"user32.dll", name);
        void *seen = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name));
        *own = real && real != seen ? reinterpret_cast<F>(real) : original;
    }
}

} // namespace

void InstallHooks()
{
    g_exeBase = reinterpret_cast<BYTE *>(GetModuleHandleW(nullptr));
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(g_exeBase);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(g_exeBase + dos->e_lfanew);
    g_exeSize = nt->OptionalHeader.SizeOfImage;
    Hook("GetSystemMetrics", &GetSystemMetrics_detour, o_GetSystemMetrics);
    Hook("GetCursorPos", &GetCursorPos_detour, o_GetCursorPos);
    Hook("ClientToScreen", &ClientToScreen_detour, o_ClientToScreen);
    Hook("ScreenToClient", &ScreenToClient_detour, o_ScreenToClient);
    Hook("GetClientRect", &GetClientRect_detour, o_GetClientRect);
    Hook("GetWindowRect", &GetWindowRect_detour, o_GetWindowRect);
    Hook("WindowFromPoint", &WindowFromPoint_detour, o_WindowFromPoint);
    Hook("SetWindowPos", &SetWindowPos_detour, o_SetWindowPos, &own_SetWindowPos);
    Hook("MoveWindow", &MoveWindow_detour, o_MoveWindow, &own_MoveWindow);
    Hook("SetWindowLongA", &SetWindowLongA_detour, o_SetWindowLongA, &own_SetWindowLongA);
    Hook("SetCursor", &SetCursor_detour, o_SetCursor);
    Hook("GetAsyncKeyState", &GetAsyncKeyState_detour, o_GetAsyncKeyState);
    Hook("GetKeyState", &GetKeyState_detour, o_GetKeyState);
}

bool Attach(HWND hwnd, core_scaling scaling, core_present_mode present)
{
    if (hwnd != g_hwnd) {
        if (g_hwnd) {
            core_log("display: game window changed from %p to %p; output stays on the first one", (void *)g_hwnd,
                     (void *)hwnd);
            return g_outputOk;
        }
        g_hwnd = hwnd;
        g_windowed = core_config_get()->windowed != 0;
        core::settings::OnChange(core::settings::Find("display", "window"), [](int, const char *v, bool initial) {
            if (!initial) {
                SetWindowed(std::strcmp(v, "windowed") == 0);
            }
            return true;
        });
        core::settings::OnChange(core::settings::Find("display", "window_lock_cursor"), [](int, const char *, bool) {
            UpdateClip();
            return true;
        });
        CoverMonitor(hwnd); // before the output starts, so SDL sizes its swap chain right away
        g_outputOk = core::output::Init(hwnd, scaling, present);
        g_prevProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassProc)));
        g_settingsMessage = RegisterWindowMessageA("EmperorEngine.Settings");
        core::settings::SetWakeup(&WakeForSettings);
        core_log("display: attached to window %p (mouse messages subclassed, previous procedure %p)", (void *)hwnd,
                 reinterpret_cast<void *>(g_prevProc));
    }
    else {
        CoverMonitor(hwnd);
    }
    return g_outputOk;
}

void SetMode(int w, int h)
{
    g_mode = SIZE{w, h};
    g_active = true;
    RECT v = Viewport();
    core_log("display: emulated mode %dx%d set (desktop unchanged); picture at [%ld,%ld,%ld,%ld] of the window "
             "(x%.3f)",
             w, h, v.left, v.top, v.right, v.bottom, w > 0 ? double(v.right - v.left) / w : 0.0);
    if (g_hwnd) {
        // What a real mode change sends: the game updates its screen size and client rectangle from these.
        ++g_ownMessages;
        SendMessageA(g_hwnd, WM_DISPLAYCHANGE, 16, MAKELPARAM(w, h));
        SendMessageA(g_hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(w, h));
        --g_ownMessages;
    }
}

void ClearMode()
{
    if (!g_active) {
        return;
    }
    g_active = false;
    int w = o_GetSystemMetrics(SM_CXSCREEN), h = o_GetSystemMetrics(SM_CYSCREEN);
    HDC dc = GetDC(nullptr);
    int bpp = dc ? GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES) : 32;
    if (dc) {
        ReleaseDC(nullptr, dc);
    }
    core_log("display: emulated mode %ldx%ld cleared; game sees the real %dx%dx%d desktop again", g_mode.cx,
             g_mode.cy, w, h, bpp);
    if (g_hwnd) {
        ++g_ownMessages;
        SendMessageA(g_hwnd, WM_DISPLAYCHANGE, bpp, MAKELPARAM(w, h));
        --g_ownMessages;
    }
}

bool ModeActive()
{
    return g_active;
}

SIZE ModeSize()
{
    if (g_active) {
        return g_mode;
    }
    return SIZE{o_GetSystemMetrics(SM_CXSCREEN), o_GetSystemMetrics(SM_CYSCREEN)};
}

void SetWindowed(bool on)
{
    if (on == g_windowed || !g_hwnd) {
        g_windowed = on;
        return;
    }
    g_windowed = on;
    if (on) {
        PlaceWindowed();
    }
    else {
        PlaceFullscreen();
    }
    UpdateClip();
}

bool Windowed()
{
    return g_windowed;
}

void SetWindowClient(int w, int h)
{
    if (!g_windowed || !g_hwnd) {
        return;
    }
    if (IsZoomed(g_hwnd) || IsIconic(g_hwnd)) {
        ShowWindow(g_hwnd, SW_RESTORE);
    }
    const RECT work = WorkArea(g_hwnd);
    RECT now{};
    o_GetWindowRect(g_hwnd, &now);
    RECT r = OuterFor(now.left, now.top, w < kMinClientW ? kMinClientW : w, h < kMinClientH ? kMinClientH : h);
    LONG ow = r.right - r.left, oh = r.bottom - r.top;
    if (ow > work.right - work.left || oh > work.bottom - work.top) { // larger than the work area: as large as fits
        RECT frame = OuterFor(0, 0, 0, 0);
        const LONG fw = frame.right - frame.left, fh = frame.bottom - frame.top;
        const LONG cw = (ow > work.right - work.left ? work.right - work.left : ow) - fw;
        const LONG ch = (oh > work.bottom - work.top ? work.bottom - work.top : oh) - fh;
        r = OuterFor(now.left, now.top, cw, ch);
        ow = r.right - r.left;
        oh = r.bottom - r.top;
    }
    LONG x = now.left, y = now.top;
    x = x + ow > work.right ? work.right - ow : x;
    y = y + oh > work.bottom ? work.bottom - oh : y;
    x = x < work.left ? work.left : x;
    y = y < work.top ? work.top : y;
    own_SetWindowPos(g_hwnd, nullptr, x, y, ow, oh, SWP_NOZORDER | SWP_NOACTIVATE);
    Settle(true);
    UpdateClip();
}

SIZE NativeSize()
{
    if (g_windowed && g_settled.cx > 0 && g_settled.cy > 0) {
        return g_settled; // the client area at rest: the game is laid out for the window
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST)
                          : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi)) {
        return SIZE{o_GetSystemMetrics(SM_CXSCREEN), o_GetSystemMetrics(SM_CYSCREEN)};
    }
    return SIZE{mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top};
}

void SetFraming(const core::output::Framing &framing)
{
    const RECT &a = g_framing.picture, &b = framing.picture;
    if (framing.on != g_framing.on || a.left != b.left || a.top != b.top || a.right != b.right ||
        a.bottom != b.bottom || framing.labels != g_framing.labels || framing.below != g_framing.below) {
        core_log("display: framed picture %s [%ld,%ld,%ld,%ld], %d units below%s", framing.on ? "on" : "off", b.left,
                 b.top, b.right, b.bottom, framing.below, framing.labels ? " (labels)" : "");
    }
    g_framing = framing;
}

bool ModePlacement(Placement *out)
{
    if (!g_active || g_mode.cx <= 0 || g_mode.cy <= 0) {
        return false;
    }
    PictureOnScreen p{};
    if (FramedPicture(&p)) {
        const LONG bw = p.box.right - p.box.left;
        *out = Placement{p.box, p.x, p.y, p.w, p.h, bw > 0 ? p.w / bw : 0.0, true};
        return bw > 0;
    }
    RECT v = Viewport();
    const double vw = v.right - v.left, vh = v.bottom - v.top;
    if (vw <= 0 || vh <= 0) {
        return false;
    }
    *out = Placement{RECT{0, 0, g_mode.cx, g_mode.cy}, double(v.left), double(v.top), vw, vh, vw / g_mode.cx, false};
    return true;
}

POINT ClientToMode(POINT client)
{
    return ToVirtual(client);
}

POINT ModeToClient(POINT mode)
{
    return ToRealClient(mode);
}

SIZE ClientSize()
{
    RECT c{0, 0, 0, 0};
    if (g_hwnd) {
        o_GetClientRect(g_hwnd, &c);
    }
    return SIZE{c.right - c.left, c.bottom - c.top};
}

void SetKeyFilter(KeyFilter filter)
{
    g_keyFilter = filter;
}

void Present(const void *pixels, int pitch, int w, int h, const char *how)
{
    core::diag::NotePresent(how);
    g_lastPixels = pixels;
    g_lastPitch = pitch;
    g_lastW = w;
    g_lastH = h;
    if (!g_outputOk || !g_hwnd || IsIconic(g_hwnd)) {
        return;
    }
    RECT c{};
    o_GetClientRect(g_hwnd, &c);
    SIZE client{c.right - c.left, c.bottom - c.top};
    if (core::output::Threaded()) {
        core::output::Submit(pixels, pitch, w, h, Viewport(), client, g_framing);
    }
    else {
        core::output::Present(pixels, pitch, w, h, Viewport(), client, g_framing);
    }
}

void FrameGone(const void *pixels)
{
    if (pixels == g_lastPixels) {
        g_lastPixels = nullptr;
    }
}

} // namespace core::display
