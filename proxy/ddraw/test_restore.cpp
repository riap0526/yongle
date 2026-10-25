// Offline test of the failure fallbacks of M4, R1 and P2 (not the game): the real detours of games/emperor run on a
// copy of the game's address range (emperor::g_imageShift, games/emperor/site.h) where the game's functions are
// small fakes. A fake faults on purpose; the test then checks that the game state the detour had changed is back
// to the game's own values, that the feature is switched off, and what the detour does afterwards (the fallback in
// docs/archive/resolution_ui_r2b.md §1).
//
// R2b: the info windows the game always opens at (0, 0x2a) (Mill, Warehouse, Trading Quay, Trading Station) are shown
// elsewhere without moving them in the game (drawn into a copy of the screen, the mouse moved back); the sidebar
// backdrop's edges below and right of the sidebar, row by row, from the player's own China_Interface pack when
// TEST_RESTORE_PACK names it (run_tests.ps1 passes testbed\DATA\China_Interface.sg3; skipped otherwise).
//
// Each scenario runs in a child process of its own (a feature switched off stays off for the rest of a process):
//   test_restore.exe            runs every scenario and prints one line each
//   test_restore.exe <name>     one scenario (exit code 0 = passed)
#include "core/assets/sg3.h"
#include "core/ddraw/surface_memory.h"
#include "core/diag/guard.h"
#include "core/display/logical_size.h"
#include "core/log.h"
#include "games/emperor/china_map.h"
#include "games/emperor/display_options.h"
#include "games/emperor/emperor.h"
#include "games/emperor/resolution.h"
#include "games/emperor/scroll.h"
#include "games/emperor/site.h"
#include "games/emperor/ui_fixes.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Stubs for the parts of the DLL these files call that are not under test

namespace core::display {
volatile bool g_nativeFaults = false;
SIZE NativeSize()
{
    if (g_nativeFaults) {
        *static_cast<volatile int *>(nullptr) = 1;
    }
    return SIZE{2560, 1440};
}
SIZE ModeSize()
{
    return SIZE{1024, 768};
}
using KeyFilter = bool (*)(UINT, WPARAM, LPARAM);
void SetKeyFilter(KeyFilter) {}
bool Windowed() { return false; }
void SetWindowClient(int, int) {}
} // namespace core::display

namespace core::output {
bool FrameWanted() { return false; }
void HoldNextFrame(DWORD) {}
void TakeCounts(long *a, long *b) { *a = *b = 0; }
int RefreshHz() { return 60; }
} // namespace core::output

namespace core::ui {
bool g_open = false;
bool IsOpen() { return g_open; }
bool WantsGameFrames() { return g_open; }
void SetTitle(const char *) {}
using Section = void (*)();
void AddSection(const char *, Section) {}
void PerfWidget() {}
} // namespace core::ui

namespace core::capture {
using TagProvider = void (*)(char *, size_t);
void SetTagProvider(TagProvider) {}
bool SaveOne(const void *, int, int, int, const char *, const char *) { return true; }
} // namespace core::capture

extern "C" unsigned core_timer_resolution_us(void) { return 1000; }
extern "C" void core_timer_end(void) {}

namespace emperor::fullscreen {
bool BorderPicture(int w, int h, std::vector<WORD> *out) // a plain frame: 0x1111 border, 0x2222 inside
{
    out->assign(static_cast<size_t>(w) * h, 0x2222);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (x < 16 || y < 16 || x >= w - 16 || y >= h - 16) {
                (*out)[static_cast<size_t>(y) * w + x] = 0x1111;
            }
        }
    }
    return true;
}
void RegisterSettings() {}
bool Attach() { return false; }
void OnArtLoaded(int, int, int) {}
void OnLoop() {}
void OnPresent() {}
void OnGameEnd() {}
} // namespace emperor::fullscreen

namespace {

using emperor::g_imageShift;

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("  %-74s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

bool LogContains(const char *text)
{
    FILE *f = std::fopen("test_restore.log", "rb");
    if (!f) {
        return false;
    }
    static char buf[1 << 20];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return std::strstr(buf, text) != nullptr;
}

// The game's memory: our copy of 0x400000 .. 0x1e60000.
constexpr DWORD kImageBase = 0x400000, kImageSize = 0x1a60000;

template <typename T> volatile T &At(DWORD address)
{
    return *reinterpret_cast<volatile T *>(address + g_imageShift);
}

void Thunk(DWORD address, const void *target) // jmp target at the game's address
{
    BYTE *p = reinterpret_cast<BYTE *>(address + g_imageShift);
    p[0] = 0xe9;
    *reinterpret_cast<DWORD *>(p + 1) = reinterpret_cast<DWORD>(target) - reinterpret_cast<DWORD>(p + 5);
}

template <typename F> F DetourOf(const emperor::Detour *list, int n, const char *name)
{
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(list[i].site->name, name) == 0) {
            return reinterpret_cast<F>(list[i].detour);
        }
    }
    std::printf("no detour %s\n", name);
    ExitProcess(3);
}

template <typename F> F Scroll(const char *name)
{
    int n;
    const emperor::Detour *d = emperor::scroll::Detours(&n);
    return DetourOf<F>(d, n, name);
}

template <typename F> F Res(const char *name)
{
    int n;
    const emperor::Detour *d = emperor::resolution::Detours(&n);
    return DetourOf<F>(d, n, name);
}

template <typename F> F Main(const char *name)
{
    int n;
    const emperor::Detour *d = emperor::MainDetours(&n);
    return DetourOf<F>(d, n, name);
}

// A fake faults once when its name is the armed one.
const char *volatile g_fault = "";
bool Fires(const char *name)
{
    if (std::strcmp(g_fault, name) == 0) {
        g_fault = "";
        return true;
    }
    return false;
}
void Fault()
{
    *static_cast<volatile int *>(nullptr) = 1;
}

// ---------------------------------------------------------------------------------------------
// The game's variables used below (docs/symbols.md)

constexpr DWORD aViewX = 0x101d0ac, aViewY = 0x101d0b0, aOriginX = 0xadf8f0, aOriginY = 0xadf8ec,
                aCols = 0xadf8c4, aRows = 0xadf8c0, aTileW = 0xadf8b8, aTileHalfH = 0xadf8ac,
                aCursorX = 0x10de030, aCursorY = 0x10de02c, aDesktopW = 0x10de024, aDesktopH = 0x10de020,
                aScrolling = 0x10de038, aScrollSpeed = 0x10de0e0, aFrame = 0x10de418, aClipL = 0xadf89c,
                aClipT = 0xadf894, aClipR = 0xadf898, aClipB = 0xadf890, aDrawBuffer = 0x1156df8,
                aPitch = 0x10de000, aDdW = 0x10de008, aDdH = 0x10de004, aScreenW = 0x10de01c, aScreenH = 0x10de018,
                aSidebarX = 0x13f8e0c, aSidebarX2 = 0x13f8e08, aGameScreen = 0x10de045, aScreenMode = 0x10de0d8,
                aSmallTiles = 0x13f8e20, aInfoX = 0x1312b40, aInfoY = 0x1312b3c, aInfoCols = 0x1312b38,
                aInfoRows = 0x1312b34, aMouseX = 0x10de0a0, aMouseY = 0x10de09c, aArtOX = 0x10c7334,
                aArtOY = 0x10c7330, aArtW = 0x10c733c, aArtH = 0x10c7338, aInfoKind = 0x1312b04,
                aTradePopup = 0x1e50a3c;

// ---------------------------------------------------------------------------------------------
// R2c (games/emperor/china_map): fakes of the China map's functions

constexpr DWORD aMapOriginX = 0x82b7b0, aMapOriginY = 0x82b7b4, aMapScrollX = 0xc05900, aMapScrollY = 0xc058fc,
                aMapW = 0x82b7a8, aMapH = 0x82b7ac, aMapNumber = 0xc058d4;

WORD MapPattern(int x, int y) // the map's pixel (x, y): never the sentinel, never 0
{
    return static_cast<WORD>(0x4000 + ((x * 7 + y * 13) & 0x3fff));
}

struct MapCalls {
    int draws, tops, inputs, campaign, clicks;
    BYTE *drawBuffer, *topBuffer;
    int mouseX, mouseY;
} g_map;

using MapVoid_fn = void(__cdecl *)();
using MapTop_fn = int(__cdecl *)(int);
constexpr WORD kSentinel16 = 0x1234;
MapTop_fn g_topBarHook = nullptr; // the hook in front of top_bar_piece_and_popup, as MinHook puts it

void __cdecl FakeMapDraw() // as china_map_draw: origin (0, 40), the picture at (0, 40), top_bar_piece_and_popup(1)
{
    ++g_map.draws;
    g_map.drawBuffer = At<BYTE *>(aDrawBuffer);
    if (Fires("china_map_draw")) {
        Fault();
    }
    At<int>(aMapOriginX) = 0;
    At<int>(aMapOriginY) = 0x28;
    BYTE *buf = At<BYTE *>(aDrawBuffer);
    const int pitch = At<int>(aPitch), w = At<int>(aMapW), h = At<int>(aMapH);
    for (int y = 0; y < h; ++y) {
        WORD *row = reinterpret_cast<WORD *>(buf + static_cast<size_t>(y + 0x28) * pitch);
        for (int x = 0; x < w; ++x) {
            row[x] = MapPattern(x, y);
        }
    }
    g_topBarHook(1);
}

int __cdecl FakeTopBar(int)
{
    ++g_map.tops;
    g_map.topBuffer = At<BYTE *>(aDrawBuffer);
    return 0;
}

void __cdecl FakeMapInput()
{
    ++g_map.inputs;
    g_map.mouseX = At<int>(aMouseX);
    g_map.mouseY = At<int>(aMouseY);
}

void __cdecl FakeCampaignDraw()
{
    ++g_map.campaign;
    g_map.mouseX = At<int>(aMouseX);
    g_map.mouseY = At<int>(aMouseY);
    // a marker on the map at (100, 100) of the map, and the sidebar's strip at the screen's right
    BYTE *buf = At<BYTE *>(aDrawBuffer);
    const int pitch = At<int>(aPitch);
    reinterpret_cast<WORD *>(buf + static_cast<size_t>(0x28 + 100) * pitch)[100] = 0x0001;
    reinterpret_cast<WORD *>(buf + static_cast<size_t>(5) * pitch)[At<int>(aSidebarX) + 5] = 0x0002;
}

unsigned __cdecl FakeAnim(int, unsigned frame) // anim_advance_ui: one step while frame_timer_flags[0] is set
{
    return At<BYTE>(0x8901cc) ? frame + 1 : frame;
}

struct PanelCall {
    int calls, x, y, w, h;
} g_panel;

void __fastcall FakePanel(void *, void *, int x, int y, int w, int h) // __thiscall: ecx = the panel, ret 0x10
{
    g_panel = PanelCall{g_panel.calls + 1, x, y, w, h};
}

