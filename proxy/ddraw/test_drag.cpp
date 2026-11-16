// Offline test of core/scroll/drag (not the game): drag scrolling with the middle mouse button on a fake game -- the
// camera's variables in an array of ours (through core::site::g_imageShift, as test_camera), a fake system (the cursor,
// its hiding, the capture, the buttons, the focus, the output's scale, the two settings) and a fake zoom filter after
// ours. Checked: a click (released within the threshold) goes to the zoom as a press at the press's place; moves
// within the threshold reach the game; past it a drag: the cursor hidden and kept where it was pressed, the camera
// moved the other way by the mouse's movement (in game pixels: the output's scale and the zoom's), the edges not
// scrolling meanwhile, the game's own step told "no direction"; released: the cursor shown again where it was pressed;
// with smooth scrolling off whole tiles only; not taken off the city, during a military drag selection or with the left
// button held; the left and right buttons swallowed with their releases while dragging; the drag ends when the focus
// goes, the screen is no city screen or End is called; the setting off: the zoom has the middle button as before; the
// edge width; and a fault inside the drag: switched off, the cursor shown, the zoom still has the click.
#include "core/diag/guard.h"
#include "core/log.h"
#include "core/scroll/camera.h"
#include "core/scroll/drag.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace camera = core::scroll::camera;
namespace drag = core::scroll::drag;

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

enum Slot {
    kViewX, kViewY, kTileW, kTileHalfH, kCursorX, kCursorY, kDesktopW, kDesktopH, kKeyL, kKeyR, kKeyU, kKeyD,
    kScrolling, kSpeed, kFrame, kSlots
};
int g_mem[kSlots];
constexpr DWORD kBase = 0x500000;
camera::IntVar V(Slot s)
{
    return camera::IntVar{kBase + 4 * static_cast<DWORD>(s)};
}
int &M(Slot s)
{
    return g_mem[s];
}

void FakeClamp()
{
    int &x = M(kViewX), &y = M(kViewY);
    x = x < 0 ? 0 : x > 1000 ? 1000 : x;
    y = y < 0 ? 0 : y > 2000 ? 2000 : y;
    y &= ~1;
}

double g_zoomFx = 0; // 0: not zoomed
core::diag::Feature g_cameraFeature{"smooth scrolling (test_drag)"};
const camera::CameraGame kCamera{
    "test",
    &g_cameraFeature,
    camera::CameraVars{V(kViewX), V(kViewY), V(kTileW), V(kTileHalfH), V(kCursorX), V(kCursorY), V(kDesktopW),
                       V(kDesktopH), V(kKeyL), V(kKeyR), V(kKeyU), V(kKeyD), V(kScrolling), V(kSpeed), V(kFrame)},
    &FakeClamp,
    [] {},
    [](unsigned) { return false; },
    [] {},
    [] { return false; },
    [] {},
    [](double *fx, double *fy) {
        if (g_zoomFx <= 0) {
            return false;
        }
        *fx = *fy = g_zoomFx;
        return true;
    },
};

// The fake game.
bool g_cityScreen = true, g_onCity = true, g_dragSelecting = false;
int g_zoomPresses = 0, g_zoomOther = 0, g_zoomX = 0, g_zoomY = 0;
bool ZoomMouse(UINT m, WPARAM, int x, int y)
{
    if (m == WM_MBUTTONDOWN) {
        ++g_zoomPresses;
        g_zoomX = x;
        g_zoomY = y;
        return true;
    }
    ++g_zoomOther;
    return false;
}
const drag::DragGame kGame{
    "test",
    [] { return g_cityScreen; },
    [](int, int) { return g_onCity; },
    [] { return g_dragSelecting; },
    [] { return true; },
    &ZoomMouse,
};

