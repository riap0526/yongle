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
#include "core/log.h"
#include "core/scroll/smooth_scroll.h"
#include "games/emperor/site.h"

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

// Return addresses of the scroll_get_direction calls whose result goes to view_scroll_step (the city view); the
// other two callers (0x539a23, 0x5f7349) scroll other things and keep the original behaviour.
const DWORD kCityScrollCalls[] = {0x538508, 0x538544, 0x5385da, 0x539f4f, 0x53800b, 0x538085, 0x54b4d4};

const Var<int> kViewX{0x101d0ac};
const Var<int> kViewY{0x101d0b0};
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
const Var<int> kScreenPitch{0x10de000};  // bytes per row
const Var<int> kDdWidth{0x10de008};
const Var<int> kDdHeight{0x10de004};

core::diag::Feature g_feature{"smooth scrolling (games/emperor/scroll)"};

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

// A conversion would see a moved origin now.
bool Shifts()
{
    return g_depth == 0 && (g_px != 0 || g_py != 0) && core::diag::Enabled(g_feature);
}

// Moves the origin by the remainder for one conversion call (screen-to-grid also gets a viewport one column and
// a row pair larger, unless city_render has made it larger already). Every change goes into r.
void BeginShift(core::diag::Restore &r, bool grow)
{
    r.Set(&g_depth, 1);
    r.Set(kOriginX.ptr(), *kOriginX - g_px);
    r.Set(kOriginY.ptr(), *kOriginY - g_py);
    if (grow && !g_inRender) {
        r.Set(kWidthTiles.ptr(), *kWidthTiles + 1);
        r.Set(kHeightRows.ptr(), *kHeightRows + 2);
    }
}