int g_frameDraws = 0;
void __cdecl FakeFrameDraw()
{
    ++g_frameDraws;
}

void __cdecl FakeCampaignClick()
{
    ++g_map.clicks;
    g_map.mouseX = At<int>(aMouseX);
    g_map.mouseY = At<int>(aMouseY);
}


// ---------------------------------------------------------------------------------------------
// Fakes of the game's functions

struct Seen {
    int calls = 0;
    int a = 0, b = 0, c = 0, d = 0;
    BYTE *buffer = nullptr;
};
Seen g_render, g_convert, g_clamp, g_drag, g_viewport, g_draws, g_info, g_loop;
int g_simDueInside = -1;

void __cdecl FakeMessagePanel();                 // below (R2b.1)
void __cdecl FakeFillRect(int, int, int, int, int); // below (R2b.1)

// Pixels the fake city_render draws (R2b.1-5 / -6: what is shown of the city past its clip).
struct Mark {
    int x, y;
    WORD v;
};
Mark g_marks[8];
int g_markCount = 0;

void __cdecl FakeCityRender(int, int, int)
{
    ++g_render.calls;
    g_render.buffer = At<BYTE *>(aDrawBuffer);
    g_render.a = At<int>(aClipR);
    g_render.b = At<int>(aClipB);
    g_render.c = At<int>(aCols);
    g_render.d = At<int>(aRows);
    if (Fires("city_render")) {
        Fault();
    }
    for (int i = 0; i < g_markCount; ++i) {
        reinterpret_cast<WORD *>(g_render.buffer + g_marks[i].y * At<int>(aPitch))[g_marks[i].x] = g_marks[i].v;
    }
}

int __cdecl FakeScreenToGrid(int, int)
{
    ++g_convert.calls;
    g_convert.a = At<int>(aOriginX);
    g_convert.b = At<int>(aOriginY);
    g_convert.c = At<int>(aCols);
    g_convert.d = At<int>(aRows);
    if (Fires("screen_to_grid")) {
        Fault();
    }
    return 1234;
}

void __cdecl FakeGridToScreen(int, int, int *x, int *y)
{
    *x = At<int>(aOriginX);
    *y = At<int>(aOriginY);
}

int __cdecl FakeGetDirection()
{
    return 3; // the game's own direction (right-down)
}

void __cdecl FakeScrollStep(int) {}

void __cdecl FakeViewClamp()
{
    ++g_clamp.calls;
    if (Fires("view_clamp")) {
        Fault();
    }
}

void __cdecl FakeNothing() {}

void __fastcall FakeDragStart(int *) {}

void __fastcall FakeDragDraw(int *drag)
{
    ++g_drag.calls;
    g_drag.a = drag[2];
    g_drag.b = drag[3];
    if (Fires("drag_draw")) {
        Fault();
    }
}

void __cdecl FakeModeGetSize(int mode)
{
    if (mode == 3) {
        At<int>(aScreenW) = 1024;
        At<int>(aScreenH) = 768;
    }
}

void __cdecl FakeChangeMode(int mode)
{
    using Fn = void(__cdecl *)(int);
    Res<Fn>("screen_mode_get_size")(mode); // the game calls the (hooked) mode size
    if (Fires("change_mode")) {
        Fault();
    }
}

constexpr DWORD aCityClipBottom = 0xadf890;

// viewport_init (__cdecl): the parts the tests look at, written as the game writes them (clip right =
// (tile width + 2) x cols - 2 + origin x, clip bottom = (rows - 1) x half-height + origin y).
void __cdecl FakeViewportInit(int, int, int originX, int originY, int cols, int rows, int tileW, int tileH)
{
    ++g_viewport.calls;
    if (Fires("viewport_init")) {
        Fault();
    }
    g_viewport.a = cols;
    g_viewport.b = rows;
    At<int>(aCols) = cols;
    At<int>(aRows) = rows;
    At<int>(aCityClipBottom) = (rows - 1) * (tileH / 2) + originY;
    At<int>(aClipR) = (tileW + 2) * cols - 2 + originX;
}

bool g_fixedKind = false; // the next window is one of the kinds the game opens at (0, 0x2a)

void __cdecl FakeInfoPlace()
{
    ++g_info.calls;
    if (g_fixedKind) { // info_window_place's second special case
        At<int>(aInfoKind) = 2;
        At<int>(aInfoX) = 0;
        At<int>(aInfoY) = 0x2a;
        At<int>(aInfoCols) = 0x1e;
        At<int>(aInfoRows) = 0x20;
        return;
    }
    At<int>(aInfoKind) = 1;
    At<int>(aInfoX) = 8;
    At<int>(aInfoY) = 0x20;
    At<int>(aInfoCols) = 0x1d;
    At<int>(aInfoRows) = 20;
}

int __cdecl FakeFixedKind(int)
{
    return g_fixedKind ? 1 : 0;
}

// building_get 0x47f1b0: a holder of the building object, whose vtable slot 0x178 is the storage window draw (or,
// with g_unknownClass, something else).
bool g_unknownClass = false;
void *g_vtable[0x200 / 4];
void *g_object = g_vtable;
void *g_holder = &g_object;

void *g_buildingList = nullptr; // `this` the detour passed (the game's building list 0x8c7634)

void **__attribute__((thiscall)) FakeBuildingGet(void *list, int)
{
    g_buildingList = list;
    g_vtable[0x178 / 4] = reinterpret_cast<void *>((g_unknownClass ? 0x5a0000 : 0x5d74e0) + g_imageShift);
    return reinterpret_cast<void **>(&g_holder);
}

// Pixels of the back buffer (RGB565, 1920 x 1080).
WORD &Px(int x, int y)
{
    return reinterpret_cast<WORD *>(At<BYTE *>(aDrawBuffer))[y * 1920 + x];
}

// A fixed-position window's draw (storage / trade window, __thiscall with x, y, cols, rows, flag): a mark inside its
// frame, one at an absolute y the way the game draws the employee panel ((rows - 4) x 16, without the window's y),
// one elsewhere on the screen; optionally the trade popup; optionally a fault.
struct DrawSeen {
    int calls = 0;
    BYTE *buffer = nullptr;
    int mx = 0, my = 0;
};
DrawSeen g_window, g_popup, g_input;
int g_buttonsX = 0, g_buttonsY = 0;
bool g_callPopup = false;

WORD *Target(int x, int y)
{
    return reinterpret_cast<WORD *>(At<BYTE *>(aDrawBuffer)) + y * 1920 + x;
}

void __attribute__((thiscall)) FakeWindowDraw(void *self, int x, int y, int, int rows, int)
{
    ++g_window.calls;
    g_window.buffer = At<BYTE *>(aDrawBuffer);
    g_window.mx = At<int>(aMouseX);
    g_window.my = At<int>(aMouseY);
    *Target(x + 5, y + 5) = 0xabcd;
    *Target(x + 5, (rows - 4) * 16 + 2) = 0x1234;
    *Target(1000, 900) = 0x7777;
    if (g_callPopup) {
        using Fn = void(__fastcall *)(void *);
        Res<Fn>("trade_popup_draw")(self);
    }
    if (Fires("window_draw")) {
        Fault();
    }
}

// The mill window's foreground (__thiscall with x, y, cols, rows; ret 0x10): the food list at the places the window
// draw laid it out (the game's own place of the window, not the x, y it is given).
DrawSeen g_foreground;
int g_foregroundX = -1, g_foregroundY = -1;

void __attribute__((thiscall)) FakeMillForeground(void *, int x, int y, int, int)
{
    ++g_foreground.calls;
    g_foreground.buffer = At<BYTE *>(aDrawBuffer);
    g_foregroundX = x;
    g_foregroundY = y;
    *Target(30, 0x2a + 100) = 0xbeef;
    if (Fires("mill_foreground")) {
        Fault();
    }
}

void __fastcall FakePopup(void *)
{
    ++g_popup.calls;
    g_popup.buffer = At<BYTE *>(aDrawBuffer);
    g_popup.mx = At<int>(aMouseX);
    g_popup.my = At<int>(aMouseY);
    *Target(1100, 950) = 0x5555;
}

void __cdecl FakeInfoInput()
{
    ++g_input.calls;
    g_input.mx = At<int>(aMouseX);
    g_input.my = At<int>(aMouseY);
}

void __cdecl FakeButtons(int x, int y, void *, int)
{
    g_buttonsX = x;
    g_buttonsY = y;
}

// The sidebar's own panels (group 0x27f at y 0, 0x274 at y 458), from the pack, as sidebar_draw_panels draws them.
std::vector<WORD> g_upper, g_lower;

void __cdecl FakeSidebarPanels()
{
    const int x0 = At<int>(aSidebarX);
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<WORD> &img = pass ? g_lower : g_upper;
        const int h = pass ? 310 : 458, top = pass ? 458 : 0;
        if (img.size() != static_cast<size_t>(226 * h)) {
            continue;
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < 226; ++x) {
                WORD v = img[y * 226 + x];
                if (v != 0xf81f) {
                    Px(x0 + x, top + y) = v;
                }
            }
        }
    }
}

constexpr int kArtGroup = 0x1a01;
int __attribute__((thiscall)) FakeImageGetId(void *, int group)
{
    return group; // image id = group, for the test
}

void __attribute__((thiscall)) FakeImageSize(void *, int id, int *w, int *h)
{
    if (Fires("image_size")) {
        Fault();
    }
    *w = id == kArtGroup ? 848 : id == 0x27e ? 798 : id == 0x27e + (2045 - 1221) ? 226 : 100;
    *h = id == kArtGroup ? 746 : id == 0x27e ? 40 : id == 0x27e + (2045 - 1221) ? 768 : 8;
}

int g_bottomEdgeDraws = 0; // the city's bottom edge (group 0x2d6; image id = group in this test)

int __cdecl FakeImageDraw(int id, int, int, int, int, int)
{
    ++g_draws.calls;
    g_bottomEdgeDraws += id == 0x2d6;
    if (Fires("image_draw")) {
        Fault();
    }
    return 1;
}

int __cdecl FakeLoopStep()
{
    ++g_loop.calls;
    using Fn = int(__cdecl *)();
    g_simDueInside = Main<Fn>("sim_tick_due")();
    if (Fires("loop_step")) {
        Fault();
    }
    return 5;
}

int __cdecl FakeSimDue()
{
    return 1;
}

