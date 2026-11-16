// Drag scrolling with the middle mouse button (drag.h). New for both games;
// the way it feels follows Augustus and Akhenaten (the cursor hidden and put back, a few pixels before a press becomes
// a drag), not their code.
#include "core/scroll/drag.h"

#include "core/diag/guard.h"
#include "core/log.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core::scroll::drag {
namespace {

namespace camera = core::scroll::camera;

core::diag::Feature g_feature{"drag scrolling (core/scroll/drag)"};
const DragGame *g_game = nullptr;
const DragSystem *g_sys = nullptr;

enum class State { Idle, Pressed, Dragging };
State g_state = State::Idle;
POINT g_anchor{};      // where the middle button went down (screen pixels): the cursor is kept there while dragging
int g_downX = 0, g_downY = 0; // ... in the game's pixels, for the click
WPARAM g_downKeys = 0;
double g_accX = 0, g_accY = 0; // the camera's movement not given yet (game pixels)
bool g_eatLeftUp = false, g_eatRightUp = false; // a press swallowed while dragging: its release too
int g_logs = 0;
constexpr int kLogs = 40;

void Log(const char *what)
{
    if (++g_logs <= kLogs) {
        core_log("%s: drag scrolling: %s", g_game ? g_game->log : "drag", what);
    }
}

bool Usable()
{
    return g_game && g_sys && core::diag::Enabled(g_feature) && g_sys->enabled() && g_game->drawingWorks();
}

void Stop(const char *why)
{
    if (g_state == State::Idle) {
        return;
    }
    const bool dragged = g_state == State::Dragging;
    g_state = State::Idle;
    g_accX = g_accY = 0;
    if (g_sys) {
        if (dragged) {
            g_sys->setCursorPos(g_anchor);
            g_sys->hideCursor(false);
        }
        g_sys->capture(false);
    }
    if (dragged || std::strcmp(why, "released") != 0) {
        char line[160];
        std::snprintf(line, sizeof(line), "%s ended (%s)", dragged ? "drag" : "press", why);
        Log(line);
    }
}

void BeginDrag()
{
    g_state = State::Dragging;
    g_accX = g_accY = 0;
    g_sys->setCursorPos(g_anchor);
    g_sys->hideCursor(true); // after: the game's own GetCursorPos is kept at where the button went down
    Log("drag started (cursor hidden, kept where the button went down)");
}

// Still allowed to drag (or to wait for one) now.
bool StillOk(const char **why)
{
    if (!Usable()) {
        *why = core::diag::Enabled(g_feature) && g_game && g_game->drawingWorks() ? "switched off"
                                                                                   : "not working";
        return false;
    }
    if (!g_game->cityScreen()) {
        *why = "not a city screen any more";
        return false;
    }
    if (!g_sys->focused()) {
        *why = "the window lost the focus";
        return false;
    }
    // Only while dragging: a quick click's release may be polled here before its WM_MBUTTONUP arrives, and that
    // message must still make the click.
    if (g_state == State::Dragging && !g_sys->middleHeld()) {
        *why = "the middle button is up";
        return false;
    }
    return true;
}

struct MouseCall {
    UINT m;
    WPARAM w;
    int x, y;
    bool taken;
    bool click; // a middle click: the press goes to the next filter (after the guard)
};

void TakeMouse(MouseCall &c)
{
    const UINT m = c.m;
    if (g_state == State::Idle) {
        if (m == WM_LBUTTONUP && g_eatLeftUp) {
            g_eatLeftUp = false;
            c.taken = true;
            return;
        }
        if (m == WM_RBUTTONUP && g_eatRightUp) {
            g_eatRightUp = false;
            c.taken = true;
            return;
        }
        if ((m != WM_MBUTTONDOWN && m != WM_MBUTTONDBLCLK) || !Usable() || !g_game->cityScreen() ||
            (c.w & (MK_LBUTTON | MK_RBUTTON)) != 0) {
            return;
        }
        if (g_game->dragSelecting() || !g_game->onCity(c.x, c.y)) {
            return; // the zoom's (or the game's) as before
        }
        if (!g_sys->cursorPos(&g_anchor)) {
            return;
        }
        g_state = State::Pressed;
        g_downX = c.x;
        g_downY = c.y;
        g_downKeys = c.w;
        g_sys->capture(true);
        c.taken = true;
        return;
    }
    if (g_state == State::Pressed) {
        if (m == WM_MBUTTONUP) {
            Stop("released");
            c.taken = true;
            c.click = true; // released without moving: the zoom's click
            return;
        }
        if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_LBUTTONDBLCLK || m == WM_RBUTTONDBLCLK) {
            Stop("another button went down"); // the game has that button as usual; no click
            return;
        }
        if (m == WM_MOUSEMOVE) {
            POINT now{};
            if (g_sys->cursorPos(&now) &&
                (std::abs(now.x - g_anchor.x) > kThreshold || std::abs(now.y - g_anchor.y) > kThreshold)) {
                BeginDrag();
                c.taken = true;
            }
        }
        return; // moves within the threshold reach the game as usual
    }
    // Dragging: every mouse message is ours.
    c.taken = true;
    if (m == WM_MBUTTONUP) {
        Stop("released");
    }
    else if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) {
        g_eatLeftUp = true;
    }
    else if (m == WM_RBUTTONDOWN || m == WM_RBUTTONDBLCLK) {
        g_eatRightUp = true;
    }
    else if (m == WM_LBUTTONUP) {
        g_eatLeftUp = false;
    }
    else if (m == WM_RBUTTONUP) {
        g_eatRightUp = false;
    }
}

