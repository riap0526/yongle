// Offline test of the emulated DirectDraw (not the game). Loads the given ddraw.dll (emperor_engine.ini next to it
// must select display.backend=emulated) and drives it the way Emperor does in fullscreen mode. Checks the
// virtual display (screen size, client rectangle, cursor and mouse message mapping, WM_DISPLAYCHANGE/WM_SIZE), the
// surfaces (pitch, stable bits after Unlock, BltFast/Blt/colour fill/clipping), the unimplemented methods and
// that releasing the IDirectDraw gives the real screen back, and our panel (its hotkey, ui.panel_key): with "panel"
// (default hotkey Insert) or "panel-ctrlf12" (the ini sets Ctrl+F12) it must open (present=display/cap60), take the
// input away from the game, freeze the cursor and key state the exe sees, draw (the log shows ImGui started) and give
// everything back after Esc; the hotkey only acts with exactly its modifiers. With "nopanel" (present=original) the
// hotkey must change nothing for the game except that the key itself is kept from it.
// The screenshot hotkey (debug.screenshot_key, default F11) must save the last presented frame as a PNG of the
// mode's size in shots\ next to the DLL (also while the panel is open), keep the key from the game, and save
// nothing once the primary surface is gone. The test deletes the files it made.
// The test window covers the monitor for about a second (black), as the game's window would.
// Usage: test_emu.exe <path-to-ddraw.dll> [panel|panel-ctrlf12|nopanel]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <ddraw.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what, long value = 0)
{
    std::printf("%-60s %s (%ld)\n", what, ok ? "ok" : "FAILED", value);
    if (!ok) {
        ++g_failures;
    }
}

LPARAM g_lastMouse = -1;
LPARAM g_lastDisplayChange = 0;
LPARAM g_lastSize = 0;
int g_buttons = 0; // WM_LBUTTONDOWN / WM_LBUTTONUP seen by the game
int g_keys = 0;    // WM_KEYDOWN seen by the game
int g_keyUps = 0;  // WM_KEYUP seen by the game
int g_chars = 0;   // WM_CHAR seen by the game

LRESULT CALLBACK GameProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEMOVE) g_lastMouse = l;
    if (m == WM_DISPLAYCHANGE) g_lastDisplayChange = l;
    if (m == WM_SIZE) g_lastSize = l;
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP) ++g_buttons;
    if (m == WM_KEYDOWN) ++g_keys;
    if (m == WM_KEYUP) ++g_keyUps;
    if (m == WM_CHAR) ++g_chars;
    return DefWindowProcA(h, m, w, l);
}

void Pump()
{
    MSG m;
    while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
        DispatchMessageA(&m);
    }
}

// The viewport the emulation should use: the mode's aspect, centred in the monitor.
RECT ExpectedViewport(const RECT &mon, LONG mw, LONG mh)
{
    LONG cw = mon.right - mon.left, ch = mon.bottom - mon.top, w, h;
    if (static_cast<long long>(cw) * mh <= static_cast<long long>(ch) * mw) {
        w = cw;
        h = static_cast<LONG>(static_cast<long long>(cw) * mh / mw);
    }
    else {
        h = ch;
        w = static_cast<LONG>(static_cast<long long>(ch) * mw / mh);
    }
    return RECT{(cw - w) / 2, (ch - h) / 2, (cw - w) / 2 + w, (ch - h) / 2 + h};
}

IDirectDrawSurface *MakeSurface(IDirectDraw *dd, DWORD w, DWORD h)
{
    DDSURFACEDESC d{};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    d.dwWidth = w;
    d.dwHeight = h;
    d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount = 16;
    d.ddpfPixelFormat.dwRBitMask = 0xF800;
    d.ddpfPixelFormat.dwGBitMask = 0x07E0;
    d.ddpfPixelFormat.dwBBitMask = 0x001F;
    IDirectDrawSurface *s = nullptr;
    dd->CreateSurface(&d, &s, nullptr);
    return s;
}