void Setup()
{
    BYTE *block = static_cast<BYTE *>(VirtualAlloc(nullptr, kImageSize, MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_EXECUTE_READWRITE));
    if (!block) {
        std::printf("cannot allocate the game's address range\n");
        ExitProcess(3);
    }
    g_imageShift = reinterpret_cast<DWORD>(block) - kImageBase;
    Thunk(0x46a220, reinterpret_cast<void *>(&FakeCityRender));
    Thunk(0x50bf10, reinterpret_cast<void *>(&FakeMessagePanel)); // city_message_panel_layout
    Thunk(0x5296f0, reinterpret_cast<void *>(&FakeFillRect));
    Thunk(0x469e50, reinterpret_cast<void *>(&FakeScreenToGrid));
    Thunk(0x5a0f60, reinterpret_cast<void *>(&FakeGridToScreen));
    Thunk(0x52b310, reinterpret_cast<void *>(&FakeGetDirection));
    Thunk(0x53c950, reinterpret_cast<void *>(&FakeScrollStep));
    Thunk(0x53cbd0, reinterpret_cast<void *>(&FakeViewClamp));
    Thunk(0x4456a0, reinterpret_cast<void *>(&FakeNothing));
    Thunk(0x44bb00, reinterpret_cast<void *>(&FakeDragStart));
    Thunk(0x44bb90, reinterpret_cast<void *>(&FakeDragDraw));
    Thunk(0x52b910, reinterpret_cast<void *>(&FakeModeGetSize));
    Thunk(0x476650, reinterpret_cast<void *>(&FakeChangeMode));
    Thunk(0x469820, reinterpret_cast<void *>(&FakeViewportInit));
    Thunk(0x5a1430, reinterpret_cast<void *>(&FakeInfoPlace));
    Thunk(0x5beb60, reinterpret_cast<void *>(&FakeNothing));   // city_frame_bars
    Thunk(0x53b3a0, reinterpret_cast<void *>(&FakeSidebarPanels)); // sidebar_draw_panels
    Thunk(0x5d3690, reinterpret_cast<void *>(&FakeFixedKind));
    Thunk(0x47f1b0, reinterpret_cast<void *>(&FakeBuildingGet));
    Thunk(0x5d74e0, reinterpret_cast<void *>(&FakeWindowDraw));  // storage window draw
    Thunk(0x5e0610, reinterpret_cast<void *>(&FakeWindowDraw));  // trade window draw
    Thunk(0x556e50, reinterpret_cast<void *>(&FakeWindowDraw));  // mill window draw
    Thunk(0x556a80, reinterpret_cast<void *>(&FakeMillForeground)); // mill window foreground
    Thunk(0x5dd810, reinterpret_cast<void *>(&FakePopup));
    Thunk(0x5a23a0, reinterpret_cast<void *>(&FakeInfoInput));
    Thunk(0x449c10, reinterpret_cast<void *>(&FakeButtons));
    Thunk(0x408170, reinterpret_cast<void *>(&FakeImageGetId));
    Thunk(0x5cd800, reinterpret_cast<void *>(&FakeImageSize));
    Thunk(0x413960, reinterpret_cast<void *>(&FakeImageDraw));
    Thunk(0x475b00, reinterpret_cast<void *>(&FakeLoopStep));
    Thunk(0x53a130, reinterpret_cast<void *>(&FakeSimDue));
    // The city's scroll_get_direction call at 0x538503 (returning to 0x538508), as the game makes it.
    using Fn = int(__cdecl *)();
    BYTE *call = reinterpret_cast<BYTE *>(0x538503 + g_imageShift);
    call[0] = 0xe8;
    *reinterpret_cast<DWORD *>(call + 1) =
        reinterpret_cast<DWORD>(Scroll<Fn>("scroll_get_direction")) - reinterpret_cast<DWORD>(call + 5);
    call[5] = 0xc3;
    // screen_draw's buttons_draw call for the info window (returning to 0x5378ef): a stub at 0x5378d0 pushes
    // (x 0, y 0x2a + 0x20 x 16 - 0x28, table 0, count 2) and calls the detour from 0x5378ea, as the game does.
    BYTE *stub = reinterpret_cast<BYTE *>(0x5378d0 + g_imageShift);
    const DWORD args[4] = {2, 0, 0x2a + 0x20 * 16 - 0x28, 0}; // pushed in this order: count, table, y, x
    for (int i = 0; i < 4; ++i) {
        stub[i * 5] = 0x68;
        *reinterpret_cast<DWORD *>(stub + i * 5 + 1) = args[i];
    }
    for (int i = 20; i < 0x1a; ++i) {
        stub[i] = 0x90;
    }
    using Buttons_fn = void(__cdecl *)(int, int, void *, int);
    BYTE *bcall = reinterpret_cast<BYTE *>(0x5378ea + g_imageShift);
    bcall[0] = 0xe8;
    *reinterpret_cast<DWORD *>(bcall + 1) =
        reinterpret_cast<DWORD>(Res<Buttons_fn>("buttons_draw")) - reinterpret_cast<DWORD>(bcall + 5);
    bcall[5] = 0x83; // add esp, 0x10
    bcall[6] = 0xc4;
    bcall[7] = 0x10;
    bcall[8] = 0xc3;
    // A 1920x1080 city screen with its back buffer.
    static WORD back[1920 * 1080];
    At<BYTE *>(aDrawBuffer) = reinterpret_cast<BYTE *>(back);
    At<int>(aPitch) = 1920 * 2;
    At<int>(aDdW) = 1920;
    At<int>(aDdH) = 1080;
    At<int>(aScreenW) = 1920;
    At<int>(aScreenH) = 1080;
    At<int>(aScreenMode) = 3;
    At<char>(aGameScreen) = 1;
    At<int>(aTileW) = 78;
    At<int>(aTileHalfH) = 20;
    At<int>(aOriginX) = 0;
    At<int>(aOriginY) = 40;
    At<int>(aCols) = 21;
    At<int>(aRows) = 53; // clip bottom 40 + 52 x 20 = 1080 (viewport_init)
    At<int>(aClipL) = 0;
    At<int>(aClipT) = 40;
    At<int>(aClipR) = 21 * 80 - 2;
    At<int>(aClipB) = 40 + 52 * 20;
    At<int>(aDesktopW) = 1920;
    At<int>(aDesktopH) = 1080;
    At<int>(aCursorX) = 900;
    At<int>(aCursorY) = 500;
    At<int>(aScrollSpeed) = 100;
    At<int>(aSidebarX) = 1678;
    At<int>(aSidebarX2) = 1798;
}

core_config Config1920()
{
    core_config c{};
    c.backend = CORE_BACKEND_EMULATED;
    c.resolution = CORE_RESOLUTION_FIXED;
    c.resolution_w = 1920;
    c.resolution_h = 1080;
    return c;
}

// ---------------------------------------------------------------------------------------------
// M4

using Render_fn = void(__cdecl *)(int, int, int);
using Convert_fn = int(__cdecl *)(int, int);
using Drag_fn = void(__fastcall *)(int *);

void M4RenderOk()
{
    emperor::scroll::SetRemainderForTest(5, 6);
    BYTE *back = At<BYTE *>(aDrawBuffer);
    Scroll<Render_fn>("city_render")(0, 0, 0);
    Check(g_render.calls == 1 && g_render.buffer != back, "while the game draws: draw_buffer is our city buffer");
    Check(g_render.a == 1678 + 80 && g_render.b == 1080 + 40 && g_render.c == 22 && g_render.d == 55,
          "while the game draws: clip one column / row pair larger, viewport +1 / +2");
    Check(At<BYTE *>(aDrawBuffer) == back && At<int>(aClipR) == 1678 && At<int>(aClipB) == 1080 &&
              At<int>(aCols) == 21 && At<int>(aRows) == 53,
          "afterwards: draw_buffer, clip and viewport are the game's again");
    int r = Scroll<Convert_fn>("screen_to_grid")(10, 10);
    Check(r == 1234 && g_convert.a == -5 && g_convert.b == 34 && g_convert.c == 22 && g_convert.d == 55,
          "screen_to_grid sees the origin moved by the remainder and a larger viewport");
    Check(At<int>(aOriginX) == 0 && At<int>(aOriginY) == 40 && At<int>(aCols) == 21 && At<int>(aRows) == 53,
          "afterwards: origin and viewport are the game's again");
    Check(emperor::scroll::Working(), "smooth scrolling still on");
}

void M4RenderFault()
{
    emperor::scroll::SetRemainderForTest(5, 6);
    BYTE *back = At<BYTE *>(aDrawBuffer);
    g_fault = "city_render";
    Scroll<Render_fn>("city_render")(0, 0, 0);
    Check(!emperor::scroll::Working(), "fault while the game draws into our buffer: smooth scrolling off");
    Check(At<BYTE *>(aDrawBuffer) == back && At<int>(aClipR) == 1678 && At<int>(aClipB) == 1080 &&
              At<int>(aCols) == 21 && At<int>(aRows) == 53,
          "draw_buffer, clip and viewport put back");
    Check(g_render.calls == 2 && g_render.buffer == back && g_render.a == 1678 && g_render.c == 21,
          "the city is drawn again the game's own way (its buffer, its clip)");
    int px, py;
    emperor::scroll::Remainder(&px, &py);
    Check(px == 0 && py == 0, "the view snaps to its whole tile (remainder 0)");
    Scroll<Convert_fn>("screen_to_grid")(10, 10);
    Check(g_convert.a == 0 && g_convert.b == 40 && g_convert.c == 21, "conversions pass through unchanged");
    Check(!emperor::scroll::SetSmooth(true), "cannot be switched on again from the panel");
    Check(LogContains("feature \"smooth scrolling (games/emperor/scroll)\" switched off") &&
              LogContains("6 changed value(s) put back"),
          "log: switched off, the 6 changed values put back");
}

void M4ConvertFault()
{
    emperor::scroll::SetRemainderForTest(5, 6);
    g_fault = "screen_to_grid";
    int r = Scroll<Convert_fn>("screen_to_grid")(10, 10);
    Check(!emperor::scroll::Working(), "fault in screen_to_grid with the origin moved: smooth scrolling off");
    Check(At<int>(aOriginX) == 0 && At<int>(aOriginY) == 40 && At<int>(aCols) == 21 && At<int>(aRows) == 53,
          "origin and viewport put back");
    Check(r == 1234 && g_convert.calls == 2 && g_convert.a == 0 && g_convert.b == 40,
          "converted again the game's own way, its result returned");
    Scroll<Render_fn>("city_render")(0, 0, 0);
    Check(g_render.calls == 1 && g_render.buffer == At<BYTE *>(aDrawBuffer), "city_render passes through");
}

