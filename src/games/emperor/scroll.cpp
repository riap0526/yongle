// M4: pixel (smooth) scrolling of the city view (docs/m4_smooth_scroll.md).
//
// The game's camera is view_x (tiles) / view_y (half rows, even). We add a pixel remainder (g_px, g_py).
//
// Drawing: the game's tile blitters (0x4527d0 and the others selected by 0x5cd8a0) only handle a tile cut exactly in
// half at a clip edge; the city clip rectangle (viewport_init: left/top = origin, right/bottom = origin + whole
// tiles) is always on that grid, so they never see anything else. Moving the origin by an arbitrary number of pixels
// breaks that (the first M4 build wrote past the back buffer and crashed at 0x452931). So city_render draws exactly
// as the game expects -- origin and clip on the tile grid -- but into our own buffer with the clip one column and one
// row pair larger on the right and bottom, and the view rectangle is then copied to the back buffer offset by
// (g_px, g_py). Everything city_render draws (tiles, figures, markers, the construction preview 0x46cb40) moves
// together.
//   city_render_tiles 0x46ff60: columns view_x-4 .. +width+6, rows view_y-8 .. +height+21  (covers the extra area)
//   0x46a810 (selection markers): columns -4 .. width+6, rows -8 .. height+5               (covers it)
//   0x46e1b0 (overlay markers): columns 0 .. width, rows 0 .. height-1 -- so while drawing, the viewport is also
//   made 1 column wider and 2 half rows taller.
// Coordinate conversion outside that drawing (clicks, the cursor tile, sounds, the floating text 0x46e000): the
// origin is moved by -(g_px, g_py) for the duration of the call and restored when it returns. Screen-to-grid calls
// made while city_render draws are converted the same way (their input is a screen position); grid_to_screen calls
// made while it draws are not (their output is a position in our buffer).
// The minimap (0x46e730/0x46e580) frames the view from the tile position, so its frame is at most one tile
// (about two minimap pixels) behind while a pixel remainder is present; it does not read the origin.
//
// Scrolling: at the city call sites of scroll_get_direction (0x52b310) the direction is computed every frame
// with the game's own rules (cursor on the screen edge, arrow keys), view_scrolling is set as the game sets it (so
// the simulation pauses while scrolling, as in the original), and the camera moves by elapsed time with Augustus's
// easing (core/scroll). The cruise speed equals the game's own average: one step of (tile_width+2) px horizontally
// and 4 half rows vertically every ((100-scroll_speed)/10)*12+2 ms (scroll_step_due 0x53a210), plus half a main-loop
// iteration because the game only steps on the first iteration after that time.
//
// City zoom (R5, docs/archive/resolution_ui_r5.md sections 9-10): the game's view is the zoomed buffer's; the city
// is drawn into our buffer of that size and scaled onto the screen (RenderZoomed). Screen positions given to the
// conversions are first turned into the buffer pixel they show, positions worked out outside the drawing are turned
// back to the screen, the drag selection works in buffer pixels with only its frame put on the screen, and the
// camera's speed is scaled so the screen scrolls as fast as at 100%.
//
// Failure (CLAUDE.md, docs/archive/resolution_ui_r2b.md §1): every call that changes game state for us runs under
// core/diag's guard with the changes in a Restore list (moved origin, larger viewport and clip, our buffer as
// draw_buffer, the camera of this frame). On an exception the list is put back, smooth scrolling is switched off
// for the rest of the run (remainder 0: the view snaps to its whole tile) and the call is made again the game's own
// way; from then on every hook passes the calls through, as with gameplay.smooth_scroll = off. Calls that change
// nothing (no remainder) go to the game outside the guard: a fault there is the game's own.
#include "games/emperor/scroll.h"

#include "core/capture/screenshot.h"
#include "core/ddraw/surface_memory.h"
#include "core/diag/guard.h"
#include "core/diag/perf.h"
#include "core/display/city_zoom.h"
#include "core/display/scaled_rect.h"
#include "core/log.h"
#include "core/scroll/smooth_scroll.h"
#include "games/emperor/keys.h"
#include "games/emperor/site.h"
#include "games/emperor/zoom.h"
#include "core/platform/parallel.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace emperor::scroll {