// Runs call() with the origin moved, under the guard; plainly when nothing is moved; the game's own way again when
// the guarded call was abandoned.
template <typename Call> void Converted(bool grow, Call &call)
{
    if (!Shifts()) {
        call();
        return;
    }
    struct Ctx {
        Call *call;
        bool grow;
        core::diag::Restore r;
    } ctx{&call, grow, {}};
    if (core::diag::Guarded(
            g_feature,
            [](void *p) {
                Ctx &c = *static_cast<Ctx *>(p);
                BeginShift(c.r, c.grow);
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
        if (held && !(GetAsyncKeyState(vk) & 0x8000)) {
            held = false;
        }
        return held;
    }
};
HeldKey g_left{kKeyLeft, VK_LEFT}, g_right{kKeyRight, VK_RIGHT}, g_up{kKeyUp, VK_UP}, g_down{kKeyDown, VK_DOWN};

int g_lastFrame = -1;

// One frame of smooth scrolling. r: the camera, view_scrolling and the key flags as they were before the frame (put
// back if the frame is abandoned; kept when it completes).
void SmoothFrame(core::diag::Restore &r)
{
    if (*kFrameCounter == g_lastFrame) {
        return; // once per game_frame
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
    int x = *kCursorX, y = *kCursorY;
    bool left = g_left.Poll() || x < 1;
    bool right = g_right.Poll() || x >= *kDesktopW - 1;
    bool up = g_up.Poll() || y < 1;
    bool down = g_down.Poll() || y >= *kDesktopH - 1;
    int dirX = (right ? 1 : 0) - (left ? 1 : 0);
    int dirY = (down ? 1 : 0) - (up ? 1 : 0);
    *kViewScrolling = (left || right || up || down) ? 1 : 0; // as scroll_get_direction: the simulation pauses
    int idx = (100 - *kScrollSpeed) / 10;
    if (idx < 0) idx = 0;
    if (idx >= 10) dirX = dirY = 0; // the game does not scroll at all then
    double stepMs = idx * 12 + 2 + g_frameMs / 2;
    double vx = (*kTileWidth + 2) / stepMs, vy = (4 * *kTileHalfHeight) / stepMs;
    int dx = 0, dy = 0;
    g_scroller.Update(dirX, dirY, vx, vy, &dx, &dy);
    g_movedX += std::abs(dx);
    g_movedY += std::abs(dy);
    Move(dx, dy);
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

void DrawCity(int vx, int vy, int orientation)
{
    core::diag::perf::Scope timed(kCityDrawPhase);
    Original(o_CityRender, kCityRender)(vx, vy, orientation);
}

// Where the city is to be seen to (SetCityExtent): 0 = the game's clip.
int g_extRight = 0, g_extBottom = 0;
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
    const int right = g_extRight > *kClipRight ? (g_extRight < width ? g_extRight : width) : *kClipRight;
    const int bottom = g_extBottom > *kClipBottom ? (g_extBottom < height ? g_extBottom : height) : *kClipBottom;
    if (back && *kClipLeft >= 0 && *kClipTop >= 0 && *kClipLeft < right && *kClipTop < bottom && right <= width &&
        bottom <= height) {
        g_cityDrawn(back, pitch, *kClipLeft, *kClipTop, right, bottom);
    }
}

void __cdecl CityRender_detour(int vx, int vy, int orientation)
{
    RenderCall c{vx, vy, orientation, false, {}};
    if (core::diag::Guarded(g_feature, [](void *p) { Render(*static_cast<RenderCall *>(p)); }, &c, &c.r)) {
        if (!c.plain) {
            AfterRender();
            return;
        }
    }
    else {
        FallBack(); // abandoned (or off): drawn again on whole tiles
    }
    DrawCity(vx, vy, orientation);
    AfterRender();
}

int __cdecl ScreenToGrid_detour(int x, int y)
{
    int r = 0;
    auto call = [&] { r = Original(o_ScreenToGrid, kScreenToGrid)(x, y); };
    Converted(true, call);
    return r;
}

int __cdecl MouseToGrid_detour(int unused)
{
    int r = 0;
    auto call = [&] { r = Original(o_MouseToGrid, kMouseToGrid)(unused); };
    Converted(true, call);
    return r;
}

int __cdecl ScreenToGridRaw_detour(int x, int y, int unused)
{
    int r = 0;
    auto call = [&] { r = Original(o_ScreenToGridRaw, kScreenToGridRaw)(x, y, unused); };
    Converted(true, call);
    return r;
}

void __cdecl GridToScreen_detour(int gx, int gy, int *sx, int *sy)
{
    auto call = [&] { Original(o_GridToScreen, kGridToScreen)(gx, gy, sx, sy); };
    if (g_inRender) {
        call(); // a position in our (unshifted) city buffer
        return;
    }
    Converted(false, call);
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
    if (g_smooth && core::diag::Enabled(g_feature) && IsCityScrollCall(__builtin_return_address(0))) {
        core::diag::Restore r;
        if (core::diag::Guarded(g_feature, [](void *p) { SmoothFrame(*static_cast<core::diag::Restore *>(p)); }, &r,
                                &r)) {
            return 8; // view_scroll_step(8) does nothing: the camera has been moved already
        }
        FallBack();
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
void __fastcall DragStart_detour(int *drag)
{
    Original(o_DragStart, kDragStart)(drag);
    g_dragPx0 = g_px; // only remembered (nothing that can fault)
    g_dragPy0 = g_py;
}

struct DragCall {
    int *drag;
    Drag_fn original;
    core::diag::Restore r;
};

template <Drag_fn *Trampoline, const Site *S> void __fastcall DragAdjusted(int *drag)
{
    const Drag_fn original = Original(*Trampoline, *S);
    const int dx = g_px - g_dragPx0, dy = g_py - g_dragPy0;
    if ((dx == 0 && dy == 0) || !core::diag::Enabled(g_feature)) {
        original(drag);
        return;
    }
    DragCall c{drag, original, {}};
    if (core::diag::Guarded(
            g_feature,
            [](void *p) {
                DragCall &c = *static_cast<DragCall *>(p);
                c.r.Set(&c.drag[2], c.drag[2] - (g_px - g_dragPx0)); // the start corner, for this call
                c.r.Set(&c.drag[3], c.drag[3] - (g_py - g_dragPy0));
                c.original(c.drag);
                c.r.Undo();
            },
            &c, &c.r)) {
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
    {&kDragDraw, reinterpret_cast<void *>(&DragAdjusted<&o_DragDraw, &kDragDraw>)},
    {&kDragFinish, reinterpret_cast<void *>(&DragAdjusted<&o_DragFinish, &kDragFinish>)},
};

} // namespace

bool Attach(bool smooth)
{
    const Site *sites[] = {&kCityRender, &kScreenToGrid, &kMouseToGrid, &kScreenToGridRaw, &kGridToScreen,
                           &kScrollGetDirection, &kViewScrollStep, &kDragStart, &kDragDraw, &kDragFinish,
                           &kViewClamp, &kSoundViewDecay};
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
    Install(kDragDraw, &DragAdjusted<&o_DragDraw, &kDragDraw>, o_DragDraw);
    Install(kDragFinish, &DragAdjusted<&o_DragFinish, &kDragFinish>, o_DragFinish);
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
            core::capture::SaveOne(g_city, g_cityPitch, g_cityPitch / 2, g_cityRows, label, "m4city");
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

bool Working()
{
    return core::diag::Enabled(g_feature);
}

} // namespace emperor::scroll