// The fake system.
POINT g_cursor{500, 400};
bool g_hidden = false, g_captured = false, g_middle = false, g_focused = true, g_enabled = true;
double g_scale = 1;
int g_edgeWidth = 1;
bool g_faultCursor = false;
volatile int *volatile g_null = nullptr;
bool (*g_filter)(UINT, WPARAM, int, int) = nullptr;
const drag::DragSystem kSystem{
    [](POINT *p) {
        if (g_faultCursor) {
            (void)*g_null; // a fault inside the drag
        }
        *p = g_cursor;
        return true;
    },
    [](POINT p) { g_cursor = p; },
    [](bool h) { g_hidden = h; },
    [](bool on) { g_captured = on; },
    [] { return g_middle; },
    [] { return g_focused; },
    [] { return g_scale; },
    [] { return g_enabled; },
    [] { return g_edgeWidth; },
    [](bool (*f)(UINT, WPARAM, int, int)) { g_filter = f; },
};

// The mouse as the window procedure gives it (the game's pixels = screen pixels / g_scale, here simply the same).
bool Send(UINT m, WPARAM w = 0)
{
    return g_filter(m, w, g_cursor.x, g_cursor.y);
}
bool Press()
{
    g_middle = true;
    return Send(WM_MBUTTONDOWN, MK_MBUTTON);
}
bool Release()
{
    g_middle = false;
    return Send(WM_MBUTTONUP);
}
bool MoveBy(int dx, int dy)
{
    g_cursor.x += dx;
    g_cursor.y += dy;
    return Send(WM_MOUSEMOVE, g_middle ? MK_MBUTTON : 0);
}
// One game frame at the city's call site: true when the game's own step is told "no direction".
bool Frame()
{
    ++M(kFrame);
    camera::OnLoop(kCamera, 16);
    return camera::CityDirection(kCamera);
}
int ViewPxX()
{
    int px, py;
    camera::Remainder(&px, &py);
    return M(kViewX) * (M(kTileW) + 2) + px;
}
int ViewPxY()
{
    int px, py;
    camera::Remainder(&px, &py);
    return M(kViewY) * M(kTileHalfH) + py;
}
void Reset()
{
    drag::End("test reset");
    drag::ResetForTest();
    g_cursor = POINT{500, 400};
    g_middle = false;
    g_hidden = g_captured = false;
    g_focused = g_enabled = g_cityScreen = g_onCity = true;
    g_dragSelecting = false;
    g_scale = 1;
    g_zoomFx = 0;
    g_zoomPresses = g_zoomOther = 0;
    M(kViewX) = 100;
    M(kViewY) = 100;
    camera::SetRemainderForTest(kCamera, 0, 0);
    camera::SetSmooth(kCamera, true);
}

void Click()
{
    Reset();
    Check(Press(), "a middle press over the city is ours");
    Check(g_captured && !g_hidden && !drag::Dragging(), "pressed: the mouse captured, the cursor still shown");
    Check(!MoveBy(3, -2), "a move within the threshold (3, -2) reaches the game");
    Check(Release(), "the release is ours");
    Check(g_zoomPresses == 1 && g_zoomX == 500 && g_zoomY == 400,
          "released within the threshold: a click, given to the zoom as a press where the button went down");
    Check(!g_captured && !g_hidden && M(kViewX) == 100, "after the click: nothing captured, the view as it was");
}

// A quick click: the button found up by the frame's poll before its WM_MBUTTONUP arrives.
void QuickClick()
{
    Reset();
    Press();
    g_middle = false;
    Frame();
    Check(!g_hidden && g_zoomPresses == 0, "a quick click polled up before its release message: still waiting");
    Check(Send(WM_MBUTTONUP) && g_zoomPresses == 1, "... its WM_MBUTTONUP still makes the click (back to 100%)");
}