namespace {

// ---------------------------------------------------------------------------------------------
// Addresses (docs/symbols.md)

const Site kCityRender{"city_render", 0x46a220, {0xa1, 0x1c, 0x3a, 0xb5, 0x00, 0x8b, 0x4c, 0x24}};
const Site kScreenToGrid{"screen_to_grid", 0x469e50, {0x51, 0x53, 0x8b, 0x5c, 0x24, 0x0c, 0x55, 0x56}};
const Site kMouseToGrid{"mouse_to_grid", 0x469c60, {0x83, 0xec, 0x0c, 0x53, 0x8b, 0x1d, 0xa0, 0xe0}};
const Site kScreenToGridRaw{"screen_to_grid_unbounded", 0x44bd30, {0x51, 0x8b, 0x4c, 0x24, 0x0c, 0x53, 0x55, 0x56}};
const Site kGridToScreen{"grid_to_screen", 0x5a0f60, {0xa1, 0xd4, 0xf8, 0xad, 0x00, 0x83, 0xec, 0x08}};
const Site kScrollGetDirection{"scroll_get_direction", 0x52b310, {0x51, 0x53, 0x56, 0x57, 0xe8, 0xf7, 0xee, 0x00}};
const Site kViewScrollStep{"view_scroll_step", 0x53c950, {0xa1, 0x60, 0x6c, 0x85, 0x00, 0x8b, 0x4c, 0x24}};
const Site kDragStart{"drag_select_start", 0x44bb00, {0x8b, 0x15, 0xbc, 0xd0, 0x01, 0x01, 0x83, 0xec}};
const Site kDragDraw{"drag_select_draw", 0x44bb90, {0x83, 0xec, 0x0c, 0x8b, 0x15, 0xbc, 0xd0, 0x01}};
const Site kDragFinish{"drag_select_finish", 0x44bea0, {0x83, 0xec, 0x24, 0xa1, 0xac, 0xd0, 0x01, 0x01}};
const Site kViewClamp{"view_clamp", 0x53cbd0, {0x51, 0x53, 0x8b, 0x1d, 0xe0, 0xf6, 0x15, 0x01}};
const Site kSoundViewDecay{"sound_view_decay", 0x4456a0, {0xb9, 0x14, 0xf6, 0xa4, 0x00, 0x8d, 0x51, 0x08}};
// line_draw(x1, y1, x2, y2, colour): __cdecl, ret (the caller pops; tools/recon/callconv.py). R5c: the drag
// selection's frame is worked out in zoomed-buffer pixels and drawn on the screen (resolution_ui_r5.md section 10.2).
const Site kLineDraw{"line_draw", 0x5292d0, {0x8b, 0x44, 0x24, 0x10, 0x8b, 0x4c, 0x24, 0x08}};
// city_view_width_px(): __cdecl, no arguments, ret; (tile width + 2) x viewport columns. Its only caller is the
// "Game paused" banner (0x5b6e93: x = (width - 0x1c0) / 2). R5e: zoomed, the city's width on the screen.
const Site kCityWidth{"city_view_width_px", 0x46a1b0, {0xa1, 0xb8, 0xf8, 0xad, 0x00, 0x83, 0xc0, 0x02}};

// Return addresses of the scroll_get_direction calls whose result goes to view_scroll_step (the city view); the
// other two callers (0x539a23, 0x5f7349) scroll other things and keep the original behaviour.
const DWORD kCityScrollCalls[] = {0x538508, 0x538544, 0x5385da, 0x539f4f, 0x53800b, 0x538085, 0x54b4d4};

const Var<int> kViewX{0x101d0ac};
const Var<int> kViewY{0x101d0b0};
const Var<int> kViewDrawnX{0xadf8d8}; // view_x / view_y as city_render last drew them (mouse_to_grid's table lookup)
const Var<int> kViewDrawnY{0xadf8d4};
const Var<int> kOriginX{0xadf8f0};
const Var<int> kOriginY{0xadf8ec};
const Var<int> kWidthTiles{0xadf8c4};
const Var<int> kHeightRows{0xadf8c0};
const Var<int> kTileWidth{0xadf8b8};
const Var<int> kTileHalfHeight{0xadf8ac};
const Var<int> kCursorX{0x10de030};     // raw cursor (screen = mode coords)
const Var<int> kCursorY{0x10de02c};
const Var<int> kDesktopW{0x10de024};    // desktop_width
const Var<int> kDesktopH{0x10de020};
const Var<int> kKeyLeft{0x1156e10};     // set by game_wndproc on VK_LEFT
const Var<int> kKeyRight{0x1156e0c};
const Var<int> kKeyUp{0x1156e18};
const Var<int> kKeyDown{0x1156e14};
const Var<int> kViewScrolling{0x10de038};
const Var<int> kScrollSpeed{0x10de0e0};  // 10..100
const Var<int> kFrameCounter{0x10de418}; // game_frame calls
const Var<int> kClipLeft{0xadf89c};      // city clip (viewport_init)
const Var<int> kClipTop{0xadf894};
const Var<int> kClipRight{0xadf898};     // exclusive
const Var<int> kClipBottom{0xadf890};    // exclusive
const Var<BYTE *> kDrawBuffer{0x1156df8}; // draw_buffer
const Var<int> kMouseX{0x10de0a0};       // the game's mouse (mouse_to_grid reads these)
const Var<int> kMouseY{0x10de09c};
const Var<int> kScreenPitch{0x10de000};  // bytes per row
const Var<int> kDdWidth{0x10de008};
const Var<int> kDdHeight{0x10de004};
const Var<int> kSidebarX{0x13f8e0c};
const Var<unsigned char> kMouseMode{0x10de041}; // 0x4d during a military drag selection (docs/symbols.md)
constexpr unsigned char kMouseModeDragSelect = 0x4d;
const Var<int> kCursorTile{0x101d0b4};             // the cursor's tile index (cursor_tile_update 0x405500)
const Var<signed char> kTileHeights{0xbebf38 + 2}; // map_tile_height 0x408cf0: this + 2 + tile, 228 x 228
constexpr int kMapTiles = 228 * 228;

core::diag::Feature g_feature{"smooth scrolling (games/emperor/scroll)"};

// R5c (docs/archive/resolution_ui_r5.md section 10): while the city is zoomed, the game works in the zoomed buffer's
// pixels and the screen shows that buffer scaled. A screen position goes to the game as the buffer pixel it shows
// (ZoomToBuffer), a position the game works out outside the drawing comes back to the screen (ZoomToScreen); the
// smooth-scrolling remainder is the origin shift below, as before.
struct ZoomMap {
    int left, top;           // where the city starts (screen and buffer)
    int showRight, showBottom; // the city on the screen (exclusive)
    double fx, fy;           // buffer pixels per screen pixel
};
ZoomMap g_map{};          // the drawing's, while city_render draws (the back buffer's sizes are ours then)
ZoomMap g_dragMap{};      // the drag selection's, while drag_select_draw runs (dd_height is changed then)
bool g_bufferMode = false; // the drag selection works in buffer pixels: no zoom conversion meanwhile
bool g_dragLines = false;  // drag_select_draw draws: line_draw takes buffer pixels

// ---------------------------------------------------------------------------------------------
// Camera remainder and the origin shift

int g_px = 0, g_py = 0;       // pixel remainder: 0 <= g_px < tile_width+2, 0 <= g_py < 2*tile_half_height
int g_lastViewX = 0, g_lastViewY = 0;
bool g_haveLast = false;
int g_depth = 0;              // 1 while a conversion runs with the origin moved (calls inside it are not moved again)
bool g_inRender = false;      // city_render is drawing into our buffer (unshifted origin, larger clip)
int g_sinceDecayPx = 0;       // movement since the last sound_view_decay
int g_dragPx0 = 0, g_dragPy0 = 0; // the remainder when the current drag selection started
bool g_smooth = false;
core::scroll::SmoothScroll g_scroller;

// The feature was switched off by its guard: the game's whole tiles from now on (the game state of the abandoned
// call has been put back by the guard already).
void FallBack()
{
    static bool logged = false;
    g_px = g_py = 0;
    g_dragPx0 = g_dragPy0 = 0;
    g_inRender = false;
    g_depth = 0;
    g_scroller.Reset();
    if (!logged) {
        logged = true;
        core_log("scroll: smooth scrolling is off after an error: the game's whole-tile scrolling from now on");
    }
    // R5: the zoomed view must not reach the game's own drawing (straight into the back buffer): 100% first.
    emperor::zoom::Off("smooth scrolling was switched off after an error");
}

// Someone else moved the camera (minimap click, "go to", rotation, loading a game): start again on a whole tile.
void SyncView()
{
    if (!g_haveLast || *kViewX != g_lastViewX || *kViewY != g_lastViewY) {
        g_px = g_py = 0;
        g_scroller.Reset();
        g_lastViewX = *kViewX;
        g_lastViewY = *kViewY;
        g_haveLast = true;
    }
}

// Where the city is to be seen to (SetCityExtent): 0 = the game's clip.
int g_extRight = 0, g_extBottom = 0;

// The zoom now (false at 100%, or without our layout): outside the drawing from the screen's sizes, while
// city_render draws the drawing's own (RenderZoomed).
bool ZoomNow(ZoomMap *m)
{
    if (!core::diag::Enabled(g_feature)) {
        return false;
    }
    if (g_inRender) {
        if (g_map.fx <= 0) {
            return false;
        }
        *m = g_map;
        return true;
    }
    const int percent = emperor::zoom::Percent();
    if (percent == 100 || g_extRight <= 0 || g_extBottom <= 0) {
        return false;
    }
    int bufW = 0, bufH = 0;
    emperor::zoom::BufferSize(&bufW, &bufH);
    const int width = *kScreenPitch / 2, height = *kDdHeight;
    m->left = *kClipLeft;
    m->top = *kClipTop;
    m->showRight = g_extRight < width ? g_extRight : width;
    m->showBottom = g_extBottom < height ? g_extBottom : height;
    const int showW = m->showRight - m->left, showH = m->showBottom - m->top;
    if (showW <= 0 || showH <= 0 || bufW <= 0 || bufH <= 0) {
        return false;
    }
    m->fx = percent > 100 ? static_cast<double>(bufW) / showW : percent / 100.0;
    m->fy = percent > 100 ? static_cast<double>(bufH) / showH : percent / 100.0;
    return true;
}

bool Zoomed()
{
    ZoomMap m;
    return ZoomNow(&m);
}

int BufX(const ZoomMap &m, int x)
{
    return core::display::ZoomToBuffer(x, m.left, m.fx);
}
int BufY(const ZoomMap &m, int y)
{
    return core::display::ZoomToBuffer(y, m.top, m.fy);
}
int ScreenX(const ZoomMap &m, int x)
{
    return core::display::ZoomToScreen(x, m.left, m.fx);
}
int ScreenY(const ZoomMap &m, int y)
{
    return core::display::ZoomToScreen(y, m.top, m.fy);
}

// A conversion would see a moved origin (or a zoomed mouse) now.
bool Shifts()
{
    return g_depth == 0 && core::diag::Enabled(g_feature) && (g_px != 0 || g_py != 0 || Zoomed());
}

// Moves the origin by the remainder for one conversion call (screen-to-grid also gets a viewport one column and
// a row pair larger, unless city_render has made it larger already); mouse: the game's mouse as the buffer pixel it
// shows (R5c, mouse_to_grid). Every change goes into r.
void BeginShift(core::diag::Restore &r, bool grow, bool mouse)
{
    r.Set(&g_depth, 1);
    r.Set(kOriginX.ptr(), *kOriginX - g_px);
    r.Set(kOriginY.ptr(), *kOriginY - g_py);
    if (grow && !g_inRender) {
        r.Set(kWidthTiles.ptr(), *kWidthTiles + 1);
        r.Set(kHeightRows.ptr(), *kHeightRows + 2);
    }
    ZoomMap m;
    if (mouse && !g_bufferMode && ZoomNow(&m)) {
        r.Set(kMouseX.ptr(), BufX(m, *kMouseX));
        r.Set(kMouseY.ptr(), BufY(m, *kMouseY));
    }
}

// The city as shown on the screen (right and bottom exclusive): the clip's top left to the extent, or to the grid
// clip without one -- the same part Render shows.
struct Shown {
    int left, top, right, bottom;
};
Shown g_renderShown{}; // while city_render draws into our buffer (the viewport is larger then)

Shown ShownCity()
{
    if (g_inRender) {
        return g_renderShown;
    }
    ZoomMap m;
    if (ZoomNow(&m)) { // R5c: the viewport is the buffer's then, larger than the city on the screen
        return {m.left, m.top, m.showRight, m.showBottom};
    }
    const int left = *kClipLeft, top = *kClipTop;
    const int right = left + *kWidthTiles * (*kTileWidth + 2) - 2, bottom = top + (*kHeightRows - 1) * *kTileHalfHeight;
    const int width = *kScreenPitch / 2, height = *kDdHeight;
    return {left, top, g_extRight > right ? (g_extRight < width ? g_extRight : width) : right,
            g_extBottom > bottom ? (g_extBottom < height ? g_extBottom : height) : bottom};
}

// Screen-to-grid only takes a point inside origin + viewport (mouse_to_grid 0x469c60, screen_to_grid 0x469e50).
// With the origin moved by the remainder and the viewport a column and a row pair larger (BeginShift), that reached
// up to a column into the sidebar and up to the remainder into the top bar, so the cursor tile and clicks went
// through them to the city (playtest 2026-11-01, since M4). A point outside the city as shown is no tile (0, what the
// game returns outside its bounds, without calling it). With the game's own layout and no remainder the game decides
// as always.
bool OffShownCity(int x, int y)
{
    if (g_depth != 0 || (!Shifts() && g_extRight == 0)) {
        return false;
    }
    const Shown s = ShownCity();
    return x < s.left || x >= s.right || y < s.top || y >= s.bottom;
}

// Runs call() with the origin moved, under the guard; plainly when nothing is moved; the game's own way again when
// the guarded call was abandoned.
template <typename Call> void Converted(bool grow, Call &call, bool mouse = false)
{
    if (!Shifts()) {
        call();
        return;
    }
    struct Ctx {
        Call *call;
        bool grow, mouse;
        core::diag::Restore r;
    } ctx{&call, grow, mouse, {}};
    if (core::diag::Guarded(
            g_feature,
            [](void *p) {
                Ctx &c = *static_cast<Ctx *>(p);
                BeginShift(c.r, c.grow, c.mouse);
                (*c.call)();
                c.r.Undo(); // the origin is moved only for this call
            },
            &ctx, &ctx.r)) {
        return;
    }
    FallBack();
    call();
}

// ---------------------------------------------------------------------------------------------
// Statistics for the rate log (game thread)

double g_frameMs = 1.5;       // average main-loop iteration, updated every log window
double g_scrollingMs = 0;     // time with view_scrolling set
double g_movedX = 0, g_movedY = 0; // pixels moved (smooth) or stepped (original)
long g_steps = 0;             // original view_scroll_step calls with a direction
long g_carries = 0;           // smooth: tile changes
long g_logged = 0;
constexpr long kMoveLogs = 12;

// ---------------------------------------------------------------------------------------------
// Smooth scrolling

using VoidFn = void(__cdecl *)();

void ViewClamp()
{
    Function<VoidFn>(kViewClamp)();
}

// The largest view_x/view_y view_clamp allows right now. (The camera is in the frame's Restore list: if view_clamp
// faults here, the guard puts the camera of the frame back.)
void Limits(int *maxX, int *maxY)
{
    int sx = *kViewX, sy = *kViewY;
    *kViewX = 1 << 20;
    *kViewY = 1 << 20;
    ViewClamp();
    *maxX = *kViewX;
    *maxY = *kViewY;
    *kViewX = sx;
    *kViewY = sy;
}

void Move(int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        return;
    }
    const int colPx = *kTileWidth + 2;     // one view_x step
    const int rowPx = 2 * *kTileHalfHeight; // one view_y step of two half rows (view_y stays even)
    g_px += dx;
    g_py += dy;
    int vx = *kViewX, vy = *kViewY;
    while (g_px >= colPx) { g_px -= colPx; ++vx; }
    while (g_px < 0) { g_px += colPx; --vx; }
    while (g_py >= rowPx) { g_py -= rowPx; vy += 2; }
    while (g_py < 0) { g_py += rowPx; vy -= 2; }
    int oldX = *kViewX, oldY = *kViewY;
    bool moved = vx != oldX || vy != oldY;
    *kViewX = vx;
    *kViewY = vy;
    ViewClamp();
    if (*kViewX != vx) { g_px = 0; g_scroller.ResetX(); } // hit a map edge: stop there on a whole tile
    if (*kViewY != vy) { g_py = 0; g_scroller.ResetY(); }
    if (g_px != 0 || g_py != 0) {
        int maxX, maxY;
        Limits(&maxX, &maxY);
        // At the right/bottom limit a remainder would show what lies beyond the map.
        if (*kViewX >= maxX && g_px != 0) { g_px = 0; g_scroller.ResetX(); }
        if (*kViewY >= maxY && g_py != 0) { g_py = 0; g_scroller.ResetY(); }
    }
    if (moved) {
        // The game decays the ambient sound weights once per scroll step (one step = colPx either way, diagonals
        // included); keep that rate instead of decaying on every (half as large) vertical carry.
        int ax = std::abs(*kViewX - oldX) * colPx, ay = std::abs(*kViewY - oldY) * *kTileHalfHeight;
        g_sinceDecayPx += ax > ay ? ax : ay;
        if (g_sinceDecayPx >= colPx) {
            Function<VoidFn>(kSoundViewDecay)();
            g_sinceDecayPx = 0;
        }
        ++g_carries;
    }
    if (moved && ++g_logged <= kMoveLogs) {
        core_log("scroll: view (%d,%d) remainder (%d,%d) px", *kViewX, *kViewY, g_px, g_py);
    }
    g_lastViewX = *kViewX;
    g_lastViewY = *kViewY;
}