void M4DirectionFault()
{
    using Fn = int(__cdecl *)();
    Fn cityCall = reinterpret_cast<Fn>(0x538503 + g_imageShift);
    At<int>(aTileW) = 0; // 2 px columns: every move changes view_x
    At<int>(aTileHalfH) = 1;
    At<int>(aCursorX) = 1919; // right edge
    emperor::scroll::SetSmooth(true);
    g_fault = "view_clamp";
    int before[3] = {}, r = 0;
    for (int i = 0; i < 400 && emperor::scroll::Working(); ++i) {
        At<int>(aFrame) = i + 1;
        At<int>(aScrolling) = 7; // as a sentinel
        before[0] = At<int>(aViewX);
        before[1] = At<int>(aViewY);
        before[2] = At<int>(aScrolling);
        r = cityCall();
        Sleep(2);
    }
    Check(!emperor::scroll::Working(), "fault in view_clamp during a smooth frame: smooth scrolling off");
    Check(At<int>(aViewX) == before[0] && At<int>(aViewY) == before[1] && At<int>(aScrolling) == before[2],
          "camera and view_scrolling put back to the frame's start");
    Check(r == 3, "that call returns the game's own direction");
    Check(cityCall() == 3, "afterwards the game's own scrolling");
}

void M4DragFault()
{
    int drag[8] = {0, 0, 300, 400};
    using Fn = void(__fastcall *)(int *);
    emperor::scroll::SetRemainderForTest(0, 0);
    Scroll<Fn>("drag_select_start")(drag);
    emperor::scroll::SetRemainderForTest(7, 4);
    Scroll<Drag_fn>("drag_select_draw")(drag);
    Check(g_drag.a == 293 && g_drag.b == 396 && drag[2] == 300 && drag[3] == 400,
          "drag: start corner moved by the remainder for the call only");
    g_fault = "drag_draw";
    Scroll<Drag_fn>("drag_select_draw")(drag);
    Check(!emperor::scroll::Working() && drag[2] == 300 && drag[3] == 400, "fault: drag start corner put back");
    Check(g_drag.calls == 3 && g_drag.a == 300 && g_drag.b == 400, "drawn again the game's own way");
}

// ---------------------------------------------------------------------------------------------
// R1

void R1SwitchFault()
{
    core_config c = Config1920();
    emperor::resolution::InstalledForTest(&c);
    At<int>(aScreenW) = 1024;
    At<int>(aScreenH) = 768;
    g_fault = "change_mode";
    emperor::resolution::OnLoop(); // city screen: switch to 1920x1080; screen_change_mode faults after the size
    Check(At<int>(aScreenW) == 1920 && LogContains("feature \"resolution switching"),
          "fault in the mode change: switching off (the size was already 1920)");
    Check(LogContains("back to the game's own 1024x768 on the next loop"), "the game's own size is due");
    Sleep(520);
    emperor::resolution::OnLoop(); // the game's own mode change: mode 3 is 1024x768 again
    emperor::resolution::OnLoop();
    Check(At<int>(aScreenW) == 1024 && At<int>(aScreenH) == 768 && LogContains("the game runs at its own 1024x768"),
          "next loops: the game's own 1024x768");
    Check(!emperor::resolution::SettingsChanged(&c), "a new resolution from the panel is refused");
}

void R1ModeFault()
{
    core_config c = Config1920();
    emperor::resolution::InstalledForTest(&c);
    emperor::resolution::OnLoop(); // city screen -> 1920x1080 (NativeSize fine)
    Check(At<int>(aScreenW) == 1920 && At<int>(aScreenH) == 1080, "mode 3 = 1920x1080");
    core::display::g_nativeFaults = true; // the next size cannot be worked out
    using Fn = void(__cdecl *)(int);
    Res<Fn>("screen_mode_get_size")(3);
    core::display::g_nativeFaults = false;
    Check(At<int>(aScreenW) == 1024 && At<int>(aScreenH) == 768,
          "fault in our mode size: the game's own 1024x768 put back for this mode set");
    Check(LogContains("2 changed value(s) put back") || LogContains("0 changed value(s) put back"),
          "log: the guard reports what it put back");
    Res<Fn>("screen_mode_get_size")(3);
    Check(At<int>(aScreenW) == 1024, "from then on mode 3 is the game's own 1024x768");
}

// City message panel (0x50bf10) and fill_rect, as the tests see them: the clip the panel is laid out in, the clip
// the city is drawn with inside it (city_draw_view -> our city_render), and the fills that reach the game.
int g_panelClipR = 0, g_panelClipB = 0, g_fills = 0;
using Render_fn2 = void(__cdecl *)(int, int, int);
Render_fn2 g_cityRenderDetour = nullptr;

void __cdecl FakeMessagePanel()
{
    g_panelClipR = At<int>(aClipR);
    g_panelClipB = At<int>(aClipB);
    if (g_cityRenderDetour) {
        g_cityRenderDetour(0, 0, 0);
    }
}

void __cdecl FakeFillRect(int, int, int, int, int)
{
    ++g_fills;
}

// R2b.1-5 / -6: the sidebar against the right edge; the city shown past its clip (which stays on the tile grid) to
// the sidebar and the screen's bottom, through our city buffer or (bottom only) straight into the back buffer.
void FillScreen(WORD v); // below

void R2b1Extent()
{
    using Fn = void(__cdecl *)();
    core_config c = Config1920();
    emperor::resolution::InstalledForTest(&c);
    BYTE *back = At<BYTE *>(aDrawBuffer);
    At<int>(aScreenH) = 1066; // 135% of 1440: not 40 + a multiple of 20
    At<int>(aDdH) = 1066;     // the test's back buffer has 1080 rows: rows 1066.. stand for the spare rows
    At<int>(aClipB) = 1440; // still the last mode's clip: the game sets the sidebar before the viewport
    core::ddraw::SetCanaryAllowance(0);
    Res<Fn>("ui_set_sidebar_position")();
    Check(At<int>(aSidebarX) == 1694 && At<int>(aSidebarX2) == 1814, "the sidebar against the right edge: 1920 - 226");
    Check(core::ddraw::CanaryAllowance() == 0,
          "the sidebar does not set the spare rows from the last mode's clip (1440 at 1066 high)");
    Res<Fn>("city_viewport_large")();
    Check(At<int>(aCols) == 21 && At<int>(aClipR) == 1678 && At<int>(aClipB) == 1060,
          "the game's clip stays on the tile grid: (1678, 1060), a gap of 16 px and 6 rows");
    Check(core::ddraw::CanaryAllowance() == 14, "spare rows the city may draw into: 1080 - 1066 = 14");
    FillScreen(0x0101);
    g_marks[0] = {1690, 1063, 0xaaaa}; // in the gap, in the bottom rows: shown
    g_marks[1] = {100, 100, 0xdddd};   // the city: shown
    g_marks[2] = {1700, 500, 0xbbbb};  // under the sidebar: not shown
    g_marks[3] = {500, 1070, 0xcccc};  // below the screen: not shown
    g_markCount = 4;
    emperor::scroll::SetRemainderForTest(0, 0);
    Scroll<Render_fn>("city_render")(0, 0, 0);
    Check(g_render.buffer != back && g_render.a == 1678 + 80 && g_render.b == 1060 + 40 && g_render.c == 22 &&
              g_render.d == 54,
          "gap: drawn into our buffer, the clip one column / row pair larger (still on the grid)");
    Check(Px(1690, 1063) == 0xaaaa && Px(100, 100) == 0xdddd, "shown: the city, the gap and the bottom rows");
    Check(Px(1700, 500) == 0x0101 && Px(500, 1070) == 0x0101, "not shown: under the sidebar, below the screen");
    Check(At<BYTE *>(aDrawBuffer) == back && At<int>(aClipR) == 1678 && At<int>(aClipB) == 1060 &&
              At<int>(aCols) == 21 && At<int>(aRows) == 52,
          "afterwards: draw_buffer, clip and viewport are the game's again");
    // No gap (1904 = 224 + 21 x 80), bottom only: straight into the back buffer.
    At<int>(aScreenW) = 1904;
    Res<Fn>("ui_set_sidebar_position")();
    Res<Fn>("city_viewport_large")();
    Check(At<int>(aSidebarX) == 1678 && At<int>(aClipR) == 1678, "1904 wide: the sidebar right at the clip, no gap");
    FillScreen(0x0101);
    Scroll<Render_fn>("city_render")(0, 0, 0);
    Check(g_render.buffer == back && g_render.b == 1080 && g_render.d == 53 && g_render.a == 1678,
          "bottom only: the back buffer itself, the clip down to the next half row (1080), viewport +1 half row");
    Check(Px(1690, 1063) == 0xaaaa && Px(500, 1070) == 0xcccc,
          "the bottom rows drawn (and the row below the screen lands in the spare rows)");
    Check(At<int>(aClipB) == 1060 && At<int>(aRows) == 52, "afterwards the game's clip and viewport again");
    // A whole number of rows: nothing below the clip to show, no spare rows used.
    At<int>(aScreenH) = 1080;
    At<int>(aDdH) = 1080;
    Res<Fn>("city_viewport_large")();
    Check(At<int>(aClipB) == 1080 && core::ddraw::CanaryAllowance() == 0, "1080 high: the clip reaches the bottom");
    g_markCount = 0;
    Check(LogContains("scroll: the city is shown past its clip to (1694,1066)"), "log: the extent, once");
}

// The city message panel laid out in the extent, the city inside it still drawn on the grid; city_draw_background's
// bottom strip not cleared while the city reaches the screen's bottom (only that call; other fills pass).
void R2b1MessagePanel()
{
    using Fn = void(__cdecl *)();
    core_config c = Config1920();
    emperor::resolution::InstalledForTest(&c);
    At<int>(aScreenH) = 1066;
    At<int>(aDdH) = 1066;
    Res<Fn>("ui_set_sidebar_position")();
    Res<Fn>("city_viewport_large")();
    g_cityRenderDetour = Scroll<Render_fn2>("city_render");
    emperor::scroll::SetRemainderForTest(0, 0);
    Res<Fn>("city_message_panel_layout")();
    Check(g_panelClipR == 1694 && g_panelClipB == 1066,
          "the panel is laid out in the city as shown: right to the sidebar (1694), bottom of the screen (1066)");
    Check(g_render.a == 1678 + 80 && g_render.b == 1060 + 40 && g_render.c == 22,
          "the city drawn inside it still on the grid clip (worked out from the viewport)");
    Check(At<int>(aClipR) == 1678 && At<int>(aClipB) == 1060, "afterwards the game's clip again");
    g_cityRenderDetour = nullptr;
    // city_draw_background's call at 0x5b5a80 (returning to 0x5b5a85): a stub at 0x5b5a60 pushes
    // (x 0, y 1060, w 1920, h 6, colour 0) and calls the detour, as the game does.
    BYTE *stub = reinterpret_cast<BYTE *>(0x5b5a60 + g_imageShift);
    const DWORD args[5] = {0, 6, 1920, 1060, 0}; // pushed in this order: colour, h, w, y, x
    for (int i = 0; i < 5; ++i) {
        stub[i * 5] = 0x68;
        *reinterpret_cast<DWORD *>(stub + i * 5 + 1) = args[i];
    }
    for (int i = 25; i < 0x20; ++i) {
        stub[i] = 0x90;
    }
    using Fill_fn = void(__cdecl *)(int, int, int, int, int);
    BYTE *call = reinterpret_cast<BYTE *>(0x5b5a80 + g_imageShift);
    call[0] = 0xe8;
    *reinterpret_cast<DWORD *>(call + 1) =
        reinterpret_cast<DWORD>(Res<Fill_fn>("fill_rect")) - reinterpret_cast<DWORD>(call + 5);
    call[5] = 0x83; // add esp, 0x14
    call[6] = 0xc4;
    call[7] = 0x14;
    call[8] = 0xc3;
    g_fills = 0;
    reinterpret_cast<Fn>(stub)();
    Check(g_fills == 0, "city_draw_background's bottom strip: not cleared while the city reaches the bottom");
    Res<Fill_fn>("fill_rect")(10, 10, 20, 20, 0);
    Check(g_fills == 1, "fill_rect from anywhere else: as always");
}