void Drag()
{
    Reset();
    Press();
    const int x0 = ViewPxX(), y0 = ViewPxY();
    Check(MoveBy(5, 0), "past the threshold (5 px): a drag, the move is ours");
    Check(drag::Dragging() && g_hidden && g_cursor.x == 500, "dragging: the cursor hidden and put back where pressed");
    M(kCursorX) = 0; // the game's cursor at the left edge: would edge-scroll
    g_cursor.x += 30;
    g_cursor.y -= 12;
    Check(Frame(), "the frame of a drag: the game's own step is told no direction");
    Check(ViewPxX() == x0 - 30 && ViewPxY() == y0 + 12,
          "the mouse 30 right and 12 up: the view 30 px left and 12 px down (the map follows the mouse; no edge "
          "scrolling meanwhile)");
    Check(g_cursor.x == 500 && g_cursor.y == 400, "the cursor put back where the button went down");
    Check(M(kScrolling) == 1, "view_scrolling set while the drag moves the view (as edge scrolling)");
    Check(MoveBy(1, 1) && Send(WM_MOUSEWHEEL), "while dragging the moves and the wheel are ours");
    Check(Release() && !g_hidden && !g_captured && g_zoomPresses == 0,
          "released after a drag: the cursor shown again, not a click (the zoom gets nothing)");
    Check(g_cursor.x == 500 && g_cursor.y == 400, "... and the cursor where the button went down");
    M(kCursorX) = 500;
}

void Scales()
{
    Reset();
    Press();
    MoveBy(10, 0);
    g_scale = 2; // two screen pixels a game pixel
    g_cursor.x += 20;
    const int x0 = ViewPxX();
    Frame();
    Check(ViewPxX() == x0 - 10, "output scale 2: 20 screen px move the view 10 game px");
    g_zoomFx = 1.5; // zoomed out: 1.5 game px a screen... game pixel
    g_cursor.x += 4;
    Frame();
    Check(ViewPxX() == x0 - 13, "zoom 1.5: 4 screen px = 2 game px x 1.5 = 3 px");
    g_cursor.x += 1;
    Frame();
    Check(ViewPxX() == x0 - 13, "a fraction (0.75 px) waits for the next frame");
    g_cursor.x += 1;
    Frame();
    Check(ViewPxX() == x0 - 14, "... and is moved once it makes a pixel");
    Release();
}

void WholeTiles()
{
    Reset();
    camera::SetSmooth(kCamera, false);
    Press();
    MoveBy(10, 0);
    const int vx = M(kViewX);
    g_cursor.x -= 50; // 50 px left: less than a column (60 px)
    Check(Frame(), "smooth off, dragging: the game's own step still told no direction");
    Check(M(kViewX) == vx, "smooth off: 50 px is less than a column, the view waits");
    g_cursor.x -= 20; // 70 in all
    Frame();
    int px, py;
    camera::Remainder(&px, &py);
    Check(M(kViewX) == vx + 1 && px == 0, "smooth off: 70 px make one whole column, no remainder");
    Check(M(kScrolling) == 1, "smooth off: view_scrolling set while the drag moved the view");
    Release();
    Check(!Frame(), "smooth off and no drag: the game's own steps");
    Check(M(kScrolling) == 0, "smooth off: view_scrolling cleared in the frame after the drag");
    camera::SetSmooth(kCamera, true);
}

void NotTaken()
{
    Reset();
    g_onCity = false;
    Press();
    Check(!g_captured && g_zoomPresses == 1, "off the city (the sidebar): the press goes on to the zoom (and the game)");
    g_middle = false;
    Reset();
    g_dragSelecting = true;
    Press();
    Check(!g_captured && g_zoomPresses == 1, "during a military drag selection: not ours (the zoom's)");
    g_middle = false;
    Reset();
    g_middle = true;
    Send(WM_MBUTTONDOWN, MK_MBUTTON | MK_LBUTTON);
    Check(!g_captured && g_zoomPresses == 1, "with the left button held (a road): not ours (the zoom's)");
    Reset();
    g_cityScreen = false;
    Press();
    Check(!g_captured && g_zoomPresses == 1, "not a city screen: not ours (the zoom's)");
    g_middle = false;
}