// A held arrow key: the game sets its flag on WM_KEYDOWN (only where it allows keyboard scrolling) and the original
// direction function clears it on every step, the key repeat setting it again. Per frame that would flicker, so a
// key counts as held from its first flag until the key is released.
struct HeldKey {
    Var<int> flag;
    int vk;
    bool held = false;
    bool Poll()
    {
        if (*flag) {
            *flag = 0;
            held = true;
        }
        if (held && !emperor::keys::Held(static_cast<unsigned>(vk))) { // a key bound to this direction (W, say)
            held = false;
        }
        return held;
    }
};
HeldKey g_left{kKeyLeft, VK_LEFT}, g_right{kKeyRight, VK_RIGHT}, g_up{kKeyUp, VK_UP}, g_down{kKeyDown, VK_DOWN};

int g_lastFrame = -1;

// R5d: the scale's factors now (buffer pixels per screen pixel; 1 at 100%) and where the city starts on the screen.
bool Factors(ZoomMap *m)
{
    if (ZoomNow(m)) {
        return true;
    }
    if (emperor::zoom::Percent() != 100 || g_extRight <= 0 || g_extBottom <= 0) {
        return false;
    }
    *m = {*kClipLeft, *kClipTop, g_extRight, g_extBottom, 1.0, 1.0};
    return true;
}

double g_zoomShiftX = 0, g_zoomShiftY = 0; // the anchor's shift not moved yet (fractions; whole tiles when not smooth)

// R5d (resolution_ui_r5.md section 11.2): the scale one step towards the wheel's target, laid out, and the view moved
// so the city point under the anchor (the cursor at the last notch) stays under it. At the map's edges view_clamp has
// the last word (the point then moves a little, as in Augustus).
void ZoomStep()
{
    ZoomMap a{}, b{};
    const bool before = Factors(&a);
    if (!emperor::zoom::Advance(g_smooth)) {
        return;
    }
    const bool after = Factors(&b);
    if (before && after) {
        int ax = 0, ay = 0;
        emperor::zoom::Anchor(&ax, &ay);
        g_zoomShiftX += core::display::ZoomAnchorShift(ax, a.left, a.fx, b.fx);
        g_zoomShiftY += core::display::ZoomAnchorShift(ay, a.top, a.fy, b.fy);
    }
    if (g_smooth) {
        const int dx = static_cast<int>(std::lround(g_zoomShiftX)), dy = static_cast<int>(std::lround(g_zoomShiftY));
        g_zoomShiftX -= dx;
        g_zoomShiftY -= dy;
        Move(dx, dy);
    }
    else { // the game's whole tiles: view_x by columns, view_y by row pairs
        const int colPx = *kTileWidth + 2, rowPx = 2 * *kTileHalfHeight;
        const int tx = static_cast<int>(std::lround(g_zoomShiftX / colPx));
        const int ty = static_cast<int>(std::lround(g_zoomShiftY / rowPx));
        g_zoomShiftX -= tx * colPx;
        g_zoomShiftY -= ty * rowPx;
        g_px = g_py = 0;
        *kViewX += tx;
        *kViewY += 2 * ty;
        ViewClamp();
    }
    if (!emperor::zoom::Moving()) {
        g_zoomShiftX = g_zoomShiftY = 0;
    }
    g_lastViewX = *kViewX; // also when only the layout's clamp moved it: not "someone else" (SyncView)
    g_lastViewY = *kViewY;
    // viewport_init (the relayout) set the drawn view's copy to the map's middle; mouse_to_grid looks tiles up from
    // it until city_render writes it again, so the cursor tile (the building preview) jumped meanwhile (R5d's first
    // test). The view as city_render will draw it.
    *kViewDrawnX = *kViewX;
    *kViewDrawnY = *kViewY;
}