void R1LayoutFault()
{
    using Fn = void(__cdecl *)();
    g_fault = "viewport_init";
    Res<Fn>("city_viewport_large")();
    Check(g_viewport.calls == 2 && g_viewport.a == 10 && g_viewport.b == 37,
          "fault in viewport_init: set again with the game's 1024 values (10 x 37)");
    Check(LogContains("feature \"city layout") && LogContains("feature \"resolution switching"),
          "layout and switching off");
    Check(LogContains("back to the game's own 1024x768 on the next loop"), "the game's own size is due");
    Res<Fn>("ui_set_sidebar_position")();
    Check(At<int>(aSidebarX) == 798 && At<int>(aSidebarX2) == 918, "sidebar at the 1024 layout's 798 meanwhile");
}

void R1Info()
{
    using Fn = void(__cdecl *)();
    using emperor::resolution::InfoMode;
    Fn place = Res<Fn>("info_window_place");
    At<int>(aMouseX) = 300;
    At<int>(aMouseY) = 200;
    place();
    Check(At<int>(aInfoX) == 0xbc + (1678 - 798) / 2 && At<int>(aInfoY) == 1072 - 320,
          "original: the game's rule, moved with the city area's centre (628, 752)");
    emperor::resolution::SetInfoMode(InfoMode::Centered);
    place();
    Check(At<int>(aInfoX) == (1678 - 464) / 2 && At<int>(aInfoY) == 40 + (1080 - 40 - 320) / 2,
          "centered: in the middle of the city area, which reaches the bottom (607, 400)");
    emperor::resolution::SetInfoMode(InfoMode::Cursor);
    place();
    Check(At<int>(aInfoX) == 324 && At<int>(aInfoY) == 40, "cursor: right of the cursor, not above the top bar");
    At<int>(aMouseX) = 1500;
    At<int>(aMouseY) = 1060;
    place();
    Check(At<int>(aInfoX) == 1500 - 24 - 464 && At<int>(aInfoY) == 1080 - 320,
          "cursor near the sidebar and the bottom: left of it, not past the bottom");
    // Fault inside our part (after the position was saved): the screen variables' page is made unreadable.
    DWORD old;
    VirtualProtect(reinterpret_cast<void *>(0x10de000 + g_imageShift), 0x1000, PAGE_NOACCESS, &old);
    place();
    VirtualProtect(reinterpret_cast<void *>(0x10de000 + g_imageShift), 0x1000, PAGE_EXECUTE_READWRITE, &old);
    Check(At<int>(aInfoX) == 8 && At<int>(aInfoY) == 0x20, "fault: the game's own position put back (8, 32)");
    place();
    Check(At<int>(aInfoX) == 8 && At<int>(aInfoY) == 0x20 && LogContains("feature \"info window position"),
          "afterwards the game's own position");
    Check(LogContains("feature \"info window position") && !LogContains("feature \"resolution switching"),
          "only the info window part is off (the resolution stays)");
}

void R1InfoPositions()
{
    using emperor::resolution::InfoArea;
    using emperor::resolution::InfoMode;
    using emperor::resolution::InfoPosition;
    InfoArea a{0, 40, 1678, 1072};
    POINT p = InfoPosition(InfoMode::Cursor, a, 464, 320, 10, 600);
    Check(p.x == 34 && p.y == 440, "cursor at the left edge: right of it");
    p = InfoPosition(InfoMode::Cursor, a, 464, 320, 1670, 45);
    Check(p.x == 1670 - 24 - 464 && p.y == 40, "cursor at the top right: left of it, below the top bar");
    p = InfoPosition(InfoMode::Centered, InfoArea{0, 40, 400, 300}, 464, 320, 0, 0);
    Check(p.x == 0 && p.y == 40, "larger than the area: at its top left");
    for (int mx = 0; mx < 1678; mx += 97) {
        for (int my = 40; my < 1072; my += 53) {
            p = InfoPosition(InfoMode::Cursor, a, 464, 512, mx, my);
            if (p.x < 0 || p.y < 40 || p.x + 464 > 1678 || p.y + 512 > 1072) {
                Check(false, "cursor: inside the city area for every cursor position");
                return;
            }
        }
    }
    Check(true, "cursor: inside the city area for every cursor position");
}

void R1BarsAndFiller()
{
    using Fn = void(__cdecl *)();
    Res<Fn>("city_frame_bars")();
    int drawn = g_draws.calls;
    Check(drawn > 2, "top bar drawn (tiled)");
    Check(g_bottomEdgeDraws == 0, "no bottom edge wider than 1024 (the city reaches the screen's bottom)");
    g_fault = "image_draw";
    Res<Fn>("city_frame_bars")();
    int before = g_draws.calls;
    Res<Fn>("city_frame_bars")();
    Check(g_draws.calls == before && LogContains("feature \"top bar ("),
          "fault: top bar no longer drawn");
    // The filler (no DATA pack next to the test: drawn with image_draw, as R1 did).
    Res<Fn>("sidebar_draw_panels")();
    Check(g_draws.calls > before, "filler drawn");
    g_fault = "image_draw";
    Res<Fn>("sidebar_draw_panels")();
    before = g_draws.calls;
    Res<Fn>("sidebar_draw_panels")();
    Check(g_draws.calls == before && LogContains("feature \"sidebar filler"), "fault: filler no longer drawn");
    Check(!LogContains("feature \"resolution switching"), "the resolution stays");
}

void R1ClampFault()
{
    using Fn = int(__cdecl *)(int, int, int, int, int, int);
    Fn draw = Res<Fn>("image_draw");
    At<int>(aArtOX) = 0;
    At<int>(aArtOY) = 0;
    At<int>(aArtW) = 1920;
    At<int>(aArtH) = 1040;
    draw(kArtGroup, 0, 40, 0, 0, 0);
    Check(At<int>(aArtW) == 848 && At<int>(aArtH) == 746 && g_draws.calls == 1,
          "the China map's copy clamped to the picture (848 x 746), then drawn");
    draw(5, 0, 0, 0, 0, 0);
    Check(g_draws.calls == 2, "other images drawn as they are");
    At<int>(aArtW) = 1920;
    At<int>(aArtH) = 1040;
    g_fault = "image_size";
    draw(kArtGroup, 0, 40, 0, 0, 0);
    Check(g_draws.calls == 2 && At<int>(aArtW) == 1920 && At<int>(aArtH) == 1040,
          "fault while clamping: the picture is not drawn, the rectangle untouched");
    Check(LogContains("feature \"full-screen picture clamp") && LogContains("feature \"resolution switching") &&
              LogContains("back to the game's own 1024x768 on the next loop"),
          "clamp and switching off; the game's own size is due");
    draw(kArtGroup, 0, 40, 0, 0, 0);
    Check(g_draws.calls == 2, "not drawn while the screen is still larger than 1024x768");
    At<int>(aScreenW) = 1024;
    At<int>(aScreenH) = 768;
    draw(kArtGroup, 0, 40, 0, 0, 0);
    Check(g_draws.calls == 3, "drawn again at the game's own 1024x768");
}

// ---------------------------------------------------------------------------------------------
// P2

void P2DrawOnly()
{
    using Fn = int(__cdecl *)();
    Fn loop = Main<Fn>("game_loop_step"), simDue = Main<Fn>("sim_tick_due");
    core::ui::g_open = true; // the panel asks to keep a new resolution
    int r = loop();
    Check(r == 5 && g_loop.calls == 1 && g_simDueInside == 0, "draw-only frame: drawn, sim_tick_due says no");
    Check(simDue() == 1, "afterwards sim_tick_due is the game's again");
    Sleep(20);
    g_fault = "loop_step";
    r = loop();
    Check(r == 0 && g_loop.calls == 2 && g_simDueInside == 0, "fault in a draw-only frame: abandoned");
    Check(simDue() == 1, "g_drawOnly put back: the simulation is not held back afterwards");
    Sleep(20);
    loop();
    Check(g_loop.calls == 2 && LogContains("feature \"draw-only frames"),
          "no more draw-only frames (the game stays paused while the panel asks)");
    core::ui::g_open = false;
    r = loop();
    Check(r == 5 && g_loop.calls == 3, "with the panel closed the game's frames run as always");
}

// ---------------------------------------------------------------------------------------------
// R2b: windows the game always opens at (0, 0x2a), shown elsewhere

using WindowDraw_fn = void(__fastcall *)(void *, void *, int, int, int, int, int);
using Void_fn = void(__cdecl *)();

void FillScreen(WORD v)
{
    for (int y = 0; y < 1080; ++y) {
        for (int x = 0; x < 1920; ++x) {
            Px(x, y) = v;
        }
    }
}