WORD Pixel(IDirectDrawSurface *s, LONG x, LONG y)
{
    DDSURFACEDESC d{};
    d.dwSize = sizeof(d);
    if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) {
        return 0xDEAD;
    }
    WORD v = reinterpret_cast<WORD *>(static_cast<BYTE *>(d.lpSurface) + y * d.lPitch)[x];
    s->Unlock(d.lpSurface);
    return v;
}

void SetCtrl(bool down)
{
    BYTE keys[256];
    GetKeyboardState(keys);
    keys[VK_CONTROL] = down ? 0x80 : 0;
    keys[VK_LCONTROL] = down ? 0x80 : 0;
    SetKeyboardState(keys);
}

void SetShift(bool down)
{
    BYTE keys[256];
    GetKeyboardState(keys);
    keys[VK_SHIFT] = down ? 0x80 : 0;
    keys[VK_LSHIFT] = down ? 0x80 : 0;
    SetKeyboardState(keys);
}

int LogCount(const char *text)
{
    FILE *f = std::fopen("ddraw_proxy.log", "rb");
    if (!f) {
        return 0;
    }
    static char buf[1 << 20];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    int count = 0;
    for (const char *p = std::strstr(buf, text); p; p = std::strstr(p + 1, text)) {
        ++count;
    }
    return count;
}

bool LogContains(const char *text)
{
    FILE *f = std::fopen("ddraw_proxy.log", "rb");
    if (!f) {
        return false;
    }
    static char buf[1 << 20];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return std::strstr(buf, text) != nullptr;
}

void PumpFor(int ms)
{
    for (int i = 0; i < ms / 10; ++i) {
        Pump();
        Sleep(10);
    }
}

bool g_ctrlF12 = false; // the hotkey under test: Ctrl+F12 (ini) or Insert (default)

void PressHotkey(HWND hwnd)
{
    SetCtrl(g_ctrlF12);
    SendMessageA(hwnd, WM_KEYDOWN, g_ctrlF12 ? VK_F12 : VK_INSERT, g_ctrlF12 ? 0x00580001 : 0x01520001);
    SendMessageA(hwnd, WM_KEYUP, g_ctrlF12 ? VK_F12 : VK_INSERT, g_ctrlF12 ? 0xC0580001 : 0xC1520001);
    SetCtrl(false);
}