// The edge scrolling and arrow keys of one frame.
void SmoothFrame()
{
    int x = *kCursorX, y = *kCursorY;
    const bool keyLeft = g_left.Poll(), keyRight = g_right.Poll(), keyUp = g_up.Poll(), keyDown = g_down.Poll();
    bool left = keyLeft || x < 1;
    bool right = keyRight || x >= *kDesktopW - 1;
    bool up = keyUp || y < 1;
    bool down = keyDown || y >= *kDesktopH - 1;
    int dirX = (right ? 1 : 0) - (left ? 1 : 0);
    int dirY = (down ? 1 : 0) - (up ? 1 : 0);
    *kViewScrolling = (left || right || up || down) ? 1 : 0; // as scroll_get_direction: the simulation pauses
    int idx = (100 - *kScrollSpeed) / 10;
    if (idx < 0) idx = 0;
    if (idx >= 10) dirX = dirY = 0; // the game does not scroll at all then
    double stepMs = idx * 12 + 2 + g_frameMs / 2;
    double vx = (*kTileWidth + 2) / stepMs, vy = (4 * *kTileHalfHeight) / stepMs;
    ZoomMap m;
    if (ZoomNow(&m)) { // R5c: the camera moves in buffer pixels; on the screen as fast as at 100%
        vx *= m.fx;
        vy *= m.fy;
    }
    int dx = 0, dy = 0;
    g_scroller.Update(dirX, dirY, vx, vy, &dx, &dy);
    g_movedX += std::abs(dx);
    g_movedY += std::abs(dy);
    Move(dx, dy);
}

// Once per game_frame, before the city is drawn: the wheel's zoom step (R5d), then smooth scrolling. r: the camera,
// view_scrolling and the key flags as they were before the frame (put back if the frame is abandoned; kept when it
// completes).
void Frame(core::diag::Restore &r)
{
    if (*kFrameCounter == g_lastFrame) {
        return;
    }
    g_lastFrame = *kFrameCounter;
    r.Save(kViewX.ptr());
    r.Save(kViewY.ptr());
    r.Save(kViewScrolling.ptr());
    r.Save(kKeyLeft.ptr());
    r.Save(kKeyRight.ptr());
    r.Save(kKeyUp.ptr());
    r.Save(kKeyDown.ptr());
    SyncView();
    if (emperor::zoom::Moving()) {
        ZoomStep();
    }
    if (g_smooth) {
        SmoothFrame();
    }
}

// ---------------------------------------------------------------------------------------------
// Hooks

using CityRender_fn = void(__cdecl *)(int, int, int);
using ScreenToGrid_fn = int(__cdecl *)(int, int);
using MouseToGrid_fn = int(__cdecl *)(int);
using ScreenToGridRaw_fn = int(__cdecl *)(int, int, int);
using GridToScreen_fn = void(__cdecl *)(int, int, int *, int *);
using GetDirection_fn = int(__cdecl *)();
using ScrollStep_fn = void(__cdecl *)(int);
using Drag_fn = void(__fastcall *)(int *);
CityRender_fn o_CityRender;
ScreenToGrid_fn o_ScreenToGrid;
MouseToGrid_fn o_MouseToGrid;
ScreenToGridRaw_fn o_ScreenToGridRaw;
GridToScreen_fn o_GridToScreen;
GetDirection_fn o_GetDirection;
ScrollStep_fn o_ScrollStep;
Drag_fn o_DragStart, o_DragDraw, o_DragFinish;

// Our city buffer: same pitch as the game's back buffer, one row pair (and a little) taller.
BYTE *g_city = nullptr;
size_t g_citySize = 0;
int g_cityPitch = 0, g_cityRows = 0; // as last drawn (for a report)
bool g_loggedRender = false;

bool CityBuffer(size_t size)
{
    if (size <= g_citySize) {
        return true;
    }
    if (g_city) {
        VirtualFree(g_city, 0, MEM_RELEASE);
    }
    g_city = static_cast<BYTE *>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_citySize = g_city ? size : 0;
    return g_city != nullptr;
}

struct RenderCall {
    int vx, vy, orientation;
    bool plain; // nothing to move: the game draws as it always does (outside the guard)
    core::diag::Restore r;
};

// Where the time goes (R2b.1-3, core/diag/perf.h): the game drawing the city (city_render's own work) and our copies
// to and from the city buffer.
const int kCityDrawPhase = core::diag::perf::Register("city drawn by the game", core::diag::perf::Thread::Game);
const int kCityCopyPhase = core::diag::perf::Register("city buffer copies (ours)", core::diag::perf::Thread::Game);
const int kCityZoomPhase = core::diag::perf::Register("city zoom scaling (ours)", core::diag::perf::Thread::Game);

void DrawCity(int vx, int vy, int orientation)
{
    core::diag::perf::Scope timed(kCityDrawPhase);
    Original(o_CityRender, kCityRender)(vx, vy, orientation);
}

bool g_loggedExtent = false;

// The game draws as always, straight into the back buffer; with the grid clip when the clip holds the extent.
void DrawPlain(RenderCall &c, bool offGrid, int right, int bottom)
{
    if (!offGrid) {
        c.plain = true;
        return;
    }
    c.r.Set(kClipRight.ptr(), right);
    c.r.Set(kClipBottom.ptr(), bottom);
    DrawCity(c.vx, c.vy, c.orientation);
    c.r.Undo();
}

// R5 (docs/archive/resolution_ui_r5.md section 8.7): the game's view is the zoomed one (games/emperor/zoom via
// games/emperor/resolution), bufW x bufH pixels of city for the showW x showH pixels on the screen. The game draws it
// into our buffer, on the tile grid as always, with one more column and two more half rows for the remainder; while
// it draws, draw_buffer, screen_pitch, dd_width and dd_height are our buffer's (the blitters address rows with
// screen_pitch, and image_draw_core / image_clip_x cull against dd_width), and all are put back after. The visible
// part (from the remainder) is then shrunk (average or nearest) or enlarged (sharp bilinear) onto the screen.
// Before drawing, the view is checked against the map's range again (zoom::Recheck); a view that does not fit is not
// drawn at all (never larger than the map: resolution_ui_r5.md section 8.2).
int g_zoomPitchPx = 0, g_zoomRows = 0; // the buffer's last layout (cleared when it changes)
int g_zoomRight2 = 0, g_zoomBottom2 = 0;
bool g_loggedZoomSkip = false;