void R2bFixedWindow()
{
    using emperor::resolution::InfoMode;
    BYTE *back = At<BYTE *>(aDrawBuffer);
    WindowDraw_fn draw = Res<WindowDraw_fn>("storage_window_draw");
    Void_fn input = Res<Void_fn>("info_window_input");
    Void_fn buttons = reinterpret_cast<Void_fn>(0x5378d0 + g_imageShift);
    const int dx = (1678 - 480) / 2, dy = 40 + (1040 - 512) / 2 - 0x2a; // centred: (599, 304) on screen
    At<int>(aMouseX) = 300;
    At<int>(aMouseY) = 200;
    g_fixedKind = true;
    emperor::resolution::SetInfoMode(InfoMode::Centered);
    Res<Void_fn>("info_window_place")();
    At<char>(aGameScreen) = 9;
    Check(At<int>(aInfoX) == 0 && At<int>(aInfoY) == 0x2a, "fixed kind: the game keeps its own (0, 42)");
    Check(g_buildingList == reinterpret_cast<void *>(0x8c7634 + g_imageShift),
          "building_get called as __thiscall on the building list 0x8c7634 (as the game calls it)");
    FillScreen(0x0101);
    draw(nullptr, nullptr, 0, 0x2a, 0x1e, 0x20, 0);
    Check(g_window.buffer != back && g_window.mx == 300 - dx && g_window.my == 200 - dy,
          "while it draws: a copy of the screen, the mouse in the game's place of the window");
    Check(At<BYTE *>(aDrawBuffer) == back && At<int>(aMouseX) == 300 && At<int>(aMouseY) == 200,
          "afterwards: draw_buffer and the mouse are the game's again");
    Check(Px(dx + 5, 0x2a + dy + 5) == 0xabcd, "the window is shown centred (its frame mark at (604, 309))");
    Check(Px(dx + 5, (0x20 - 4) * 16 + 2 + dy) == 0x1234,
          "the part the game draws at an absolute y (employee panel) moves with it");
    Check(Px(5, 0x2a + 5) == 0x0101 && Px(5, (0x20 - 4) * 16 + 2) == 0x0101,
          "nothing of the window where the game has it (the city shows there)");
    Check(Px(1000, 900) == 0x7777, "anything drawn outside the window stays where it was drawn");
    input();
    Check(g_input.mx == 300 - dx && g_input.my == 200 - dy && At<int>(aMouseX) == 300,
          "input: the mouse in the game's place of the window (clicks hit what is shown)");
    At<int>(aMouseX) = 10;
    At<int>(aMouseY) = 60;
    input();
    Check(g_input.mx == 10 - dx && (g_input.mx < 0 || g_input.my < 0x2a),
          "a click at the window's hidden place lands outside the game's window");
    At<int>(aMouseX) = 300;
    At<int>(aMouseY) = 200;
    buttons();
    Check(g_buttonsX == dx && g_buttonsY == 0x2a + 0x20 * 16 - 0x28 + dy, "help / OK buttons shown with the window");
    At<char>(aTradePopup) = 1;
    g_callPopup = true;
    Res<WindowDraw_fn>("trade_window_draw")(nullptr, nullptr, 0, 0x2a, 0x1e, 0x20, 0);
    Check(g_popup.calls == 1 && g_popup.buffer == back && g_popup.mx == 300 && Px(1100, 950) == 0x5555,
          "trade popup: drawn on the real screen with the real mouse");
    Check(Px(dx + 5, 0x2a + dy + 5) == 0xabcd, "the window under the popup is still shown moved");
    FillScreen(0x0101);
    Res<WindowDraw_fn>("mill_window_draw")(nullptr, nullptr, 0, 0x2a, 0x1e, 0x20, 0);
    Check(Px(dx + 5, 0x2a + dy + 5) == 0xabcd && Px(5, 0x2a + 5) == 0x0101, "the mill window is shown moved too");
    using Foreground_fn = void(__fastcall *)(void *, void *, int, int, int, int);
    Res<Foreground_fn>("mill_window_foreground")(nullptr, nullptr, 0, 0x2a, 0x1e, 0x20);
    Check(g_foreground.calls == 1 && g_foreground.buffer != back && g_foregroundX == 0 && g_foregroundY == 0x2a &&
              At<BYTE *>(aDrawBuffer) == back,
          "mill foreground: drawn into the copy with the game's own x, y; draw_buffer back afterwards");
    Check(Px(dx + 30, 0x2a + dy + 100) == 0xbeef && Px(30, 0x2a + 100) == 0x0101 &&
              Px(dx + 5, 0x2a + dy + 5) == 0xabcd,
          "mill foreground: its food list is shown with the moved window, nothing at the game's place");
    input();
    Check(g_input.mx == 300 && g_input.my == 200, "popup open: the input is not moved (the popup takes it)");
    At<char>(aTradePopup) = 0;
    g_callPopup = false;
    // A fault while the window draws: put back, and from then on the window is where the game has it.
    FillScreen(0x0101);
    g_fault = "window_draw";
    draw(nullptr, nullptr, 0, 0x2a, 0x1e, 0x20, 0);
    Check(At<BYTE *>(aDrawBuffer) == back && At<int>(aMouseX) == 300 && At<int>(aMouseY) == 200,
          "fault while it draws: draw_buffer and the mouse put back");
    Check(Px(5, 0x2a + 5) == 0xabcd && g_window.buffer == back, "drawn again where the game has it");
    input();
    buttons();
    Check(g_input.mx == 300 && g_buttonsX == 0, "input and buttons not moved any more");
    Check(LogContains("feature \"info window position"), "log: the info window part switched off");
}

void R2bUnknownClass()
{
    g_fixedKind = true;
    g_unknownClass = true;
    emperor::resolution::SetInfoMode(emperor::resolution::InfoMode::Centered);
    Res<Void_fn>("info_window_place")();
    At<char>(aGameScreen) = 9;
    At<int>(aMouseX) = 300;
    At<int>(aMouseY) = 200;
    Res<Void_fn>("info_window_input")();
    reinterpret_cast<Void_fn>(0x5378d0 + g_imageShift)();
    Check(At<int>(aInfoX) == 0 && At<int>(aInfoY) == 0x2a && g_input.mx == 300 && g_buttonsX == 0,
          "a fixed kind whose window draw is not one of ours stays where the game has it (nothing moved)");
}

void R2bFixedOriginal()
{
    BYTE *back = At<BYTE *>(aDrawBuffer);
    g_fixedKind = true;
    Res<Void_fn>("info_window_place")(); // original mode
    At<char>(aGameScreen) = 9;
    At<int>(aMouseX) = 300;
    At<int>(aMouseY) = 200;
    Res<WindowDraw_fn>("storage_window_draw")(nullptr, nullptr, 0, 0x2a, 0x1e, 0x20, 0);
    Res<Void_fn>("info_window_input")();
    Check(At<int>(aInfoX) == 0 && At<int>(aInfoY) == 0x2a && g_window.buffer == back && g_input.mx == 300,
          "original: fixed kinds stay at the game's (0, 42), nothing moved");
    g_fixedKind = false;
    emperor::resolution::SetInfoMode(emperor::resolution::InfoMode::Cursor);
    Res<Void_fn>("info_window_place")();
    Res<WindowDraw_fn>("storage_window_draw")(nullptr, nullptr, 0, 0x2a, 0x1e, 0x20, 0);
    Check(At<int>(aInfoX) == 324 && g_window.buffer == back, "other kinds: the game's position is set, no copy");
}

// The sidebar backdrop from the player's own pack: the sidebar's left edge (dark, light) continues on every row below
// the sidebar's own panels, the strip right of it has its edge on every row, and no band at the bottom.
int Lum(WORD p)
{
    int r = (p >> 11) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
    return (r * 299 + g * 587 + b * 114) / 1000;
}