void Buttons()
{
    Reset();
    Press();
    MoveBy(10, 0);
    Check(Send(WM_LBUTTONDOWN, MK_LBUTTON | MK_MBUTTON) && Send(WM_RBUTTONDOWN, MK_RBUTTON | MK_MBUTTON),
          "while dragging the left and right presses are ours");
    Release();
    Check(Send(WM_LBUTTONUP) && Send(WM_RBUTTONUP), "... and their releases after the drag ended too");
    Check(!Send(WM_LBUTTONUP), "a later left release reaches the game again");
    Reset();
    Press();
    Check(!Send(WM_LBUTTONDOWN, MK_LBUTTON | MK_MBUTTON), "pressed (no drag yet), the left button: the game's");
    Check(!g_captured && !Release(), "... and the press ended without a click");
    Check(g_zoomPresses == 0, "no click after another button went down");
}

void Ends()
{
    Reset();
    Press();
    MoveBy(10, 0);
    g_focused = false;
    Frame();
    Check(!drag::Dragging() && !g_hidden && !g_captured, "the window lost the focus: the drag ends, the cursor shown");
    g_middle = false;
    Reset();
    Press();
    MoveBy(10, 0);
    g_cityScreen = false;
    camera::OnLoop(kCamera, 16); // no city call site on another screen: the loop ends it
    Check(!drag::Dragging() && !g_hidden, "another screen: the drag ends");
    g_middle = false;
    Reset();
    Press();
    MoveBy(10, 0);
    drag::End("our panel opened");
    Check(!drag::Dragging() && !g_hidden && !g_captured, "End (our panel): the drag ends, the cursor shown");
    Check(Release() == false && g_zoomPresses == 0, "the release after End: not ours, no click");
    Reset();
    Press();
    MoveBy(10, 0);
    g_middle = false; // released where we did not see it
    Frame();
    Check(!drag::Dragging() && !g_hidden, "the middle button found up: the drag ends");
}

void SettingOff()
{
    Reset();
    g_enabled = false;
    Check(Press() && g_zoomPresses == 1 && !g_captured,
          "setting off: the middle press goes straight to the zoom (back to 100% on the press, as before)");
    g_middle = false;
}

void EdgeWidth()
{
    Reset();
    M(kCursorY) = 400;
    M(kCursorX) = 3;
    g_edgeWidth = 1;
    Frame();
    Check(M(kScrolling) == 0, "edge width 1: the cursor 3 px from the left edge does not scroll");
    g_edgeWidth = 5;
    Frame();
    Check(M(kScrolling) == 1, "edge width 5: 3 px from the left edge scrolls");
    M(kCursorX) = M(kDesktopW) - 5;
    Frame();
    Check(M(kScrolling) == 1, "edge width 5: 5 px from the right edge scrolls");
    M(kCursorX) = M(kDesktopW) - 6;
    Frame();
    Check(M(kScrolling) == 0, "edge width 5: 6 px from the right edge does not");
    g_scale = 2; // two screen pixels a game pixel: 6 screen px are 3 of the game's
    g_edgeWidth = 6;
    M(kCursorX) = 2;
    Frame();
    Check(M(kScrolling) == 1, "edge width 6 screen px at scale 2: 2 game px from the edge scrolls");
    M(kCursorX) = 3;
    Frame();
    Check(M(kScrolling) == 0, "... 3 game px (6 screen px) does not");
    g_scale = 1;
    g_edgeWidth = 0; // off
    M(kCursorX) = 0;
    Frame();
    Check(M(kScrolling) == 0, "edge width 0: the cursor on the left edge does not scroll");
    M(kCursorX) = M(kDesktopW) - 1;
    M(kCursorY) = M(kDesktopH) - 1;
    Frame();
    Check(M(kScrolling) == 0, "edge width 0: the bottom right corner does not scroll either");
    M(kCursorY) = 400;
    g_edgeWidth = 1;
    M(kCursorX) = 500;
    Frame();
}