bool RenderZoomed(RenderCall &c)
{
    emperor::zoom::Recheck();
    const int percent = emperor::zoom::Percent();
    if (percent == 100 || g_extRight <= 0 || g_extBottom <= 0) {
        return false; // not zoomed: the usual drawing
    }
    BYTE *back = *kDrawBuffer;
    const int pitch = *kScreenPitch, width = pitch / 2, height = *kDdHeight;
    const int halfH = *kTileHalfHeight, colPx = *kTileWidth + 2, rowPx = 2 * halfH;
    const int left = *kClipLeft, top = *kClipTop;
    const int cols = *kWidthTiles, rows = *kHeightRows;
    const int showRight = g_extRight < width ? g_extRight : width;
    const int showBottom = g_extBottom < height ? g_extBottom : height;
    const int showW = showRight - left, showH = showBottom - top;
    int bufW = 0, bufH = 0;
    emperor::zoom::BufferSize(&bufW, &bufH);
    const int right2 = left + (cols + 1) * colPx - 2, bottom2 = top + (rows + 1) * halfH;
    const int px = g_px, py = g_py;
    const bool fits = emperor::zoom::ViewFits(cols, rows);
    if (!back || showW <= 0 || showH <= 0 || bufW <= 0 || bufH <= 0 || halfH <= 0 || colPx <= 2 || left < 0 ||
        top < 0 || !fits || px + bufW > right2 - left || py + bufH > bottom2 - top) {
        if (!g_loggedZoomSkip) {
            g_loggedZoomSkip = true;
            core_log("zoom: city not drawn this frame (%s; view %d x %d, buffer part %d x %d, remainder %d,%d)",
                     fits ? "the layout does not add up" : "the view is larger than the map's range", cols, rows,
                     bufW, bufH, px, py);
        }
        c.plain = false;
        return true; // nothing drawn (the last frame's city stays): never drawn larger than the map
    }
    int pitchPx = (right2 + 2 + 15) & ~15;
    int totalRows = bottom2 + 2 * rowPx; // spare rows below the clip, as the back buffer has
    // R5d: while the wheel moves the scale, the layout changes every frame. The buffer then keeps the largest scale's
    // pitch and rows, so it is not cleared whole every frame (36 MB at 150%); only what a larger layout adds is
    // cleared. When the movement ends the layout is the exact one again and the buffer is cleared once.
    const bool moving = emperor::zoom::Moving();
    if (moving) {
        const int capW = left + core::display::ZoomedSize(showW, emperor::zoom::Cap()) + 3 * colPx;
        const int capRows = top + core::display::ZoomedSize(showH, emperor::zoom::Cap()) + 8 * halfH;
        pitchPx = pitchPx > ((capW + 15) & ~15) ? pitchPx : (capW + 15) & ~15;
        pitchPx = pitchPx > g_zoomPitchPx ? pitchPx : g_zoomPitchPx;
        totalRows = totalRows > capRows ? totalRows : capRows;
        totalRows = totalRows > g_zoomRows ? totalRows : g_zoomRows;
    }
    const size_t size = static_cast<size_t>(pitchPx) * 2 * totalRows;
    if (!CityBuffer(size)) {
        char why[96];
        std::snprintf(why, sizeof(why), "no memory for a %.1f MB city buffer", size / (1024.0 * 1024.0));
        emperor::zoom::Off(why); // laid out at 100 now; this frame is drawn the usual way below
        return false;
    }
    const bool newLayout = pitchPx != g_zoomPitchPx || totalRows != g_zoomRows || right2 != g_zoomRight2 ||
                           bottom2 != g_zoomBottom2;
    if (pitchPx != g_zoomPitchPx || (newLayout && !moving)) {
        std::memset(g_city, 0, size); // a new layout: no rows of the last one left around the map
        if (!moving) {
            core_log("zoom: city buffer %d x %d px (%.1f MB) for %d%%: view %d x %d (+1, +2 while drawing), clip "
                     "(%d,%d)-(%d,%d), shown %d x %d of it on %d x %d px",
                     pitchPx, totalRows, size / (1024.0 * 1024.0), percent, cols, rows, left, top, right2, bottom2,
                     bufW, bufH, showW, showH);
        }
    }
    else if (newLayout) { // moving: only the columns and rows the larger view adds
        WORD *rowsAt = reinterpret_cast<WORD *>(g_city);
        if (right2 > g_zoomRight2) {
            const int from = g_zoomRight2 > 0 ? g_zoomRight2 : 0, to = right2 + 2 < pitchPx ? right2 + 2 : pitchPx;
            for (int y = 0; y < totalRows && from < to; ++y) {
                std::memset(rowsAt + static_cast<size_t>(y) * pitchPx + from, 0, (to - from) * 2);
            }
        }
        if (bottom2 > g_zoomBottom2) {
            const int from = g_zoomBottom2 > 0 ? g_zoomBottom2 : 0;
            if (from < totalRows) {
                std::memset(rowsAt + static_cast<size_t>(from) * pitchPx, 0,
                            static_cast<size_t>(totalRows - from) * pitchPx * 2);
            }
        }
    }
    g_zoomPitchPx = pitchPx;
    g_zoomRows = totalRows;
    g_zoomRight2 = right2;
    g_zoomBottom2 = bottom2;
    g_cityPitch = pitchPx * 2;
    g_cityRows = totalRows;
    c.r.Set(kDrawBuffer.ptr(), g_city);
    c.r.Set(kScreenPitch.ptr(), pitchPx * 2);
    c.r.Set(kDdWidth.ptr(), pitchPx);
    c.r.Set(kDdHeight.ptr(), totalRows);
    c.r.Set(kClipRight.ptr(), right2);
    c.r.Set(kClipBottom.ptr(), bottom2);
    c.r.Set(kWidthTiles.ptr(), cols + 1);
    c.r.Set(kHeightRows.ptr(), rows + 2);
    g_renderShown = {left, top, showRight, showBottom}; // on the screen (conversions take screen positions)
    // Conversions made while the game draws (their input is a screen position) use the drawing's zoom.
    g_map = {left, top, showRight, showBottom, percent > 100 ? static_cast<double>(bufW) / showW : percent / 100.0,
             percent > 100 ? static_cast<double>(bufH) / showH : percent / 100.0};
    c.r.Set(&g_inRender, true);
    DrawCity(c.vx, c.vy, c.orientation);
    c.r.Undo();
    g_map.fx = 0;
    const std::int64_t start = core::diag::perf::Now();
    const WORD *city = reinterpret_cast<const WORD *>(g_city);
    const core::display::Pixels16 src{city + static_cast<size_t>(top + py) * pitchPx + left + px, pitchPx, bufW,
                                      bufH};
    WORD *screen = reinterpret_cast<WORD *>(back);
    if (percent > 100) {
        const core::display::Target16 dst{screen + static_cast<size_t>(top) * width + left, width, showW, showH};
        core::display::ShrinkRgb565(src, showW, showH, emperor::zoom::AverageShrink(), dst,
                                    &core::platform::parallel::ForRows);
    }
    else {
        const core::display::Target16 dst{screen, width, width, height};
        const RECT clip{left, top, showRight, showBottom};
        core::display::ScaleRgb565(src, 100.0 / percent, true, dst, left, top, clip,
                                   &core::platform::parallel::ForRows);
    }
    core::diag::perf::Add(kCityZoomPhase, core::diag::perf::Now() - start);
    return true;
}

// city_render with the pixel remainder (see the top of this file) and the extent (R2b.1). While the game draws,
// draw_buffer, the clip's right and bottom and the viewport size are ours (all in c.r, put back after the draw or
// when it is abandoned). The clip stays on the tile grid (whole columns, whole half rows): the game's quick tile
// drawing only handles tiles cut in half (m4_smooth_scroll.md §8). The grid clip is worked out from the viewport as
// viewport_init does (right = left + cols x (tile width + 2) - 2, bottom = top + (rows - 1) x half height), not
// read from the clip itself: while the city message panel is laid out (games/emperor/resolution, 0x50bf10) the
// clip holds the extent, which is not on the grid.
void Render(RenderCall &c)
{
    SyncView();
    if (RenderZoomed(c)) {
        return;
    }
    BYTE *back = *kDrawBuffer;
    const int pitch = *kScreenPitch, width = pitch / 2, height = *kDdHeight;
    const int halfH = *kTileHalfHeight, colPx = *kTileWidth + 2, rowPx = 2 * halfH;
    const int left = *kClipLeft, top = *kClipTop;
    const int right = left + *kWidthTiles * colPx - 2, bottom = top + (*kHeightRows - 1) * halfH;
    const bool offGrid = *kClipRight != right || *kClipBottom != bottom; // the extent is in the clip just now
    const int px = g_px, py = g_py;
    // The part to show: up to the extent, never past the screen.
    const int showRight = g_extRight > right ? (g_extRight < width ? g_extRight : width) : right;
    const int showBottom = g_extBottom > bottom ? (g_extBottom < height ? g_extBottom : height) : bottom;
    const int needR = showRight - right, needB = showBottom - bottom;
    // Our buffer: as many more columns / row pairs as the offset and the extent need.
    const int moreCols = (px + needR + colPx - 1) / colPx, moreRows = (py + needB + rowPx - 1) / rowPx;
    const int right2 = right + (moreCols > 0 ? moreCols : 1) * colPx;
    const int bottom2 = bottom + (moreRows > 0 ? moreRows : 1) * rowPx;
    const size_t size = static_cast<size_t>(pitch) * (bottom2 + 2);
    bool usable = back && pitch >= *kDdWidth * 2 && 0 <= left && left < right && 0 <= top && top < bottom &&
                  bottom <= height && right2 <= width && halfH > 0 && colPx > 2;
    if (!g_loggedRender) {
        g_loggedRender = true;
        core_log("scroll: city view clip (%d,%d)-(%d,%d), tile step %d x %d px, back buffer %d x %d pitch %d%s", left,
                 top, right, bottom, colPx, rowPx, *kDdWidth, height, pitch,
                 usable ? "" : "; no room for the pixel offset: the view is drawn on whole tiles");
    }
    if ((needR > 0 || needB > 0) && !g_loggedExtent) {
        g_loggedExtent = true;
        core_log("scroll: the city is shown past its clip to (%d,%d): %d px on the right (the gap to the sidebar), %d "
                 "rows at the bottom%s",
                 showRight, showBottom, needR, needB, usable ? "" : "; not possible here (no room): the clip only");
    }
    if (!usable) {
        DrawPlain(c, offGrid, right, bottom);
        return;
    }
    if (px == 0 && py == 0 && needR == 0) {
        if (needB == 0) {
            DrawPlain(c, offGrid, right, bottom); // nothing to show past the clip
            return;
        }
        // Only the bottom: the clip down to the next half row past the screen's bottom, straight into the back
        // buffer; the rows below the screen land in its spare rows (never shown).
        const int moreHalf = (needB + halfH - 1) / halfH;
        if (bottom + moreHalf * halfH - height > core::ddraw::kSpareRows) {
            DrawPlain(c, offGrid, right, bottom);
            return;
        }
        c.r.Set(kClipRight.ptr(), right);
        c.r.Set(kClipBottom.ptr(), bottom + moreHalf * halfH);
        c.r.Set(kHeightRows.ptr(), *kHeightRows + moreHalf);
        DrawCity(c.vx, c.vy, c.orientation);
        c.r.Undo();
        return;
    }
    if (!CityBuffer(size)) {
        DrawPlain(c, offGrid, right, bottom);
        return;
    }
    g_zoomPitchPx = 0; // the buffer holds the screen's layout now: a zoomed frame clears it again
    // Start from what the back buffer holds there, as the game would draw over it.
    std::int64_t copyStart = core::diag::perf::Now();
    for (int y = top; y < bottom2; ++y) {
        BYTE *dst = g_city + static_cast<size_t>(y) * pitch + left * 2;
        if (y < height) {
            std::memcpy(dst, back + static_cast<size_t>(y) * pitch + left * 2, (right2 - left) * 2);
        }
        else {
            std::memset(dst, 0, (right2 - left) * 2);
        }
    }
    core::diag::perf::Add(kCityCopyPhase, core::diag::perf::Now() - copyStart);
    g_cityPitch = pitch;
    g_cityRows = bottom2;
    c.r.Set(kDrawBuffer.ptr(), g_city);
    c.r.Set(kClipRight.ptr(), right2);
    c.r.Set(kClipBottom.ptr(), bottom2);
    c.r.Set(kWidthTiles.ptr(), *kWidthTiles + (right2 - right) / colPx);
    c.r.Set(kHeightRows.ptr(), *kHeightRows + 2 * ((bottom2 - bottom) / rowPx));
    g_renderShown = {left, top, showRight, showBottom};
    c.r.Set(&g_inRender, true);
    DrawCity(c.vx, c.vy, c.orientation);
    c.r.Undo();
    copyStart = core::diag::perf::Now();
    for (int y = top; y < showBottom; ++y) {
        std::memcpy(back + static_cast<size_t>(y) * pitch + left * 2,
                    g_city + static_cast<size_t>(y + py) * pitch + (left + px) * 2, (showRight - left) * 2);
    }
    core::diag::perf::Add(kCityCopyPhase, core::diag::perf::Now() - copyStart);
}