// The camera's frame (camera.cpp, inside its guard): the movement since the last frame, in game pixels.
struct TakeCall {
    const camera::CameraGame *g;
    bool smooth;
    int dx, dy;
    bool dragging;
};

void TakeMove(TakeCall &t)
{
    const char *why = "";
    if (g_state != State::Idle && !StillOk(&why)) {
        Stop(why);
    }
    if (g_state != State::Dragging) {
        return;
    }
    t.dragging = true;
    POINT now{};
    if (!g_sys->cursorPos(&now)) {
        return;
    }
    const int sx = now.x - g_anchor.x, sy = now.y - g_anchor.y;
    if (sx == 0 && sy == 0) {
        return;
    }
    g_sys->setCursorPos(g_anchor);
    double scale = g_sys->screenPerGamePixel();
    if (!(scale > 0)) {
        scale = 1;
    }
    double fx = 1, fy = 1;
    if (!t.g->zoomScale(&fx, &fy)) {
        fx = fy = 1;
    }
    // The map follows the mouse: the view moves the other way.
    g_accX -= sx / scale * fx;
    g_accY -= sy / scale * fy;
    const camera::CameraVars &v = t.g->vars;
    const int colPx = *v.tileWidth + 2, rowPx = 2 * *v.tileHalfHeight;
    if (t.smooth || colPx <= 0 || rowPx <= 0) {
        t.dx = static_cast<int>(g_accX); // towards 0: the rest stays for the next frame
        t.dy = static_cast<int>(g_accY);
    }
    else {
        t.dx = static_cast<int>(g_accX / colPx) * colPx; // whole tiles only (no remainder without smooth scrolling)
        t.dy = static_cast<int>(g_accY / rowPx) * rowPx;
    }
    g_accX -= t.dx;
    g_accY -= t.dy;
}

// camera.cpp's hook: true while dragging (the edges and the arrow keys do not scroll then).
bool CameraTake(const camera::CameraGame &g, bool smooth, int *dx, int *dy)
{
    *dx = *dy = 0;
    if (!g_game) {
        return false;
    }
    TakeCall t{&g, smooth, 0, 0, false};
    if (!core::diag::Guarded(g_feature, [](void *p) { TakeMove(*static_cast<TakeCall *>(p)); }, &t)) {
        Stop("an error");
        Log("switched off after an error: the middle button only goes back to 100% zoom");
        return false;
    }
    *dx = t.dx;
    *dy = t.dy;
    return t.dragging;
}

// camera.cpp's hook, every frame: the edge width; a press or drag that may not go on ends.
void CameraLoop()
{
    if (!g_game) {
        return;
    }
    // The setting is in screen pixels; the camera compares the game's cursor (game pixels).
    double scale = g_sys->screenPerGamePixel();
    if (!(scale > 0)) {
        scale = 1;
    }
    const int setting = g_sys->edgeWidth();
    const long w = std::lround(setting / scale);
    camera::SetEdgeWidth(setting <= 0 ? 0 : w < 1 ? 1 : static_cast<int>(w)); // 0: no edge scrolling
    const char *why = "";
    if (g_state != State::Idle && !StillOk(&why)) {
        Stop(why);
    }
}

} // namespace

void Install(const DragGame &g, const DragSystem &s)
{
    g_game = &g;
    g_sys = &s;
    camera::SetDrag(&CameraTake, &CameraLoop);
    s.setMouseFilter(&MouseFilter);
    core_log("%s: drag scrolling installed (middle button over the city; %s; edge scrolling %d px)", g.log,
             s.enabled() ? "on" : "off (gameplay.drag_scroll)", s.edgeWidth());
}

bool MouseFilter(UINT message, WPARAM wparam, int x, int y)
{
    if (!g_game) {
        return false;
    }
    MouseCall c{message, wparam, x, y, false, false};
    if (core::diag::Enabled(g_feature) &&
        !core::diag::Guarded(g_feature, [](void *p) { TakeMouse(*static_cast<MouseCall *>(p)); }, &c)) {
        Stop("an error");
        Log("switched off after an error: the middle button only goes back to 100% zoom");
        c = MouseCall{message, wparam, x, y, false, false};
    }
    if (c.click) {
        // The zoom takes a middle press over the city (back to 100%); now that it was a click, it gets one.
        g_game->otherMouse(WM_MBUTTONDOWN, g_downKeys, g_downX, g_downY);
        return true;
    }
    if (c.taken) {
        return true;
    }
    return g_game->otherMouse(message, wparam, x, y);
}

void End(const char *why)
{
    Stop(why);
}

bool Dragging()
{
    return g_state == State::Dragging;
}

void ResetForTest()
{
    g_state = State::Idle;
    g_accX = g_accY = 0;
    g_eatLeftUp = g_eatRightUp = false;
    g_logs = 0;
}

} // namespace core::scroll::drag