void WriteBmp(const char *file, int x0, int y0, int w, int h)
{
    FILE *f = std::fopen(file, "wb");
    if (!f) {
        return;
    }
    const int row = (w * 3 + 3) & ~3;
    BYTE hdr[54] = {'B', 'M'};
    *reinterpret_cast<DWORD *>(hdr + 2) = 54 + row * h;
    *reinterpret_cast<DWORD *>(hdr + 10) = 54;
    *reinterpret_cast<DWORD *>(hdr + 14) = 40;
    *reinterpret_cast<LONG *>(hdr + 18) = w;
    *reinterpret_cast<LONG *>(hdr + 22) = h;
    *reinterpret_cast<WORD *>(hdr + 26) = 1;
    *reinterpret_cast<WORD *>(hdr + 28) = 24;
    std::fwrite(hdr, 1, 54, f);
    std::vector<BYTE> line(row, 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            WORD p = Px(x0 + x, y0 + y);
            line[x * 3] = static_cast<BYTE>((p & 31) * 255 / 31);
            line[x * 3 + 1] = static_cast<BYTE>(((p >> 5) & 63) * 255 / 63);
            line[x * 3 + 2] = static_cast<BYTE>((p >> 11) * 255 / 31);
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
}

void R2bSidebarEdges()
{
    const char *pack = std::getenv("TEST_RESTORE_PACK");
    if (!pack || !*pack) {
        std::printf("  (skipped: TEST_RESTORE_PACK does not name China_Interface.sg3)\n");
        return;
    }
    wchar_t path[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, pack, -1, path, MAX_PATH);
    core::assets::Sg3Pack sg;
    Check(sg.Open(path) && sg.Decode565(1223, 0xf81f, &g_upper) && sg.Decode565(1224, 0xf81f, &g_lower),
          "the sidebar's own panels read from the pack (entries 1223, 1224)");
    emperor::resolution::SetInterfacePackForTest(path);
    FillScreen(0x0000);
    using Fn = void(__cdecl *)();
    Res<Fn>("sidebar_draw_panels")();
    WriteBmp("test_restore_sidebar.bmp", 1600, 680, 320, 400);
    const int left = 1678, right = 1678 + 226;
    int badLeft = 0, firstLeft = -1, badRight = 0, firstRight = -1;
    for (int y = 60; y < 1080; ++y) {
        if (!(Lum(Px(left, y)) < 50 && Lum(Px(left + 1, y)) > 150)) {
            ++badLeft;
            if (firstLeft < 0) firstLeft = y;
        }
        if (!(Lum(Px(right, y)) < 150 && Lum(Px(right + 1, y)) < 150 && Lum(Px(right + 2, y)) < 150)) {
            ++badRight; // a light line next to the sidebar (the backdrop's edge columns)
            if (firstRight < 0) firstRight = y;
        }
    }
    char what[160];
    std::snprintf(what, sizeof(what), "left edge (dark, light) on every row 60..1079 (%d rows off, first %d)", badLeft,
                  firstLeft);
    Check(badLeft == 0, what);
    std::snprintf(what, sizeof(what),
                  "right strip: plain leather, no light edge line next to the sidebar, rows 60..1079 (%d rows off, "
                  "first %d)",
                  badRight, firstRight);
    Check(badRight == 0, what);
    int blue = 0;
    for (int y = 768; y < 1080; ++y) {
        for (int x = left + 3; x < 1920; ++x) {
            WORD p = Px(x, y);
            int r = (p >> 11) * 255 / 31, b = (p & 31) * 255 / 31;
            blue += b > r + 40;
        }
    }
    Check(blue == 0, "no blue band below the sidebar's panels or at the bottom");
}

// R2b.1-7 (games/emperor/ui_fixes): the Mill's list top in drawing and clicks moves by the same amount; the build
// submenu's names and costs move down; the messages screen's second-row button (frame and text) is centred while
// there is no third button and the game's own again with it.
struct TextCall {
    int calls, x, y, width;
} g_text;

int __cdecl FakeTextCentred(int, int, int x, int y, int width, int, int)
{
    ++g_text.calls;
    g_text.x = x;
    g_text.y = y;
    g_text.width = width;
    return 0;
}

void Plant(DWORD address, std::initializer_list<BYTE> bytes)
{
    BYTE *p = const_cast<BYTE *>(&At<BYTE>(address));
    for (BYTE b : bytes) {
        *p++ = b;
    }
}

void R2b17UiFixes()
{
    Plant(0x556b0b, {0x8d, 0x59, 0x73, 0x89, 0x5c, 0x24, 0x28, 0x0f});
    Plant(0x556983, {0x8d, 0x6a, 0x73, 0x8b, 0x54, 0x24, 0x20, 0xc1});
    Plant(0x857ef4, {0x9c, 0xff, 0x20, 0x00, 0xc8, 0x00, 0x3e, 0x00});
    Plant(0x5b7136, {0x8d, 0x4c, 0x06, 0x71, 0x8b, 0x44, 0x24, 0x4c});
    Plant(0x5b71cf, {0x8d, 0x44, 0x16, 0x71, 0x68, 0xb0, 0x00, 0x00});
    Plant(0x5b7242, {0x8d, 0x54, 0x0e, 0x71, 0x68, 0xb0, 0x00, 0x00});
    Plant(0x5b7375, {0x8d, 0x4c, 0x06, 0x71, 0x68, 0x00, 0x01, 0x00});
    Plant(0x5b72e2, {0x8d, 0x54, 0x0e, 0x72, 0x53, 0x8d, 0x47, 0xbe});
    Plant(0x5b73fb, {0x8d, 0x4c, 0x06, 0x72, 0x53, 0x8d, 0x57, 0xdc});
    Plant(0x5496b6, {0x6a, 0x07, 0x6a, 0x3f, 0xe8, 0xd1, 0xf1, 0xfd, 0xff});
    Thunk(0x528890, reinterpret_cast<void *>(&FakeTextCentred)); // lang_text_draw_centered
    At<int>(0x10bc358) = 0;
    At<int>(0x1192a58) = 34; // the messages dialog's width in 16-pixel units
    Check(emperor::ui_fixes::Attach(), "ui_fixes: attached");
    const int top = 0x73 + emperor::ui_fixes::kMillListShift;
    Check(At<BYTE>(0x556b0d) == top && At<BYTE>(0x556985) == top,
          "Mill list: the same new top for drawing and clicks");
    Check(At<BYTE>(0x556b0b) == 0x8d && At<BYTE>(0x556b0e) == 0x89 && At<BYTE>(0x556983) == 0x8d &&
              At<BYTE>(0x556986) == 0x8b,
          "Mill list: nothing else in the instructions changed");
    const int d = emperor::ui_fixes::kSubmenuTextShift;
    Check(At<BYTE>(0x5b7139) == 0x71 + d && At<BYTE>(0x5b71d2) == 0x71 + d && At<BYTE>(0x5b7245) == 0x71 + d &&
              At<BYTE>(0x5b7378) == 0x71 + d && At<BYTE>(0x5b72e5) == 0x72 + d && At<BYTE>(0x5b73fe) == 0x72 + d,
          "build submenu: names and costs moved down by the same amount");
    Check(At<BYTE>(0x5b7138) == 0x06 && At<BYTE>(0x5b713a) == 0x8b && At<BYTE>(0x5b72e6) == 0x53,
          "build submenu: nothing else in the instructions changed");
    Check(At<short>(0x857ef4) == -100, "messages: the table untouched until the first pass");
    Check(At<BYTE>(0x5496ba) == 0xe8 && At<BYTE>(0x5496b9) == 0x3f, "messages: still a call after push 0x3f");
    using Text_fn = int(__cdecl *)(int, int, int, int, int, int, int);
    BYTE *call = const_cast<BYTE *>(&At<BYTE>(0x5496ba));
    Text_fn target = reinterpret_cast<Text_fn>(call + 5 + *reinterpret_cast<int *>(call + 1));
    target(0x3f, 7, 100 + 0x40, 500, 0xf0, 0, 0);
    Check(g_text.calls == 1 && g_text.x == 100 + 0x40 && g_text.y == 500 && g_text.width == 0xf0,
          "messages text before the first pass: the game's own place");
    emperor::ui_fixes::OnLoop();
    Check(At<short>(0x857ef4) == 0 && At<short>(0x857ef8) == 300 && At<short>(0x857ef6) == 32 &&
              At<short>(0x857efa) == 62,
          "messages, no third button: (0, 32)-(300, 62), under Delete opened messages");
    target(0x3f, 7, 100 + 0x40, 500, 0xf0, 0, 0);
    // The frame: base + (0 .. 300) with base = window x + 34 * 8 - 0x8c (window x = 100); the game centres in it.
    Check(g_text.calls == 2 && g_text.x == 100 + 34 * 8 - 0x8c && g_text.width == 300,
          "messages text, no third button: centred in its frame (x = frame left, width = frame width)");
    At<int>(0x10bc358) = 1;
    emperor::ui_fixes::OnLoop();
    Check(At<short>(0x857ef4) == -100 && At<short>(0x857ef8) == 200, "messages, third button: the game's own place");
    target(0x3f, 7, 100 + 0x40, 500, 0xf0, 0, 0);
    Check(g_text.x == 100 + 0x40 && g_text.width == 0xf0, "messages text, third button: the game's own place");
}


// R2c (games/emperor/china_map): the map drawn into our layer, shown scaled or at its own size over the backdrop in the
// map area; the top bar's piece drawn afterwards on the screen; the mouse converted only inside the map's input.
template <typename F> F MapDetour(const char *name)
{
    int n = 0;
    const emperor::Detour *d = emperor::china_map::Detours(&n);
    return DetourOf<F>(d, n, name);
}

std::vector<WORD> g_mapScreen;

void MapScreen(int w, int h, int sidebar)
{
    g_mapScreen.assign(static_cast<size_t>(w) * h, kSentinel16);
    At<BYTE *>(aDrawBuffer) = reinterpret_cast<BYTE *>(g_mapScreen.data());
    At<int>(aPitch) = w * 2;
    At<int>(aDdW) = w;
    At<int>(aDdH) = h;
    At<int>(aSidebarX) = sidebar;
    At<int>(aMapW) = 848;
    At<int>(aMapH) = 746;
    At<int>(aMapScrollX) = 0;
    At<int>(aMapScrollY) = 0;
    At<int>(aMapNumber) = 1;
    At<char>(aGameScreen) = 0x14;
    g_map = MapCalls{};
}

WORD Screen(int x, int y)
{
    return g_mapScreen[static_cast<size_t>(y) * At<int>(aDdW) + x];
}

void R2cChinaMap()
{
    Thunk(0x5b9c40, reinterpret_cast<void *>(&FakeMapDraw));
    Thunk(0x489d20, reinterpret_cast<void *>(&FakeMapInput));
    Thunk(0x42f900, reinterpret_cast<void *>(&FakeCampaignDraw));
    Thunk(0x42f7d0, reinterpret_cast<void *>(&FakeCampaignClick));
    Thunk(0x5b6480, reinterpret_cast<void *>(&FakeTopBar)); // the detour's original
    g_topBarHook = MapDetour<MapTop_fn>("top_bar_piece_and_popup");
    auto draw = MapDetour<MapVoid_fn>("china_map_draw");
    auto input = MapDetour<MapVoid_fn>("china_map_input");

    // 2560x1440, sidebar at 2334: the map area is 2334 x 1400 below the top bar.
    MapScreen(2560, 1440, 2334);
    emperor::china_map::SetModeForTest(0, false);
    draw();
    Check(g_map.draws == 1 && g_map.drawBuffer != At<BYTE *>(aDrawBuffer), "scaled: the game draws into our layer");
    Check(At<BYTE *>(aDrawBuffer) == reinterpret_cast<BYTE *>(g_mapScreen.data()), "the screen's buffer afterwards");
    // Frame 880 x 778 at scale min(2334 / 880, 1400 / 778) = 1.7995: 1584 x 1400 at (375, 40); the map at +29.
    const double s = 1400.0 / 778;
    const int tx = (2334 - static_cast<int>(880 * s + 0.5)) / 2, ty = 40; // the framed map: 375, 40
    const int mapX = tx + static_cast<int>(16 * s), mapY = ty + static_cast<int>(16 * s);
    const int cx = tx + static_cast<int>((16 + 424 + 0.5) * s), cy = ty + static_cast<int>((16 + 373 + 0.5) * s);
    if (Screen(cx, cy) != MapPattern(424, 373)) {
        std::printf("    at (%d,%d): %04x, expected %04x; around: %04x %04x\n", cx, cy, Screen(cx, cy), MapPattern(424, 373),
                    Screen(cx - 1, cy), Screen(cx + 1, cy));
    }
    Check(Screen(cx, cy) == MapPattern(424, 373), "scaled: the map's middle pixel in the middle of the screen's map");
    Check(Screen(mapX - 10, mapY + 100) == 0x1111, "scaled: the fret border left of the map");
    Check(Screen(20, 700) != kSentinel16 && Screen(20, 700) != MapPattern(20, 660), "scaled: the backdrop outside");
    Check(Screen(2400, 700) == kSentinel16 && Screen(100, 10) == kSentinel16,
          "the sidebar and the top bar are not touched");
    Check(g_map.tops == 1 && g_map.topBuffer == At<BYTE *>(aDrawBuffer),
          "top bar piece: drawn once, after the map, on the screen (not into the layer)");
    // Mouse: on the map, in the backdrop, on the sidebar.
    At<int>(aMouseX) = cx;
    At<int>(aMouseY) = cy;
    input();
    Check(g_map.inputs == 1 && g_map.mouseX == 424 && g_map.mouseY == 373 + 40,
          "mouse on the map: the game sees its own map coordinates");
    Check(At<int>(aMouseX) == cx && At<int>(aMouseY) == cy, "and the real position afterwards");
    At<int>(aMouseX) = 100;
    At<int>(aMouseY) = 700;
    input();
    Check(g_map.mouseX == 850 && g_map.mouseY == 42, "mouse on the backdrop: an empty point of the game's map window");
    At<int>(aMouseX) = 2400;
    At<int>(aMouseY) = 100;
    input();
    Check(g_map.mouseX == 2400 && g_map.mouseY == 100, "mouse on the sidebar: the real position");

    // Native: the map at its own size in the middle of the area, pixel for pixel.
    MapScreen(2560, 1440, 2334);
    emperor::china_map::SetModeForTest(1, false);
    draw();
    const int nx = (2334 - 880) / 2 + 16, ny = 40 + (1400 - 778) / 2 + 16;
    Check(Screen(nx, ny) == MapPattern(0, 0) && Screen(nx + 847, ny + 745) == MapPattern(847, 745) &&
              Screen(nx + 300, ny + 200) == MapPattern(300, 200),
          "native: the map pixel for pixel, centred");
    At<int>(aMouseX) = nx + 300;
    At<int>(aMouseY) = ny + 200;
    input();
    Check(g_map.mouseX == 300 && g_map.mouseY == 240, "native: the mouse in the game's map coordinates");

    // Integer scaling at 2560x1440: 1.8 rounds down to 1, the same as native.
    MapScreen(2560, 1440, 2334);
    emperor::china_map::SetModeForTest(0, true);
    draw();
    Check(Screen(nx + 300, ny + 200) == MapPattern(300, 200), "integer scaling at 2560x1440: scale 1");
    // 3840x2160: integer scale 2.
    MapScreen(3840, 2160, 3614);
    draw();
    const int ix = (3614 - 1760) / 2 + 32, iy = 40 + (2120 - 1556) / 2 + 32;
    Check(Screen(ix + 600, iy + 400) == MapPattern(300, 200) && Screen(ix + 601, iy + 401) == MapPattern(300, 200),
          "integer scaling at 3840x2160: each map pixel a 2 x 2 block");

    // Original, the editor's map, a screen too small for the whole map: the game's own drawing and mouse.
    MapScreen(2560, 1440, 2334);
    emperor::china_map::SetModeForTest(2, false);
    draw();
    Check(g_map.drawBuffer == At<BYTE *>(aDrawBuffer) && Screen(5, 45) == MapPattern(5, 5),
          "original: drawn straight to the screen at (0, 40)");
    At<int>(aMouseX) = 100;
    At<int>(aMouseY) = 700;
    input();
    Check(g_map.mouseX == 100 && g_map.mouseY == 700, "original: the real mouse");
    MapScreen(2560, 1440, 2334);
    emperor::china_map::SetModeForTest(0, false);
    At<char>(aGameScreen) = 0x15;
    draw();
    Check(g_map.drawBuffer == At<BYTE *>(aDrawBuffer), "the editor's map (0x15): the game's way");
    MapScreen(1024, 768, 798);
    draw();
    Check(g_map.drawBuffer == At<BYTE *>(aDrawBuffer), "1024x768 (the map scrolls): the game's way");

    // Campaign map (0x34): markers into the layer and shown with the map, the sidebar's strip to the screen.
    MapScreen(2560, 1440, 2334);
    At<char>(aGameScreen) = 0x34;
    draw();
    At<int>(aMouseX) = cx;
    At<int>(aMouseY) = cy;
    MapDetour<MapVoid_fn>("campaign_map_sidebar_draw")();
    Check(g_map.campaign == 1 && g_map.mouseX == 424 && g_map.mouseY == 413, "campaign map: hover in map coordinates");
    const int mx = tx + static_cast<int>((16 + 100 + 0.5) * s), my = ty + static_cast<int>((16 + 100 + 0.5) * s);
    Check(Screen(mx, my) == 0x0001, "campaign map: the marker shown on the scaled map");
    Check(Screen(2334 + 5, 5) == 0x0002, "campaign map: the sidebar drawn where it belongs");
    MapDetour<MapVoid_fn>("campaign_map_click")();
    Check(g_map.clicks == 1 && g_map.mouseX == 424, "campaign map: the click in map coordinates");

    // Animations of speed 0 (the allies' flags): one step per 50 ms of frame_time, the same for every call of a pass.
    Thunk(0x5b9760, reinterpret_cast<void *>(&FakeAnim));
    using Anim_fn = unsigned(__cdecl *)(int, unsigned);
    auto anim = MapDetour<Anim_fn>("anim_advance_ui");
    At<BYTE>(0x8901cc) = 1;
    unsigned frame = 1;
    int steps = 0;
    bool same = true;
    for (DWORD t = 100000; t < 101000; t += 5) { // 200 passes in one second, two calls each
        At<DWORD>(0x1156dec) = t;
        const unsigned a = anim(0, frame), b = anim(0, frame);
        steps += a != frame;
        same = same && a == b;
        frame = a;
    }
    Check(same, "speed 0: every call of a pass steps alike");
    Check(steps == 20, "speed 0: 20 steps in a second of 200 passes (one per 50 ms)");
    Check(At<BYTE>(0x8901cc) == 1, "frame_timer_flags[0] as the game set it afterwards");

    // The city map panels: the rectangle cut to the 924 x 914 grid around the same centre.
    Thunk(0x5cbc30, reinterpret_cast<void *>(&FakePanel));
    Thunk(0x5cb170, reinterpret_cast<void *>(&FakePanel));
    using Panel_fn = void(__fastcall *)(void *, void *, int, int, int, int);
    MapDetour<Panel_fn>("city_map_panel_spy")(nullptr, nullptr, 0, 40, 2334, 1400);
    Check(g_panel.calls == 1 && g_panel.w == 924 && g_panel.h == 914 && g_panel.x == 704 && g_panel.y == 282 &&
              g_panel.x + g_panel.w / 2 == 1166 && g_panel.y + g_panel.h / 2 == 739,
          "spy map at 2560x1440: (704, 282) 924 x 914, the same centre (rows in pairs)");
    MapDetour<Panel_fn>("city_map_panel")(nullptr, nullptr, 0, 40, 798, 728);
    Check(g_panel.calls == 2 && g_panel.x == 0 && g_panel.y == 40 && g_panel.w == 798 && g_panel.h == 728,
          "city map at 1024x768: unchanged");

    // The map's sidebar: drawn by the game, then the leather below it (resolution's filler, here off: 1024 x 768).
    Thunk(0x5b81d0, reinterpret_cast<void *>(&FakeFrameDraw));
    MapDetour<MapVoid_fn>("china_map_frame_draw")();
    Check(g_frameDraws == 1, "china_map_frame_draw: the game's sidebar drawn");

    // A fault while the game draws into our layer: off, the game's own drawing again.
    MapScreen(2560, 1440, 2334);
    g_fault = "china_map_draw";
    draw();
    Check(g_map.draws == 2 && g_map.drawBuffer == At<BYTE *>(aDrawBuffer) && Screen(5, 45) == MapPattern(5, 5),
          "fault: drawn again the game's way, straight to the screen");
    Check(LogContains("feature \"China map layout"), "fault: the China map layout is switched off");
    At<int>(aMouseX) = 100;
    At<int>(aMouseY) = 700;
    input();
    Check(g_map.mouseX == 100, "fault: the real mouse from then on");
}

// R3: the display scale steps and the status lines of our Display options dialog (pure parts).
void R3DisplayOptions()
{
    namespace o = emperor::display_options;
    Check(o::MaxScale(2560, 1440, 1024, 768) == 185, "2560x1440: the largest scale is 185% (1383x778)");
    Check(o::MaxScale(1920, 1080, 1024, 768) == 140, "1920x1080: the largest scale is 140% (1371x771)");
    Check(o::MaxScale(1024, 768, 1024, 768) == 100, "1024x768: only 100%");
    Check(o::MaxScale(3840, 2160, 1024, 768) == 250, "3840x2160: up to 250% (1536x864; R4 test C, offline)");
    Check(o::StepScale(250, 1, 3840, 2160, 1024, 768) == 250, "+ at 250% on 4K stays");
    Check(o::StepScale(0, 1, 2560, 1440, 1024, 768) == 135, "+ from auto (133% at 1440p) -> 135%");
    Check(o::StepScale(0, -1, 2560, 1440, 1024, 768) == 130, "- from auto -> 130%");
    Check(o::StepScale(0, 1, 3840, 2160, 1024, 768) == 205, "+ from auto (200% at 4K) -> 205%");
    Check(o::StepScale(125, 1, 2560, 1440, 1024, 768) == 130, "+ from 125% -> 130%");
    Check(o::StepScale(185, 1, 2560, 1440, 1024, 768) == 185, "+ at the largest scale stays");
    Check(o::StepScale(100, -1, 2560, 1440, 1024, 768) == 100, "- at 100% stays");
    Check(o::StepScale(250, -1, 2560, 1440, 1024, 768) == 180, "- from a scale too large: from the one in effect");
    char a[48], b[48];
    o::StatusLines(2048, 1152, true, 125, 125, a, sizeof(a), b, sizeof(b));
    Check(std::strcmp(a, "Resolution: 2048 x 1152") == 0 && std::strcmp(b, "Display scale: 125%") == 0,
          "status: scaled 125%");
    o::StatusLines(1896, 1066, true, 0, 135, a, sizeof(a), b, sizeof(b));
    Check(std::strcmp(b, "Display scale: Auto (135%)") == 0, "status: auto");
    o::StatusLines(1024, 768, false, 125, 125, a, sizeof(a), b, sizeof(b));
    Check(std::strcmp(a, "Resolution: 1024 x 768") == 0 && std::strcmp(b, "Display scale: off") == 0,
          "status: a fixed size");
}

struct Scenario {
    const char *name;
    void (*run)();
};
const Scenario kScenarios[] = {
    {"m4_render_ok", &M4RenderOk},       {"m4_render_fault", &M4RenderFault},
    {"m4_convert_fault", &M4ConvertFault}, {"m4_direction_fault", &M4DirectionFault},
    {"m4_drag_fault", &M4DragFault},     {"r1_switch_fault", &R1SwitchFault},
    {"r1_mode_fault", &R1ModeFault},     {"r1_layout_fault", &R1LayoutFault},
    {"r2b1_extent", &R2b1Extent},       {"r2b1_message_panel", &R2b1MessagePanel},
    {"r1_info", &R1Info},                {"r1_info_positions", &R1InfoPositions},
    {"r1_bars_filler", &R1BarsAndFiller}, {"r1_clamp_fault", &R1ClampFault},
    {"p2_draw_only", &P2DrawOnly},       {"r2b_fixed_window", &R2bFixedWindow},
    {"r2b_fixed_original", &R2bFixedOriginal}, {"r2b_sidebar_edges", &R2bSidebarEdges},
    {"r2b_unknown_class", &R2bUnknownClass}, {"r2b17_ui_fixes", &R2b17UiFixes},
    {"r2c_china_map", &R2cChinaMap},   {"r3_display_options", &R3DisplayOptions},
};

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 2) {
        for (const Scenario &s : kScenarios) {
            if (std::strcmp(argv[1], s.name) == 0) {
                DeleteFileA("test_restore.log");
                core_log_open(GetModuleHandleW(nullptr), "test_restore.log");
                core::diag::InstallExceptionLog();
                Setup();
                s.run();
                return g_failures ? 1 : 0;
            }
        }
        std::printf("unknown scenario %s\n", argv[1]);
        return 2;
    }
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    int failed = 0;
    for (const Scenario &s : kScenarios) {
        std::printf("%s\n", s.name);
        std::fflush(stdout);
        char cmd[MAX_PATH + 64];
        std::snprintf(cmd, sizeof(cmd), "\"%s\" %s", self, s.name);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        DWORD code = 99;
        if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 30000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        if (code != 0) {
            std::printf("  scenario %s FAILED (exit code %lu)\n", s.name, code);
            ++failed;
        }
    }
    std::printf("RESULT: %s (%d scenario(s) failed)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