CityDrawn g_cityDrawn = nullptr;

// The city as shown, right after it was drawn (before the sidebar and the rest of the interface): on the back
// buffer from the clip's top left to the extent (or the clip's right and bottom without one).
void AfterRender()
{
    if (!g_cityDrawn) {
        return;
    }
    const BYTE *back = *kDrawBuffer;
    const int pitch = *kScreenPitch, width = pitch / 2, height = *kDdHeight;
    const bool zoomed = emperor::zoom::Percent() != 100 && g_extRight > 0 && g_extBottom > 0; // clip = our buffer's
    const int right = zoomed || g_extRight > *kClipRight ? (g_extRight < width ? g_extRight : width) : *kClipRight;
    const int bottom =
        zoomed || g_extBottom > *kClipBottom ? (g_extBottom < height ? g_extBottom : height) : *kClipBottom;
    if (back && *kClipLeft >= 0 && *kClipTop >= 0 && *kClipLeft < right && *kClipTop < bottom && right <= width &&
        bottom <= height) {
        g_cityDrawn(back, pitch, *kClipLeft, *kClipTop, right, bottom);
    }
}

AfterCity g_afterCity = nullptr;

void __cdecl CityRender_detour(int vx, int vy, int orientation)
{
    RenderCall c{vx, vy, orientation, false, {}};
    if (core::diag::Guarded(g_feature, [](void *p) { Render(*static_cast<RenderCall *>(p)); }, &c, &c.r)) {
        if (!c.plain) {
            AfterRender();
            if (g_afterCity) {
                g_afterCity(orientation);
            }
            return;
        }
    }
    else {
        FallBack(); // abandoned (or off): drawn again on whole tiles
    }
    DrawCity(vx, vy, orientation);
    AfterRender();
    if (g_afterCity) {
        g_afterCity(orientation);
    }
}

// R5c: a screen position as the zoomed buffer pixel it shows (unchanged at 100% and in the drag's buffer mode).
void ToBuffer(int *x, int *y)
{
    ZoomMap m;
    if (g_depth == 0 && !g_bufferMode && ZoomNow(&m)) {
        *x = BufX(m, *x);
        *y = BufY(m, *y);
    }
}

int __cdecl ScreenToGrid_detour(int x, int y)
{
    if (OffShownCity(x, y)) {
        return 0;
    }
    ToBuffer(&x, &y);
    int r = 0;
    auto call = [&] { r = Original(o_ScreenToGrid, kScreenToGrid)(x, y); };
    Converted(true, call);
    return r;
}

// R5e: during a military drag selection the cursor's tile (0x405500, from mouse_to_grid) is the frame's moving corner.
// Edge scrolling puts the cursor on the sidebar or the top bar, where there is no tile (0): the corner then lies far
// away, drag_select_box_valid 0x44c120 says no and the frame is not drawn until the cursor is back on the city (the
// original does the same; R5e's second test). While the drag lasts, the mouse is kept to the city as shown for
// mouse_to_grid, so the corner stays at the city's edge and the frame stays (scrolling reads the raw cursor).
// A drag is a frame only from the city, and once the cursor is more than kDragPx away from where it was pressed on
// both axes (R5e's fifth test): a press on the sidebar (the game starts a drag for any press that hits no soldier,
// 0x54b496) or a small drag stays a click as in the original -- released, a click with no frame orders the selected
// company to the cursor's tile (0x407300), none off the city. The game's own threshold is 4 (0x44c310: frame wider
// and taller than 2), which with tile-snapped corners meant "into another tile".
constexpr int kDragPx = 8;
bool g_dragOnCity = false;
int g_dragStartX = 0, g_dragStartY = 0; // the press, on the screen

bool DragMoved(int x, int y)
{
    return g_dragOnCity && std::abs(x - g_dragStartX) > kDragPx && std::abs(y - g_dragStartY) > kDragPx;
}

bool DragClampMouse()
{
    if (*kMouseMode != kMouseModeDragSelect || g_depth != 0 || g_inRender) {
        return false;
    }
    const Shown s = ShownCity();
    const int x = *kMouseX, y = *kMouseY;
    const int cx = x < s.left ? s.left : x >= s.right ? s.right - 1 : x;
    const int cy = y < s.top ? s.top : y >= s.bottom ? s.bottom - 1 : y;
    if (!DragMoved(cx, cy)) {
        return false; // not a frame (yet): the game's own tile (none off the city)
    }
    if (cx == x && cy == y) {
        return false;
    }
    *kMouseX = cx;
    *kMouseY = cy;
    return true;
}

int __cdecl MouseToGrid_detour(int unused)
{
    const int mouseX = *kMouseX, mouseY = *kMouseY;
    const bool clamped = core::diag::Enabled(g_feature) && DragClampMouse();
    int r = 0;
    if (!OffShownCity(*kMouseX, *kMouseY)) {
        auto call = [&] { r = Original(o_MouseToGrid, kMouseToGrid)(unused); };
        Converted(true, call, true); // the mouse as the buffer pixel it shows while zoomed
    }
    if (clamped) {
        *kMouseX = mouseX;
        *kMouseY = mouseY;
    }
    return r;
}

int __cdecl ScreenToGridRaw_detour(int x, int y, int unused)
{
    ToBuffer(&x, &y);
    int r = 0;
    auto call = [&] { r = Original(o_ScreenToGridRaw, kScreenToGridRaw)(x, y, unused); };
    Converted(true, call);
    return r;
}

// R5e: the drag frame's corners follow the cursor pixel by pixel. drag_select_start / drag_select_draw take a corner
// from grid_to_screen of the cursor's tile (0x101d0b8 / 0x101d0bc) less the tile's height x 40 (0x44bb38.., 0x44bbc4..):
// the frame then jumps by tiles (a column sideways is also half a row up or down) and an edge tile's position can lie
// off the city, so the frame is dropped at the left and top edges (drag_select_box_valid 0x44c120; R5e's third test).
// For those two calls the result is the cursor (kept to the city as shown, in the game's coordinates: the buffer pixel
// it shows while zoomed) plus the height the game takes off again.
constexpr DWORD kDragStartCorner = 0x44bb29; // return addresses of their grid_to_screen calls
constexpr DWORD kDragDrawCorner = 0x44bbb5;

bool DragCorner(int *sx, int *sy)
{
    if (*kMouseMode != kMouseModeDragSelect || !sx || !sy || g_inRender || !g_dragOnCity) {
        return false; // a drag from the sidebar or the top bar: the game's own (no frame, as in the original)
    }
    // While drag_select_draw runs zoomed, dd_height is the buffer's (DragBody): the city as shown comes from the
    // drag's own map, taken before (with ShownCity the corner stopped at the buffer's height below 100%: R5e's fourth
    // test).
    ZoomMap m;
    const bool zoomed = g_dragLines ? (m = g_dragMap, true) : ZoomNow(&m);
    const Shown s = zoomed ? Shown{m.left, m.top, m.showRight, m.showBottom} : ShownCity();
    // 2 px inside on the left and the top, as the frame's right and bottom lines are (they are drawn 1 px inside
    // their corner): the game leaves out a side at 0 or less (0x44bccd), and at the top bar's edge the line hides.
    constexpr int kInside = 2;
    int x = *kMouseX, y = *kMouseY;
    x = x < s.left + kInside ? s.left + kInside : x >= s.right ? s.right - 1 : x;
    y = y < s.top + kInside ? s.top + kInside : y >= s.bottom ? s.bottom - 1 : y;
    if (zoomed) {
        x = BufX(m, x);
        y = BufY(m, y);
    }
    const int tile = *kCursorTile;
    const int height = tile > 0 && tile < kMapTiles ? kTileHeights.ptr()[tile] : 0;
    *sx = x;
    *sy = y + 40 * height;
    return true;
}