// The game's own direction (smooth scrolling off) through the edge width: what the game's edge test sees.
int g_seenX = 0, g_seenY = 0;
int FakeGameDirection()
{
    g_seenX = M(kCursorX);
    g_seenY = M(kCursorY);
    return 42;
}
void GameDirection()
{
    Reset();
    camera::SetSmooth(kCamera, false);
    M(kCursorY) = 400;
    g_edgeWidth = 10;
    M(kCursorX) = 7;
    Frame(); // the loop gives the camera the width
    Check(camera::GameDirection(kCamera, &FakeGameDirection) == 42, "the game's own direction is returned");
    Check(g_seenX == 0 && g_seenY == 400, "width 10: 7 px from the left edge is seen on the edge (0) by the game");
    Check(M(kCursorX) == 7, "... and the cursor put back afterwards");
    M(kCursorX) = M(kDesktopW) - 10;
    camera::GameDirection(kCamera, &FakeGameDirection);
    Check(g_seenX == M(kDesktopW) - 1, "width 10: 10 px from the right edge is seen on the right edge");
    M(kCursorX) = 500;
    camera::GameDirection(kCamera, &FakeGameDirection);
    Check(g_seenX == 500, "away from the edges: unchanged");
    g_edgeWidth = 0;
    Frame();
    M(kCursorX) = 0;
    M(kCursorY) = M(kDesktopH) - 1;
    camera::GameDirection(kCamera, &FakeGameDirection);
    Check(g_seenX == 1 && g_seenY == M(kDesktopH) - 2, "width 0: the cursor on an edge is seen a pixel off it");
    Check(M(kCursorX) == 0 && M(kCursorY) == M(kDesktopH) - 1, "... and put back afterwards");
    g_edgeWidth = 1;
    Frame();
    M(kCursorX) = 0;
    camera::GameDirection(kCamera, &FakeGameDirection);
    Check(g_seenX == 0, "width 1: the game's own test, unchanged");
    M(kCursorX) = 500;
    M(kCursorY) = 400;
    camera::SetSmooth(kCamera, true);
}

void Fault()
{
    Reset();
    Press();
    MoveBy(10, 0);
    g_faultCursor = true;
    g_cursor.x += 20;
    const int x0 = ViewPxX();
    Frame();
    g_faultCursor = false;
    Check(!drag::Dragging() && !g_hidden && !g_captured, "a fault inside the drag: ended, the cursor shown");
    Check(ViewPxX() == x0, "... the view not moved by it");
    g_middle = false;
    g_zoomPresses = 0;
    Check(Press() && g_zoomPresses == 1 && !g_captured, "switched off: the middle press goes to the zoom as before");
    g_middle = false;
    Check(camera::Smooth() && core::diag::Enabled(g_cameraFeature), "smooth scrolling is not switched off by the drag's fault");
}

} // namespace

int main()
{
    DeleteFileA("test_drag.log");
    core_log_open(GetModuleHandleW(nullptr), "test_drag.log");
    core::diag::InstallExceptionLog();
    core::site::g_imageShift = reinterpret_cast<DWORD>(g_mem) - kBase;
    M(kTileW) = 58;     // a column is 60 px
    M(kTileHalfH) = 15; // a row pair is 30 px
    M(kDesktopW) = 1024;
    M(kDesktopH) = 768;
    M(kSpeed) = 50;
    M(kCursorX) = 500;
    M(kCursorY) = 400;
    camera::Start(kCamera, true);
    drag::Install(kGame, kSystem);
    Check(g_filter != nullptr, "Install: the drag's mouse filter is core::display's");
    Click();
    QuickClick();
    Drag();
    Scales();
    WholeTiles();
    NotTaken();
    Buttons();
    Ends();
    SettingOff();
    EdgeWidth();
    GameDirection();
    Fault(); // last: a feature switched off stays off
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