// PNG files in shots\ (next to the DLL, which is the current directory of the tests).
int CountShots(char *newest, size_t size)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("shots\\*.png", &fd);
    int n = 0;
    FILETIME best{};
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        ++n;
        if (CompareFileTime(&fd.ftLastWriteTime, &best) >= 0) {
            best = fd.ftLastWriteTime;
            std::snprintf(newest, size, "shots\\%s", fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

// Width and height from a PNG's IHDR (0 when it is not a PNG).
void PngSize(const char *path, long *w, long *h)
{
    *w = *h = 0;
    unsigned char b[24];
    FILE *f = std::fopen(path, "rb");
    if (f && std::fread(b, 1, 24, f) == 24 && std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0 &&
        std::memcmp(b + 12, "IHDR", 4) == 0) {
        *w = (long(b[16]) << 24) | (long(b[17]) << 16) | (long(b[18]) << 8) | b[19];
        *h = (long(b[20]) << 24) | (long(b[21]) << 16) | (long(b[22]) << 8) | b[23];
    }
    if (f) {
        std::fclose(f);
    }
}

bool g_realKey = false; // press F11 as real input (the output thread polls it) instead of sending messages
char g_madeShots[16][MAX_PATH];
int g_madeCount = 0;
void (*g_presentOnce)() = nullptr; // presents one frame (the screen picture is read back from the next frame drawn)
IDirectDrawSurface *g_shotPrimary = nullptr, *g_shotBack = nullptr;

void PresentOnce()
{
    RECT full{0, 0, 1024, 768};
    g_shotPrimary->BltFast(0, 0, g_shotBack, &full, 0);
}

bool Saved(const char *file)
{
    char line[MAX_PATH + 32];
    std::snprintf(line, sizeof(line), "screenshot: saved shots\\%s", file);
    return LogContains(line);
}

// Presses F11 (or vk: F12, the report key) and waits up to 3 s (presenting frames meanwhile) for the two PNGs of one
// press: the game frame (<tag>_game_...) and the screen (<tag>_screen_...), both written completely. Their names go
// to game and screen.
bool TakeShot(HWND hwnd, char *game, char *screen, WORD vk = VK_F11)
{
    char before[64][MAX_PATH];
    int nb = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("shots\\*.png", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (nb < 64) std::snprintf(before[nb++], MAX_PATH, "%s", fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    int keys = g_keys, ups = g_keyUps;
    if (g_realKey) { // the output thread reads the key state itself (display/cap60): real input, held ~150 ms
        // It only listens while the game window is in front; Windows does not let a background process take the
        // focus, so tap Alt first (the usual way to be allowed to) and wait until the window really is in front.
        for (int i = 0; i < 50 && GetForegroundWindow() != hwnd; ++i) {
            INPUT alt[2]{};
            alt[0].type = alt[1].type = INPUT_KEYBOARD;
            alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
            alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, alt, sizeof(INPUT));
            SetForegroundWindow(hwnd);
            PumpFor(20);
        }
        Check(GetForegroundWindow() == hwnd, "test window in front (the screenshot key only acts then)");
        INPUT in[1]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = vk;
        SendInput(1, in, sizeof(INPUT));
        PumpFor(150);
        in[0].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, in, sizeof(INPUT));
        PumpFor(50);
    }
    else {
        const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyA(vk, MAPVK_VK_TO_VSC)) << 16;
        SendMessageA(hwnd, WM_KEYDOWN, vk, scan | 1);
        SendMessageA(hwnd, WM_KEYUP, vk, scan | static_cast<LPARAM>(0xC0000001));
    }
    Check(g_keys == keys && g_keyUps == ups, "screenshot key (press and release) is not passed to the game",
          g_keys - keys);
    game[0] = screen[0] = '\0';
    for (int i = 0; i < 300 && !(game[0] && screen[0]); ++i) {
        if (g_presentOnce) {
            g_presentOnce();
        }
        Pump();
        Sleep(10);
        h = FindFirstFileA("shots\\*.png", &fd);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            bool old = false;
            for (int k = 0; k < nb && !old; ++k) old = std::strcmp(before[k], fd.cFileName) == 0;
            if (old || !Saved(fd.cFileName)) {
                continue;
            }
            char *slot = std::strstr(fd.cFileName, "_game_") ? game : std::strstr(fd.cFileName, "_screen_") ? screen : nullptr;
            if (slot && !slot[0]) {
                std::snprintf(slot, MAX_PATH, "shots\\%s", fd.cFileName);
                if (g_madeCount < 16) std::snprintf(g_madeShots[g_madeCount++], MAX_PATH, "%s", slot);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    // One press, one pair: no more files appear (no auto-repeat, no second trigger).
    PumpFor(400);
    int after = 0;
    h = FindFirstFileA("shots\\*.png", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            bool old = false;
            for (int k = 0; k < nb && !old; ++k) old = std::strcmp(before[k], fd.cFileName) == 0;
            after += !old;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    Check(after == 2, "one press saves exactly one pair", after);
    return game[0] && screen[0];
}

void ScreenshotTests(HWND hwnd, bool expectPanel, int screenW, int screenH)
{
    g_realKey = expectPanel; // present=display: the output thread sees the key
    std::printf("-- screenshot (F11)\n");
    char game[MAX_PATH], screen[MAX_PATH];
    bool made = TakeShot(hwnd, game, screen);
    Check(made, "F11: two PNGs in shots\\ (game frame and screen), the log says saved");
    if (made) {
        long w = 0, h = 0;
        PngSize(game, &w, &h);
        Check(w == 1024 && h == 768 && std::strstr(game, "_game_1024x768_"), "game frame: 1024x768, named _game_1024x768_", w);
        PngSize(screen, &w, &h);
        char expect[48];
        std::snprintf(expect, sizeof(expect), "_screen_%dx%d_", screenW, screenH);
        Check(w == screenW && h == screenH && std::strstr(screen, expect), "screen: the window's size, named _screen_WxH_", w);
        Check(std::strcmp(std::strrchr(game, '_'), std::strrchr(screen, '_')) == 0, "both files have the same time", 0);
    }
    // The problem report key (debug.report_key, default F12; R2b.1, core/diag/report.h).
    std::printf("-- problem report (F12)\n");
    made = TakeShot(hwnd, game, screen, VK_F12);
    Check(made && std::strstr(game, "shots\\report1_") && std::strstr(screen, "shots\\report1_"),
          "F12: the report's two pictures, named report1_...");
    Check(LogContains("==== REPORT #1 (report key)"), "F12: the log is marked ==== REPORT #1 (report key)");
    Check(!LogContains("frame check: the output drew a copy that changed"), "no torn output frames so far");
    if (expectPanel) {
        PressHotkey(hwnd);
        PumpFor(100);
        Check(TakeShot(hwnd, game, screen), "F11 with the panel open: both saved too");
        SendMessageA(hwnd, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
        PumpFor(50);
    }
}

void DeleteShots()
{
    for (int i = 0; i < g_madeCount; ++i) {
        DeleteFileA(g_madeShots[i]);
    }
    RemoveDirectoryA("shots"); // only when empty
}

void PanelTests(HWND hwnd, bool expectPanel)
{
    std::printf("-- panel (%s, hotkey %s)\n", expectPanel ? "expected to open" : "not available with present=original",
                g_ctrlF12 ? "Ctrl+F12" : "Insert");
    POINT before{};
    GetCursorPos(&before);
    // Wrong modifiers: Ctrl+Insert for Insert, Shift+F12 for Ctrl+F12 (plain F12 is the report key, R2b.1). Not the
    // hotkey: reaches the game.
    SetCtrl(!g_ctrlF12);
    SetShift(g_ctrlF12);
    SendMessageA(hwnd, WM_KEYDOWN, g_ctrlF12 ? VK_F12 : VK_INSERT, 0x00580001);
    SetCtrl(false);
    SetShift(false);
    g_lastMouse = -1;
    SendMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(40, 50));
    Check(g_keys == 1 && g_lastMouse != -1, "hotkey with other modifiers: not the hotkey, reaches the game", g_keys);
    g_keys = 0;
    PressHotkey(hwnd);
    Check(g_keys == 0 && g_keyUps == 0, "hotkey (press and release) is not passed to the game", g_keys + g_keyUps);
    g_lastMouse = -1;
    SendMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(40, 50));
    bool open = g_lastMouse == -1;
    Check(open == expectPanel, expectPanel ? "panel open: WM_MOUSEMOVE kept from the game"
                                           : "no panel: WM_MOUSEMOVE reaches the game");
    if (!open) {
        return;
    }
    SendMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 50));
    SendMessageA(hwnd, WM_KEYDOWN, 'A', 0x001e0001);
    SendMessageA(hwnd, WM_CHAR, 'a', 0x001e0001);
    Check(g_buttons == 0 && g_keys == 0, "panel open: button and key messages kept from the game", g_buttons + g_keys);
    SetCursorPos(before.x + 37, before.y + 23);
    POINT frozen{};
    GetCursorPos(&frozen);
    POINT again{};
    SetCursorPos(before.x + 3, before.y + 7);
    GetCursorPos(&again);
    Check(frozen.x == again.x && frozen.y == again.y, "panel open: exe's GetCursorPos does not move", again.x);
    SetCtrl(true);
    Check(GetKeyState(VK_CONTROL) == 0 && GetAsyncKeyState(VK_SHIFT) == 0, "panel open: exe sees no key down");
    SetCtrl(false);
    PumpFor(300);
    Check(LogContains("panel: Dear ImGui"), "panel drawn by the output thread (ImGui context in the log)");
    SendMessageA(hwnd, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
    Check(g_keys == 0, "Esc closes the panel and is not passed to the game", g_keys);
    SendMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(40, 50));
    Check(g_buttons == 0, "release of a button pressed on the panel is kept from the game", g_buttons);
    SendMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(0, 0));
    SendMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(0, 0));
    SendMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(0, 0));
    SendMessageA(hwnd, WM_KEYDOWN, 'A', 0x001e0001);
    Check(g_lastMouse == MAKELPARAM(0, 0) && g_buttons == 2 && g_keys == 1, "panel closed: input reaches the game again",
          g_buttons);
    SetCursorPos(before.x + 37, before.y + 23);
    GetCursorPos(&again);
    Check(again.x != frozen.x || again.y != frozen.y || (before.x == 0 && before.y == 0),
          "panel closed: exe's GetCursorPos follows the cursor again", again.x);
    // The hotkey twice: open, then close.
    PressHotkey(hwnd);
    PressHotkey(hwnd);
    g_lastMouse = -1;
    SendMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(0, 0));
    Check(g_lastMouse == MAKELPARAM(0, 0), "hotkey again closes the panel");
    // Other keys and their characters still reach the game (only the hotkey's own character is kept from it).
    int keys = g_keys;
    SendMessageA(hwnd, WM_KEYDOWN, 'B', 0x00300001);
    SendMessageA(hwnd, WM_CHAR, 'b', 0x00300001);
    Check(g_keys == keys + 1 && g_chars == 1, "other keys and their characters still reach the game", g_chars);
    SetCursorPos(before.x, before.y);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ddraw.dll>\n", argv[0]);
        return 2;
    }
    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    using CreateFn = HRESULT(WINAPI *)(GUID *, IDirectDraw **, IUnknown *);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(mod, "DirectDrawCreate"));

    WNDCLASSA wc{};
    wc.lpfnWndProc = GameProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = "test_emu";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_APPWINDOW, wc.lpszClassName, "test_emu", WS_POPUP, 100, 100, 640, 480, nullptr,
                                nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    Pump();
    // Only now: the DLL makes the process DPI aware at its first CreateWindowExA (the exe entry ran before it was
    // loaded), and before that the screen size is DPI-virtualised.
    const int realW = GetSystemMetrics(SM_CXSCREEN), realH = GetSystemMetrics(SM_CYSCREEN);
    const bool expectPanel = argc > 2 && std::strncmp(argv[2], "panel", 5) == 0;
    g_ctrlF12 = argc > 2 && std::strcmp(argv[2], "panel-ctrlf12") == 0;

    IDirectDraw *dd = nullptr;
    HRESULT hr = create(nullptr, &dd, nullptr);
    Check(SUCCEEDED(hr) && dd, "DirectDrawCreate (emulated)", hr);
    if (!dd) return 1;
    hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
    Check(hr == DD_OK, "SetCooperativeLevel(0x13)", hr);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wr{};
    GetWindowRect(hwnd, &wr); // no mode yet: the real rectangle
    Check(EqualRect(&wr, &mi.rcMonitor) != 0, "window covers its monitor", wr.right - wr.left);

    Check(dd->SetDisplayMode(1024, 768, 32) == DDERR_INVALIDMODE, "SetDisplayMode(32 bpp) rejected");
    hr = dd->SetDisplayMode(1024, 768, 16);
    Check(hr == DD_OK, "SetDisplayMode(1024x768x16)", hr);
    Check(g_lastDisplayChange == MAKELPARAM(1024, 768), "WM_DISPLAYCHANGE 1024x768 sent");
    Check(g_lastSize == MAKELPARAM(1024, 768), "WM_SIZE 1024x768 sent");
    Check(GetSystemMetrics(SM_CXSCREEN) == 1024 && GetSystemMetrics(SM_CYSCREEN) == 768,
          "GetSystemMetrics -> 1024x768 (exe caller)", GetSystemMetrics(SM_CXSCREEN));
    RECT cr{};
    GetClientRect(hwnd, &cr);
    Check(cr.left == 0 && cr.top == 0 && cr.right == 1024 && cr.bottom == 768, "GetClientRect -> 0,0,1024,768",
          cr.right);
    POINT p0{10, 20};
    ClientToScreen(hwnd, &p0);
    Check(p0.x == 10 && p0.y == 20, "ClientToScreen is identity");

    RECT vp = ExpectedViewport(mi.rcMonitor, 1024, 768);
    CURSORINFO ci{}; // not hooked (GetPhysicalCursorPos ends up in the hooked GetCursorPos)
    ci.cbSize = sizeof(ci);
    GetCursorInfo(&ci);
    POINT real = ci.ptScreenPos;
    POINT got{};
    GetCursorPos(&got);
    LONG ex = (real.x - mi.rcMonitor.left - vp.left) * 1024 / (vp.right - vp.left);
    LONG ey = (real.y - mi.rcMonitor.top - vp.top) * 768 / (vp.bottom - vp.top);
    ex = ex < 0 ? 0 : ex > 1023 ? 1023 : ex;
    ey = ey < 0 ? 0 : ey > 767 ? 767 : ey;
    if (real.x - mi.rcMonitor.left < vp.left) ex = 0;
    if (real.y - mi.rcMonitor.top < vp.top) ey = 0;
    Check(std::abs(got.x - ex) <= 1 && std::abs(got.y - ey) <= 1, "GetCursorPos mapped into 1024x768", got.x);
    std::printf("  real cursor (%ld,%ld) -> (%ld,%ld), viewport [%ld,%ld,%ld,%ld]\n", real.x, real.y, got.x, got.y,
                vp.left, vp.top, vp.right, vp.bottom);

    LONG cx = (vp.left + vp.right) / 2, cy = (vp.top + vp.bottom) / 2;
    SendMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
    Check(std::abs(GET_X_LPARAM(g_lastMouse) - 512) <= 1 && std::abs(GET_Y_LPARAM(g_lastMouse) - 384) <= 1,
          "WM_MOUSEMOVE at viewport centre -> (512,384)", GET_X_LPARAM(g_lastMouse));
    SendMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(0, 0));
    Check(g_lastMouse == MAKELPARAM(0, 0), "WM_MOUSEMOVE in the black bar -> clamped to (0,0)");
    SendMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(mi.rcMonitor.right - mi.rcMonitor.left - 1, 5));
    Check(GET_X_LPARAM(g_lastMouse) == 1023, "WM_MOUSEMOVE at right edge -> x 1023", GET_X_LPARAM(g_lastMouse));

    DDSURFACEDESC md{};
    md.dwSize = sizeof(md);
    hr = dd->GetDisplayMode(&md);
    Check(hr == DD_OK && md.dwWidth == 1024 && md.ddpfPixelFormat.dwRGBBitCount == 16 &&
              md.ddpfPixelFormat.dwGBitMask == 0x07E0,
          "GetDisplayMode -> 1024x768 565", hr);

    DDSURFACEDESC pd{};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DDSD_CAPS;
    pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface *primary = nullptr;
    hr = dd->CreateSurface(&pd, &primary, nullptr);
    Check(SUCCEEDED(hr) && primary, "CreateSurface(primary)", hr);
    DDSURFACEDESC ld{};
    ld.dwSize = sizeof(ld);
    hr = primary->Lock(nullptr, &ld, DDLOCK_WAIT, nullptr);
    Check(hr == DD_OK && ld.lpSurface && ld.lPitch == 2048 && ld.dwWidth == 1024 && ld.dwHeight == 768,
          "primary Lock: 1024x768, pitch 2048", ld.lPitch);
    primary->Unlock(nullptr);

    IDirectDrawSurface *back = MakeSurface(dd, 1024, 768);
    Check(back != nullptr, "CreateSurface(1024x768 565 system memory)");
    DDSURFACEDESC bd{};
    bd.dwSize = sizeof(bd);
    back->Lock(nullptr, &bd, DDLOCK_WAIT, nullptr);
    WORD *bits = static_cast<WORD *>(bd.lpSurface);
    back->Unlock(nullptr);
    // Like the game: keep drawing through the pointer after Unlock.
    for (int y = 0; y < 768; ++y) {
        for (int x = 0; x < 1024; ++x) {
            bits[y * (bd.lPitch / 2) + x] = static_cast<WORD>((x * 7 + y * 13) & 0xFFFF);
        }
    }
    RECT full{0, 0, 1024, 768};
    hr = primary->BltFast(0, 0, back, &full, 0);
    Check(hr == DD_OK, "primary BltFast(back, 0,0,1024,768)", hr);
    Check(Pixel(primary, 517, 301) == static_cast<WORD>(517 * 7 + 301 * 13), "primary pixel matches back buffer");

    RECT big{0, 0, 2560, 1440}; // oversize source rectangle (a DPI-virtualised game would pass one): clipped
    hr = primary->BltFast(0, 0, back, &big, 0);
    Check(hr == DD_OK, "BltFast with an oversize source rectangle is clipped", hr);

    DDBLTFX fx{};
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = 0x1234;
    RECT fr{10, 10, 20, 20};
    hr = back->Blt(&fr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    Check(hr == DD_OK && Pixel(back, 15, 15) == 0x1234 && Pixel(back, 25, 15) != 0x1234, "Blt colour fill", hr);

    IDirectDrawSurface *pic = MakeSurface(dd, 800, 600);
    DDSURFACEDESC pdx{};
    pdx.dwSize = sizeof(pdx);
    pic->Lock(nullptr, &pdx, DDLOCK_WAIT, nullptr);
    for (int y = 0; y < 600; ++y) {
        for (int x = 0; x < 800; ++x) {
            static_cast<WORD *>(pdx.lpSurface)[y * (pdx.lPitch / 2) + x] = static_cast<WORD>(x ^ y);
        }
    }
    pic->Unlock(nullptr);
    RECT dr{112, 84, 912, 684};
    hr = back->Blt(&dr, pic, nullptr, DDBLT_WAIT | DDBLT_ASYNC, nullptr);
    Check(hr == DD_OK && Pixel(back, 112 + 300, 84 + 200) == static_cast<WORD>(300 ^ 200), "Blt 800x600 centred",
          hr);

    Check(back->IsLost() == DD_OK && back->Restore() == DD_OK, "IsLost / Restore -> DD_OK");
    DDPIXELFORMAT pf{};
    pf.dwSize = sizeof(pf);
    Check(primary->GetPixelFormat(&pf) == DD_OK && pf.dwRBitMask == 0xF800, "GetPixelFormat -> 565");
    DDSURFACEDESC gd{};
    gd.dwSize = sizeof(gd);
    Check(primary->GetSurfaceDesc(&gd) == DD_OK && (gd.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE),
          "GetSurfaceDesc(primary)");
    Check(primary->Flip(nullptr, 0) == DDERR_UNSUPPORTED, "unimplemented Flip -> DDERR_UNSUPPORTED");
    IDirectDrawClipper *clip = nullptr;
    Check(dd->CreateClipper(0, &clip, nullptr) == DDERR_UNSUPPORTED, "unimplemented CreateClipper -> DDERR_UNSUPPORTED");

    for (int i = 0; i < 30; ++i) { // a few presents for the log
        primary->BltFast(0, 0, back, &full, 0);
        Pump();
        Sleep(10);
    }
    if (argc > 2) {
        g_shotPrimary = primary;
        g_shotBack = back;
        g_presentOnce = &PresentOnce;
        ScreenshotTests(hwnd, expectPanel, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
        g_presentOnce = nullptr;
        PanelTests(hwnd, expectPanel);
    }
    // What one presented frame costs the calling (game) thread: the copy into the primary plus, depending on
    // display.present, drawing and presenting with SDL (original) or the copy into the output thread's triple buffer.
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 200; ++i) {
        primary->BltFast(0, 0, back, &full, 0);
    }
    QueryPerformanceCounter(&t1);
    std::printf("BltFast cost to the calling thread: %.3f ms per 1024x768 frame (200 frames)\n",
                (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / 200);
    Pump();
    // The game sizes its window to the mode (screen_set_mode); the emulation keeps it covering the monitor.
    SetWindowPos(hwnd, nullptr, 0, 0, 1024, 768, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    WINDOWPLACEMENT wp{}; // GetWindowRect is hooked (reports the mode), so read the real size from the placement

    wp.length = sizeof(wp);
    GetWindowPlacement(hwnd, &wp);
    Check(wp.rcNormalPosition.right - wp.rcNormalPosition.left == mi.rcMonitor.right - mi.rcMonitor.left,
          "game SetWindowPos(1024x768) keeps the window covering the monitor",
          wp.rcNormalPosition.right - wp.rcNormalPosition.left);
    // A size change behind SDL's back (not through the hooked APIs): the next present must rebuild the output.
    wp.rcNormalPosition = RECT{0, 0, 1280, 720};
    SetWindowPlacement(hwnd, &wp);
    Check(primary->BltFast(0, 0, back, &full, 0) == DD_OK, "present after a window size change");
    for (int i = 0; i < 20; ++i) { // give an output thread (present=display) time to draw it and rebuild
        Pump();
        Sleep(10);
    }

    hr = dd->SetDisplayMode(640, 480, 16);
    Check(hr == DD_OK && GetSystemMetrics(SM_CXSCREEN) == 640, "SetDisplayMode(640x480) -> metrics 640", hr);

    Check(pic->Release() == 0, "800x600 surface final Release == 0");
    Check(back->Release() == 0, "back buffer final Release == 0");
    Check(primary->Release() == 0, "primary final Release == 0");
    Check(dd->Release() == 0, "DirectDraw final Release == 0");
    if (argc > 2 && !expectPanel) { // present=original: the game thread saves from the primary, which is gone now
        char newest[MAX_PATH] = "";
        int before = CountShots(newest, sizeof(newest));
        int noPicture = LogCount("screenshot: no picture to save yet");
        SendMessageA(hwnd, WM_KEYDOWN, VK_F11, 0x00570001);
        SendMessageA(hwnd, WM_KEYUP, VK_F11, 0xC0570001);
        PumpFor(200);
        Check(CountShots(newest, sizeof(newest)) == before &&
                  LogCount("screenshot: no picture to save yet") == noPicture + 1,
              "F11 after the primary is released: nothing saved (logged)");
    }
    if (argc > 2) {
        DeleteShots();
    }
    Check(GetSystemMetrics(SM_CXSCREEN) == realW && GetSystemMetrics(SM_CYSCREEN) == realH,
          "after release: real screen size again", GetSystemMetrics(SM_CXSCREEN));
    Check(g_lastDisplayChange == MAKELPARAM(realW, realH), "WM_DISPLAYCHANGE back to the real size");

    DestroyWindow(hwnd);
    // The DLL stays loaded (as in the game): SDL and its renderer live until the process ends.
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