void __cdecl GridToScreen_detour(int gx, int gy, int *sx, int *sy)
{
    auto call = [&] { Original(o_GridToScreen, kGridToScreen)(gx, gy, sx, sy); };
    const DWORD from = GameAddress(__builtin_return_address(0));
    if ((from == kDragStartCorner || from == kDragDrawCorner) && core::diag::Enabled(g_feature)) {
        call(); // as the game does (it only writes *sx, *sy)
        if (DragCorner(sx, sy)) {
            return;
        }
    }
    if (g_inRender) {
        call(); // a position in our (unshifted) city buffer
        return;
    }
    const bool outer = g_depth == 0;
    Converted(false, call);
    ZoomMap m;
    if (outer && !g_bufferMode && sx && sy && ZoomNow(&m)) { // R5c: back to the screen
        *sx = ScreenX(m, *sx);
        *sy = ScreenY(m, *sy);
    }
}

// R5c: the drag selection's frame, worked out in buffer pixels (buffer mode), drawn on the screen.
using Line_fn = void(__cdecl *)(int, int, int, int, int);
Line_fn o_Line;
int g_dragScreenH = 0; // dd_height as the screen's while drag_select_draw has it as buffer pixels

void __cdecl Line_detour(int x1, int y1, int x2, int y2, int colour)
{
    if (!g_dragLines) {
        Original(o_Line, kLineDraw)(x1, y1, x2, y2, colour);
        return;
    }
    x1 = ScreenX(g_dragMap, x1);
    x2 = ScreenX(g_dragMap, x2);
    y1 = ScreenY(g_dragMap, y1);
    y2 = ScreenY(g_dragMap, y2);
    // line_draw's pixels (0x5292a0) are dropped at dd_width / dd_height: the screen's for screen positions (R5e's
    // first test: at 50% the frame's lower part was cut off at the buffer's height). Put back by DragBody's Restore
    // too, if this faults.
    const int bufferH = *kDdHeight;
    *kDdHeight = g_dragScreenH;
    Original(o_Line, kLineDraw)(x1, y1, x2, y2, colour);
    *kDdHeight = bufferH;
}

// R5e: the "Game paused" banner is centred on the city's width: the city as shown, up to the sidebar. The game's is
// whole columns (2320 of 2334 px at 2560x1440: 7 px left of the middle), zoomed the buffer's (R5e's second test).
// The game's own layouts (no extent) keep the game's width.
using CityWidth_fn = int(__cdecl *)();
CityWidth_fn o_CityWidth;

int __cdecl CityWidth_detour()
{
    if (!g_inRender && g_depth == 0 && !g_bufferMode && g_extRight > 0 && core::diag::Enabled(g_feature)) {
        const Shown s = ShownCity();
        return s.right - s.left;
    }
    return Original(o_CityWidth, kCityWidth)();
}

bool IsCityScrollCall(void *ret)
{
    DWORD r = GameAddress(ret);
    for (DWORD c : kCityScrollCalls) {
        if (r == c) {
            return true;
        }
    }
    return false;
}

int __cdecl GetDirection_detour()
{
    if ((g_smooth || emperor::zoom::Moving()) && core::diag::Enabled(g_feature) &&
        IsCityScrollCall(__builtin_return_address(0))) {
        core::diag::Restore r;
        if (core::diag::Guarded(g_feature, [](void *p) { Frame(*static_cast<core::diag::Restore *>(p)); }, &r, &r)) {
            if (g_smooth) {
                return 8; // view_scroll_step(8) does nothing: the camera has been moved already
            }
        }
        else {
            FallBack();
        }
    }
    return Original(o_GetDirection, kScrollGetDirection)();
}

// Counting only (the rate log); nothing here can fault, so it is not guarded.
void __cdecl ScrollStep_detour(int dir)
{
    if (dir >= 0 && dir < 8) {
        ++g_steps;
        int colPx = *kTileWidth + 2, rowPx = 4 * *kTileHalfHeight;
        if (dir != 0 && dir != 4) g_movedX += colPx; // 1,2,3 right; 5,6,7 left
        if (dir != 2 && dir != 6) g_movedY += rowPx; // 7,0,1 up; 3,4,5 down
    }
    Original(o_ScrollStep, kViewScrollStep)(dir);
}

// Military drag selection: the start corner is kept in screen pixels plus the view_x/view_y of that moment, and
// moved by whole tiles when the view scrolls during the drag. With a pixel remainder the start corner must also move
// by the change of the remainder since the drag started.
//
// R5c, zoomed: the three functions work in buffer pixels (the corners come from grid_to_screen, less the ground's
// height x 40; the start moves by whole tiles x tile_width; the selection steps through the frame by tile_width and
// asks screen_to_grid_unbounded) -- buffer mode, so none of their positions is converted to the screen and back.
// Only the frame drawn (drag_select_draw: line_draw) goes to the screen, with sidebar_x and dd_height, its limits,
// as buffer pixels meanwhile (resolution_ui_r5.md section 10.2, point 4).
struct DragCall {
    int *drag;
    Drag_fn original;
    bool draw, start;
    core::diag::Restore r;
};

void DragBody(DragCall &c)
{
    c.r.Set(&g_bufferMode, true);
    // The start corner follows the view by (view_x moved) x tile_width (0x44bc3c), but a column is tile_width + 2: 2 px
    // per column scrolled (R5e) besides the remainder's change.
    const int dx = g_px - g_dragPx0 + (c.start ? 0 : 2 * (*kViewX - c.drag[0])), dy = g_py - g_dragPy0;
    if (!c.start && (dx != 0 || dy != 0)) {
        c.r.Set(&c.drag[2], c.drag[2] - dx); // the start corner, for this call
        c.r.Set(&c.drag[3], c.drag[3] - dy);
    }
    ZoomMap m;
    if (c.draw && ZoomNow(&m)) {
        g_dragMap = m;
        g_dragScreenH = *kDdHeight;
        c.r.Set(kSidebarX.ptr(), BufX(m, *kSidebarX));
        c.r.Set(kDdHeight.ptr(), BufY(m, *kDdHeight));
        c.r.Set(&g_dragLines, true);
    }
    c.original(c.drag);
    c.r.Undo();
}

bool DragPlain(const int *drag)
{
    return !core::diag::Enabled(g_feature) ||
           (g_px == g_dragPx0 && g_py == g_dragPy0 && *kViewX == drag[0] && !Zoomed());
}

void __fastcall DragStart_detour(int *drag)
{
    const Drag_fn original = Original(o_DragStart, kDragStart);
    // R5e: where the drag was pressed (the frame follows the cursor only from the city), before the game takes its
    // corner (grid_to_screen, DragCorner).
    g_dragStartX = *kMouseX;
    g_dragStartY = *kMouseY;
    g_dragOnCity = false;
    if (core::diag::Enabled(g_feature) && !g_inRender) {
        const Shown s = ShownCity();
        g_dragOnCity = g_dragStartX >= s.left && g_dragStartX < s.right && g_dragStartY >= s.top &&
                       g_dragStartY < s.bottom;
    }
    if (!core::diag::Enabled(g_feature) || !Zoomed()) {
        original(drag);
    }
    else {
        DragCall c{drag, original, false, true, {}};
        if (!core::diag::Guarded(g_feature, [](void *p) { DragBody(*static_cast<DragCall *>(p)); }, &c, &c.r)) {
            FallBack();
            original(drag);
        }
    }
    g_dragPx0 = g_px; // only remembered (nothing that can fault)
    g_dragPy0 = g_py;
    // The smallest frame (drag_select_box_valid: wider and taller than this - 2, in the game's pixels -- the buffer's
    // while zoomed): kDragPx on the screen. Written for every drag (the game sets 4 once, 0x44c310); the release
    // decides frame or click with it (0x5380be, 0x54b4fb).
    if (g_dragOnCity && core::diag::Enabled(g_feature)) {
        ZoomMap m;
        const bool zoomed = ZoomNow(&m);
        drag[11] = static_cast<int>(kDragPx * (zoomed ? m.fx : 1.0) + 0.999) + 2; // +0x2c
        drag[12] = static_cast<int>(kDragPx * (zoomed ? m.fy : 1.0) + 0.999) + 2; // +0x30
    }
    else {
        drag[11] = drag[12] = 4; // the game's own
    }
}

template <Drag_fn *Trampoline, const Site *S, bool Draw> void __fastcall DragAdjusted(int *drag)
{
    const Drag_fn original = Original(*Trampoline, *S);
    if (DragPlain(drag)) {
        original(drag);
        return;
    }
    DragCall c{drag, original, Draw, false, {}};
    if (core::diag::Guarded(g_feature, [](void *p) { DragBody(*static_cast<DragCall *>(p)); }, &c, &c.r)) {
        return;
    }
    FallBack();
    original(drag);
}

const Detour kDetours[] = {
    {&kCityRender, reinterpret_cast<void *>(&CityRender_detour)},
    {&kScreenToGrid, reinterpret_cast<void *>(&ScreenToGrid_detour)},
    {&kMouseToGrid, reinterpret_cast<void *>(&MouseToGrid_detour)},
    {&kScreenToGridRaw, reinterpret_cast<void *>(&ScreenToGridRaw_detour)},
    {&kGridToScreen, reinterpret_cast<void *>(&GridToScreen_detour)},
    {&kScrollGetDirection, reinterpret_cast<void *>(&GetDirection_detour)},
    {&kViewScrollStep, reinterpret_cast<void *>(&ScrollStep_detour)},
    {&kDragStart, reinterpret_cast<void *>(&DragStart_detour)},
    {&kDragDraw, reinterpret_cast<void *>(&DragAdjusted<&o_DragDraw, &kDragDraw, true>)},
    {&kDragFinish, reinterpret_cast<void *>(&DragAdjusted<&o_DragFinish, &kDragFinish, false>)},
    {&kLineDraw, reinterpret_cast<void *>(&Line_detour)},
    {&kCityWidth, reinterpret_cast<void *>(&CityWidth_detour)},
};

} // namespace

bool Attach(bool smooth)
{
    const Site *sites[] = {&kCityRender, &kScreenToGrid, &kMouseToGrid, &kScreenToGridRaw, &kGridToScreen,
                           &kScrollGetDirection, &kViewScrollStep, &kDragStart, &kDragDraw, &kDragFinish,
                           &kViewClamp, &kSoundViewDecay, &kLineDraw, &kCityWidth};
    for (const Site *s : sites) {
        if (!Matches(*s)) {
            core_log("emperor: %s at 0x%lx does not match; smooth scrolling not installed", s->name, s->address);
            return false;
        }
    }
    for (DWORD ret : kCityScrollCalls) {
        const BYTE *call = reinterpret_cast<const BYTE *>(ret - 5);
        DWORD target = ret + *reinterpret_cast<const DWORD *>(call + 1);
        if (call[0] != 0xe8 || target != kScrollGetDirection.address) {
            core_log("emperor: city scroll call before 0x%lx is not a call of scroll_get_direction; smooth scrolling "
                     "not installed", ret);
            return false;
        }
    }
    Install(kViewScrollStep, &ScrollStep_detour, o_ScrollStep); // counts steps in both modes (rate log)
    Install(kCityRender, &CityRender_detour, o_CityRender);
    Install(kScreenToGrid, &ScreenToGrid_detour, o_ScreenToGrid);
    Install(kMouseToGrid, &MouseToGrid_detour, o_MouseToGrid);
    Install(kScreenToGridRaw, &ScreenToGridRaw_detour, o_ScreenToGridRaw);
    Install(kGridToScreen, &GridToScreen_detour, o_GridToScreen);
    Install(kDragStart, &DragStart_detour, o_DragStart);
    Install(kDragDraw, &DragAdjusted<&o_DragDraw, &kDragDraw, true>, o_DragDraw);
    Install(kDragFinish, &DragAdjusted<&o_DragFinish, &kDragFinish, false>, o_DragFinish);
    Install(kLineDraw, &Line_detour, o_Line);
    Install(kCityWidth, &CityWidth_detour, o_CityWidth);
    Install(kScrollGetDirection, &GetDirection_detour, o_GetDirection);
    g_smooth = smooth;
    if (smooth) {
        core_log("emperor: smooth scrolling on (city drawn on the tile grid into our buffer and copied with the pixel "
                 "remainder; conversions shift the origin; Augustus easing %u ms; guarded: on an error the game's "
                 "whole tiles)",
                 core::scroll::SmoothScroll::kEaseMs);
    }
    else {
        core_log("emperor: smooth scrolling off (gameplay.smooth_scroll): the game's tile steps (hooks installed, "
                 "passing through, so it can be switched on from the panel)");
    }
    return true;
}

bool SetSmooth(bool smooth)
{
    if (smooth && !core::diag::Enabled(g_feature)) {
        core_log("emperor: smooth scrolling cannot be switched on: it was switched off after an error");
        return false;
    }
    if (smooth == g_smooth) {
        return true;
    }
    g_smooth = smooth;
    if (!smooth) {
        g_px = g_py = 0; // the view snaps to the whole tile it is on
    }
    g_scroller.Reset();
    core_log("emperor: smooth scrolling switched %s", smooth ? "on" : "off");
    return true;
}

void OnLoop(double frameMs)
{
    if (*kViewScrolling) {
        g_scrollingMs += frameMs;
    }
}

void Resume()
{
    g_scroller.Reset();
}

void SetAverageFrameMs(double ms)
{
    if (ms > 0.05 && ms < 100) {
        g_frameMs = ms;
    }
}

void SetCityExtent(int right, int bottom)
{
    right = right > 0 ? right : 0;
    bottom = bottom > 0 ? bottom : 0;
    if (right != g_extRight || bottom != g_extBottom) {
        g_loggedExtent = false; // logged again only when it changes (the game sets the viewport every frame at times)
    }
    g_extRight = right;
    g_extBottom = bottom;
}

void SaveCityBuffer(int report)
{
    if (!g_city || g_cityPitch <= 0 || g_cityRows <= 0) {
        core_log("report #%d: the M4 city buffer has not been used yet (no pixel offset drawn since the start)", report);
        return;
    }
    static core::diag::Feature save{"report: city buffer (games/emperor/scroll)"};
    struct Call {
        int report;
    } c{report};
    core::diag::Guarded(
        save,
        [](void *p) {
            char label[24];
            std::snprintf(label, sizeof(label), "report%d", static_cast<Call *>(p)->report);
            core::capture::SaveOne(g_city, g_cityPitch, g_cityPitch / 2, g_cityRows, label, "city");
        },
        &c);
}

void TakeStats(char *out, size_t size)
{
    if (g_scrollingMs < 1) {
        out[0] = 0;
    }
    else {
        double s = g_scrollingMs / 1000.0;
        int idx = (100 - *kScrollSpeed) / 10;
        std::snprintf(out, size,
                      "; scrolling %.0f ms at scroll speed %d%%: x %.0f px/s, y %.0f px/s, %ld steps, %ld tile changes "
                      "(expected %.0f px/s per axis)",
                      g_scrollingMs, *kScrollSpeed, g_movedX / s, g_movedY / s, g_steps, g_carries,
                      (*kTileWidth + 2) * 1000.0 / ((idx < 0 ? 0 : idx) * 12 + 2 + g_frameMs / 2));
    }
    g_scrollingMs = 0;
    g_movedX = g_movedY = 0;
    g_steps = 0;
    g_carries = 0;
}

const Detour *Detours(int *count)
{
    *count = static_cast<int>(sizeof(kDetours) / sizeof(kDetours[0]));
    return kDetours;
}

void SetRemainderForTest(int px, int py)
{
    g_px = px;
    g_py = py;
    g_haveLast = true;
    g_lastViewX = *kViewX;
    g_lastViewY = *kViewY;
}

void Remainder(int *px, int *py)
{
    *px = g_px;
    *py = g_py;
}

void SetCityDrawn(CityDrawn fn)
{
    g_cityDrawn = fn;
}

void SetAfterCity(AfterCity fn)
{
    g_afterCity = fn;
}

void PictureDrawing(core::diag::Restore &r, int right, int bottom)
{
    r.Set(&g_px, 0);
    r.Set(&g_py, 0);
    r.Set(&g_inRender, true);
    g_map.fx = 0; // no zoom in the drawing's conversions (it is 0 outside a zoomed drawing anyway)
    g_renderShown = {0, 0, right, bottom};
}

void DrawCityForPicture(int vx, int vy, int orientation)
{
    DrawCity(vx, vy, orientation);
}

bool Working()
{
    return core::diag::Enabled(g_feature);
}

bool OnCity(int x, int y)
{
    if (g_inRender) {
        return false;
    }
    const Shown s = ShownCity();
    return x >= s.left && x < s.right && y >= s.top && y < s.bottom;
}

} // namespace emperor::scroll
