// The picture of smooth scrolling (picture.h). Moved from games/emperor/scroll.cpp (Yongle's)
// for Zeus; the behavior is Yongle's, except that a city clip reaching
// past the screen's bottom (Zeus's layout) is drawn with the remainder too (only the rows on the screen are shown).
#include "core/scroll/picture.h"

#include "core/capture/screenshot.h"
#include "core/ddraw/surface_memory.h"
#include "core/diag/perf.h"
#include "core/display/city_zoom.h"
#include "core/display/framed_screens.h"
#include "core/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core::scroll::picture {

namespace {

namespace perf = core::diag::perf;

core::diag::Feature &FeatureOf(const PictureGame &g)
{
    return *g.camera->feature;
}

int g_depth = 0;              // 1 while a conversion runs with the origin moved (calls inside it are not moved again)
bool g_inRender = false;      // city_render is drawing into our buffer (unshifted origin, larger clip)
int g_dragPx0 = 0, g_dragPy0 = 0; // the remainder when the current drag selection started
ZoomMap g_map{};              // the drawing's zoom, while city_render draws (fx 0: none)
bool g_bufferMode = false;    // the drag selection works in buffer pixels: no zoom conversion meanwhile
Shown g_renderShown{};        // while city_render draws into our buffer (the viewport is larger then)
int g_extRight = 0, g_extBottom = 0; // where the city is to be seen to (SetCityExtent): 0 = the game's clip

// Our city buffer: same pitch as the game's back buffer, one row pair (and a little) taller.
BYTE *g_city = nullptr;
std::size_t g_citySize = 0;
int g_cityPitch = 0, g_cityRows = 0; // as last drawn (for a report)
int g_bufferUses = 0;
bool g_loggedRender = false;
bool g_loggedExtent = false;
CityDrawn g_cityDrawn = nullptr;
AfterCity g_afterCity = nullptr;

// Where the time goes (core/diag/perf.h): the game drawing the city (city_render's own work) and our copies to and
// from the city buffer.
const int kCityDrawPhase = perf::Register("city drawn by the game", perf::Thread::Game);
const int kCityCopyPhase = perf::Register("city buffer copies (ours)", perf::Thread::Game);

int Width(const PictureGame &g)
{
    return *g.vars.screenPitch / 2;
}

int ColPx(const PictureGame &g)
{
    return *g.camera->vars.tileWidth + 2;
}

int HalfH(const PictureGame &g)
{
    return *g.camera->vars.tileHalfHeight;
}

// The feature was switched off by its guard: the game's whole tiles from now on (the game state of the abandoned call
// has been put back by the guard already). The camera's remainder goes to 0, then the game layer's fallBack (Reset).
void FallBack(const PictureGame &g)
{
    camera::FallBack(*g.camera);
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

// The extent's edge, or the clip's when the extent does not reach past it; never past the screen.
int ShownEdge(int ext, int clip, int screen)
{
    const int edge = ext > clip ? ext : clip;
    return edge < screen ? edge : screen;
}

// The grid clip's right edge (viewport_init's: right = left + cols x (tile width + 2) - 2).
int GridRight(const PictureGame &g)
{
    return *g.vars.clipLeft + *g.vars.widthTiles * ColPx(g) - 2;
}

// The city is shown past the grid clip on the right (our layout's extent, at 100%).
bool PastClip(const PictureGame &g)
{
    if (g_extRight <= 0) {
        return false;
    }
    const int right = GridRight(g);
    return ShownEdge(g_extRight, right, Width(g)) > right;
}

// The east and south edges (camera.h SetEdgeOffsets): the original's edge
// (the grid clip's right / bottom edge: the view's whole columns / half rows, where the original's city ends at the
// game's limit) less where our city ends, in the game's pixels. 100%: the extent past the grid clip, or the grid clip
// past the screen (negative: the camera stops before the game's limit). Zoomed: the view's columns / half rows less the
// size of the buffer that is shown (the zoom's own BufferSize, which the zoomed drawing shows; positive: the camera
// goes past the game's limit, into the column / row pair the zoomed drawing draws besides). The game's own layout (no
// extent): 0.
int EastOffset(const PictureGame &g)
{
    if (g_extRight <= 0) {
        return 0;
    }
    const int right = GridRight(g);
    if (g.zoomPercent() != 100) {
        int bufW = 0, bufH = 0;
        g.zoomBufferSize(&bufW, &bufH);
        return bufW > 0 ? right - (*g.vars.clipLeft + bufW) : 0;
    }
    return right - ShownEdge(g_extRight, right, Width(g));
}

int SouthOffset(const PictureGame &g)
{
    if (g_extBottom <= 0) {
        return 0;
    }
    const PictureVars &v = g.vars;
    const int bottom = *v.clipTop + (*v.heightRows - 1) * HalfH(g);
    if (g.zoomPercent() != 100) {
        int bufW = 0, bufH = 0;
        g.zoomBufferSize(&bufW, &bufH);
        return bufH > 0 ? bottom - (*v.clipTop + bufH) : 0;
    }
    return bottom - ShownEdge(g_extBottom, bottom, *v.ddHeight);
}

// How much larger a conversion's viewport is made (BeginShift): as many columns as the remainder and the city shown
// past the grid clip need together (at least one), and as many row pairs (two half rows each) below. One was not
// enough with a large remainder and the gap left of the sidebar.
int GrowCols(const PictureGame &g)
{
    const int colPx = ColPx(g), right = GridRight(g);
    const int past = g_extRight > 0 ? ShownEdge(g_extRight, right, Width(g)) - right : 0;
    const int n = (RemX() + (past > 0 ? past : 0) + colPx - 1) / colPx;
    return n > 1 ? n : 1;
}

int GrowRows(const PictureGame &g)
{
    const PictureVars &v = g.vars;
    const int rowPx = 2 * HalfH(g), bottom = *v.clipTop + (*v.heightRows - 1) * HalfH(g);
    const int past = g_extBottom > 0 ? ShownEdge(g_extBottom, bottom, *v.ddHeight) - bottom : 0;
    const int n = rowPx > 0 ? (RemY() + (past > 0 ? past : 0) + rowPx - 1) / rowPx : 1;
    return 2 * (n > 1 ? n : 1);
}

// Before the game draws the city: the camera kept to the original's east and south edges (camera::KeepEdges; also with
// smooth scrolling off). city_render's view is the game's view_x / view_y when it is called by the game's city drawing:
// moved with it.
void KeepEdgesBeforeDrawing(const PictureGame &g, int *vx, int *vy)
{
    UpdateEdges(g);
    if (!core::diag::Enabled(FeatureOf(g)) || g_inRender) {
        return;
    }
    struct Ctx {
        const PictureGame *g;
        core::diag::Restore r;
    } c{&g, {}};
    const int beforeX = *g.camera->vars.viewX, beforeY = *g.camera->vars.viewY;
    if (!core::diag::Guarded(
            FeatureOf(g), [](void *p) { camera::KeepEdges(*static_cast<Ctx *>(p)->g->camera, static_cast<Ctx *>(p)->r); },
            &c, &c.r)) {
        FallBack(g);
        return;
    }
    if (*vx == beforeX) {
        *vx = *g.camera->vars.viewX;
    }
    if (*vy == beforeY) {
        *vy = *g.camera->vars.viewY;
    }
}

// Moves the origin by the remainder for one conversion call (screen-to-grid also gets a viewport larger by what the
// remainder and the city shown past the clip need, GrowCols / GrowRows, unless city_render has made it larger
// already); mouse: the game's mouse as the buffer pixel it shows (zoomed, mouse_to_grid). Every change goes into r.
void BeginShift(const PictureGame &g, core::diag::Restore &r, bool grow, bool mouse)
{
    const PictureVars &v = g.vars;
    const int cols = grow && !g_inRender ? GrowCols(g) : 0, rows = grow && !g_inRender ? GrowRows(g) : 0;
    r.Set(&g_depth, 1);
    r.Set(v.originX.ptr(), *v.originX - RemX());
    r.Set(v.originY.ptr(), *v.originY - RemY());
    if (grow && !g_inRender) {
        r.Set(v.widthTiles.ptr(), *v.widthTiles + cols);
        r.Set(v.heightRows.ptr(), *v.heightRows + rows);
    }
    ZoomMap m;
    if (mouse && !g_bufferMode && ZoomNow(g, &m)) {
        r.Set(v.mouseX.ptr(), BufX(m, *v.mouseX));
        r.Set(v.mouseY.ptr(), BufY(m, *v.mouseY));
    }
}

// The city as shown on the screen (right and bottom exclusive): the clip's top left to the extent, or to the grid
// clip without one -- the same part Render shows.
Shown ShownCity(const PictureGame &g)
{
    if (g_inRender) {
        return g_renderShown;
    }
    ZoomMap m;
    if (ZoomNow(g, &m)) { // the viewport is the buffer's then, larger than the city on the screen
        return {m.left, m.top, m.showRight, m.showBottom};
    }
    const PictureVars &v = g.vars;
    const int left = *v.clipLeft, top = *v.clipTop;
    const int right = left + *v.widthTiles * ColPx(g) - 2, bottom = top + (*v.heightRows - 1) * HalfH(g);
    return {left, top, ShownEdge(g_extRight, right, Width(g)), ShownEdge(g_extBottom, bottom, *v.ddHeight)};
}

// Screen-to-grid only takes a point inside origin + viewport (mouse_to_grid, screen_to_grid). With the origin moved by
// the remainder and the viewport a column and a row pair larger (BeginShift), that reached up to a column into the
// sidebar and up to the remainder into the top bar, so the cursor tile and clicks went through them to the city
// (found in a Yongle playtest). A point outside the city as shown is no tile (0, what the game returns outside its
// bounds, without calling it). With the game's own layout and no remainder the game decides as always.
bool OffShownCity(const PictureGame &g, int x, int y)
{
    if (g_depth != 0 || (!Shifts(g) && g_extRight == 0)) {
        return false;
    }
    const Shown s = ShownCity(g);
    return x < s.left || x >= s.right || y < s.top || y >= s.bottom;
}

// Runs call(ctx) with the origin moved, under the guard; plainly when nothing is moved; the game's own way again when
// the guarded call was abandoned.
void Converted(const PictureGame &g, bool grow, void (*call)(void *), void *ctx, bool mouse = false)
{
    // With no remainder the viewport is still made larger when the city is shown past the grid clip: the gap left of
    // the sidebar is outside the game's viewport, which has no tile there otherwise.
    if (!Shifts(g) && !(grow && g_depth == 0 && core::diag::Enabled(FeatureOf(g)) && PastClip(g))) {
        call(ctx);
        return;
    }
    struct Ctx {
        const PictureGame *g;
        void (*call)(void *);
        void *ctx;
        bool grow, mouse;
        core::diag::Restore r;
    } c{&g, call, ctx, grow, mouse, {}};
    if (core::diag::Guarded(
            FeatureOf(g),
            [](void *p) {
                Ctx &c = *static_cast<Ctx *>(p);
                BeginShift(*c.g, c.r, c.grow, c.mouse);
                c.call(c.ctx);
                c.r.Undo(); // the origin is moved only for this call
            },
            &c, &c.r)) {
        return;
    }
    FallBack(g);
    call(ctx);
}

// ---------------------------------------------------------------------------------------------
// Drawing

void DrawAgain(const PictureGame &g, int vx, int vy, int orientation)
{
    perf::Scope timed(kCityDrawPhase);
    g.drawCityAgain(vx, vy, orientation);
}

// The game draws as always, straight into the back buffer; with the grid clip when the clip holds the extent.
void DrawPlain(const PictureGame &g, RenderCall &c, bool offGrid, int right, int bottom)
{
    if (!offGrid) {
        c.plain = true;
        return;
    }
    c.r.Set(g.vars.clipRight.ptr(), right);
    c.r.Set(g.vars.clipBottom.ptr(), bottom);
    DrawCity(g, c.vx, c.vy, c.orientation);
    c.r.Undo();
}

// city_render with the pixel remainder (see picture.h) and the extent. While the game draws, draw_buffer, the clip's
// right and bottom and the viewport size are ours (all in c.r, put back after the draw or when it is abandoned). The
// clip stays on the tile grid (whole columns, whole half rows): the game's quick tile drawing only handles tiles cut
// in half. The grid clip is worked out from the viewport as viewport_init does (right = left + cols x (tile width + 2)
// - 2, bottom = top + (rows - 1) x half height), not read from the clip itself: while Emperor's city message panel is
// laid out the clip holds the extent, which is not on the grid.
// The grid clip's bottom may lie below the screen (Zeus's layout draws into the back buffer's spare rows): the city is
// then shown down to the screen's bottom only, and drawn as long as that overshoot stays within the spare rows.
void Render(const PictureGame &g, RenderCall &c)
{
    camera::SyncView(*g.camera);
    if (g.renderZoomed(c)) {
        return;
    }
    const PictureVars &v = g.vars;
    BYTE *back = *v.drawBuffer;
    const int pitch = *v.screenPitch, width = pitch / 2, height = *v.ddHeight;
    const int halfH = HalfH(g), colPx = ColPx(g), rowPx = 2 * halfH;
    const int left = *v.clipLeft, top = *v.clipTop;
    const int right = left + *v.widthTiles * colPx - 2, bottom = top + (*v.heightRows - 1) * halfH;
    const bool offGrid = *v.clipRight != right || *v.clipBottom != bottom; // the extent is in the clip just now
    const int px = RemX(), py = RemY();
    // The part to show: up to the extent, never past the screen.
    const int showRight = ShownEdge(g_extRight, right, width);
    const int showBottom = ShownEdge(g_extBottom, bottom, height);
    // Past the grid clip (0 when the clip reaches as far, or farther: the bottom of Zeus's layout).
    const int needR = showRight > right ? showRight - right : 0;
    const int needB = showBottom > bottom ? showBottom - bottom : 0;
    // Our buffer: as many more columns / row pairs as the offset and the extent need.
    const int moreCols = (px + needR + colPx - 1) / colPx, moreRows = (py + needB + rowPx - 1) / rowPx;
    const int right2 = right + (moreCols > 0 ? moreCols : 1) * colPx;
    const int bottom2 = bottom + (moreRows > 0 ? moreRows : 1) * rowPx;
    const std::size_t size = static_cast<std::size_t>(pitch) * (bottom2 + 2);
    const bool usable = back && pitch >= *v.ddWidth * 2 && 0 <= left && left < right && 0 <= top && top < bottom &&
                        bottom <= height + core::ddraw::kSpareRows && right2 <= width && halfH > 0 && colPx > 2;
    if (!g_loggedRender) {
        g_loggedRender = true;
        core_log("scroll: city view clip (%d,%d)-(%d,%d), tile step %d x %d px, back buffer %d x %d pitch %d%s", left,
                 top, right, bottom, colPx, rowPx, static_cast<int>(*v.ddWidth), height, pitch,
                 usable ? "" : "; no room for the pixel offset: the view is drawn on whole tiles");
    }
    if ((needR > 0 || needB > 0) && !g_loggedExtent) {
        g_loggedExtent = true;
        core_log("scroll: the city is shown past its clip to (%d,%d): %d px on the right (the gap to the sidebar), %d "
                 "rows at the bottom%s",
                 showRight, showBottom, needR, needB, usable ? "" : "; not possible here (no room): the clip only");
    }
    if (!usable) {
        DrawPlain(g, c, offGrid, right, bottom);
        return;
    }
    if (px == 0 && py == 0 && needR == 0) {
        if (needB == 0) {
            DrawPlain(g, c, offGrid, right, bottom); // nothing to show past the clip
            return;
        }
        // Only the bottom: the clip down to the next half row past the screen's bottom, straight into the back
        // buffer; the rows below the screen land in its spare rows (never shown).
        const int moreHalf = (needB + halfH - 1) / halfH;
        if (bottom + moreHalf * halfH - height > core::ddraw::kSpareRows) {
            DrawPlain(g, c, offGrid, right, bottom);
            return;
        }
        c.r.Set(v.clipRight.ptr(), right);
        c.r.Set(v.clipBottom.ptr(), bottom + moreHalf * halfH);
        c.r.Set(v.heightRows.ptr(), *v.heightRows + moreHalf);
        DrawCity(g, c.vx, c.vy, c.orientation);
        c.r.Undo();
        return;
    }
    BYTE *city = Buffer(size);
    if (!city) {
        DrawPlain(g, c, offGrid, right, bottom);
        return;
    }
    ++g_bufferUses; // the buffer holds the screen's layout now: a zoomed frame clears it again
    // Start from what the back buffer holds there, as the game would draw over it.
    std::int64_t copyStart = perf::Now();
    for (int y = top; y < bottom2; ++y) {
        BYTE *dst = city + static_cast<std::size_t>(y) * pitch + left * 2;
        if (y < height) {
            std::memcpy(dst, back + static_cast<std::size_t>(y) * pitch + left * 2, (right2 - left) * 2);
        }
        else {
            std::memset(dst, 0, (right2 - left) * 2);
        }
    }
    perf::Add(kCityCopyPhase, perf::Now() - copyStart);
    NoteBuffer(pitch, bottom2);
    c.r.Set(v.drawBuffer.ptr(), city);
    c.r.Set(v.clipRight.ptr(), right2);
    c.r.Set(v.clipBottom.ptr(), bottom2);
    c.r.Set(v.widthTiles.ptr(), *v.widthTiles + (right2 - right) / colPx);
    c.r.Set(v.heightRows.ptr(), *v.heightRows + 2 * ((bottom2 - bottom) / rowPx));
    g_renderShown = {left, top, showRight, showBottom};
    c.r.Set(&g_inRender, true);
    DrawCity(g, c.vx, c.vy, c.orientation);
    c.r.Undo();
    copyStart = perf::Now();
    for (int y = top; y < showBottom; ++y) {
        std::memcpy(back + static_cast<std::size_t>(y) * pitch + left * 2,
                    city + static_cast<std::size_t>(y + py) * pitch + (left + px) * 2, (showRight - left) * 2);
    }
    perf::Add(kCityCopyPhase, perf::Now() - copyStart);
}

// The city as shown, right after it was drawn (before the sidebar and the rest of the interface).
void AfterRender(const PictureGame &g)
{
    if (!g_cityDrawn) {
        return;
    }
    int left, top, right, bottom;
    const char *why = nullptr;
    if (DrawnCity(g, &left, &top, &right, &bottom, &why)) {
        g_cityDrawn(*g.vars.drawBuffer, *g.vars.screenPitch, left, top, right, bottom);
    }
    else { // the menus' city background: a summary line if no city is ever kept (core/display/framed_screens)
        core::display::framed::NoteCitySkipped(why, left, top, right, bottom);
    }
}

void After(const PictureGame &g, int orientation)
{
    AfterRender(g);
    if (g_afterCity) {
        g_afterCity(orientation);
    }
}

// ---------------------------------------------------------------------------------------------
// The drag selection
//
// During a military drag selection the cursor's tile (from mouse_to_grid) is the frame's moving corner. Edge
// scrolling puts the cursor on the sidebar or the top bar, where there is no tile (0): the corner then lies far away,
// drag_select_box_valid says no and the frame is not drawn until the cursor is back on the city (the original does the
// same). While the drag lasts, the mouse is kept to the city as shown for mouse_to_grid, so the corner stays at the
// city's edge and the frame stays (scrolling reads the raw cursor).
// A drag is a frame only from the city, and once the cursor is more than kDragPx away from where it was pressed on
// both axes: a press on the sidebar (the games start a drag for any press that hits no soldier) or a small drag stays
// a click as in the original -- released, a click with no frame orders the selected company to the cursor's tile, none
// off the city. The games' own threshold is 4 (set once at start-up: frame wider and taller than 2), which with
// tile-snapped corners meant "into another tile".
constexpr int kDragPx = 8;
bool g_dragOnCity = false;
int g_dragStartX = 0, g_dragStartY = 0; // the press, on the screen

bool DragMoved(int x, int y)
{
    return g_dragOnCity && std::abs(x - g_dragStartX) > kDragPx && std::abs(y - g_dragStartY) > kDragPx;
}

bool DragClampMouse(const PictureGame &g)
{
    const PictureVars &v = g.vars;
    if (*v.mouseMode != g.mouseModeDragSelect || g_depth != 0 || g_inRender) {
        return false;
    }
    const Shown s = ShownCity(g);
    const int x = *v.mouseX, y = *v.mouseY;
    const int cx = x < s.left ? s.left : x >= s.right ? s.right - 1 : x;
    const int cy = y < s.top ? s.top : y >= s.bottom ? s.bottom - 1 : y;
    if (!DragMoved(cx, cy)) {
        return false; // not a frame (yet): the game's own tile (none off the city)
    }
    if (cx == x && cy == y) {
        return false;
    }
    *v.mouseX = cx;
    *v.mouseY = cy;
    return true;
}

// The drag frame's corners follow the cursor pixel by pixel. drag_select_start / drag_select_draw take a corner from
// grid_to_screen of the cursor's tile less the tile's height x heightPx: the frame then jumps by tiles (a column
// sideways is also half a row up or down) and an edge tile's position can lie off the city, so the frame was dropped
// at the left and top edges (drag_select_box_valid). For those two calls the result is the cursor (kept to the city
// as shown, in the game's coordinates: the buffer pixel it shows while zoomed) plus the height the game takes off
// again.
bool DragCorner(const PictureGame &g, int *sx, int *sy)
{
    const PictureVars &v = g.vars;
    if (*v.mouseMode != g.mouseModeDragSelect || !sx || !sy || g_inRender || !g_dragOnCity) {
        return false; // a drag from the sidebar or the top bar: the game's own (no frame, as in the original)
    }
    // While drag_select_draw runs zoomed, dd_height is the buffer's: the city as shown comes from the drag's own map,
    // taken before.
    ZoomMap m;
    const bool zoomed = g.dragLinesMap(&m) ? true : ZoomNow(g, &m);
    const Shown s = zoomed ? Shown{m.left, m.top, m.showRight, m.showBottom} : ShownCity(g);
    // 2 px inside on the left and the top, as the frame's right and bottom lines are (they are drawn 1 px inside
    // their corner): the game leaves out a side at 0 or less, and at the top bar's edge the line hides.
    constexpr int kInside = 2;
    int x = *v.mouseX, y = *v.mouseY;
    x = x < s.left + kInside ? s.left + kInside : x >= s.right ? s.right - 1 : x;
    y = y < s.top + kInside ? s.top + kInside : y >= s.bottom ? s.bottom - 1 : y;
    if (zoomed) {
        x = BufX(m, x);
        y = BufY(m, y);
    }
    const int tile = *v.cursorTile;
    const int height = tile > 0 && tile < g.mapTiles ? v.tileHeights.ptr()[tile] : 0;
    *sx = x;
    *sy = y + g.heightPx * height;
    return true;
}

// The start corner is kept in screen pixels plus the view_x / view_y of that moment, and moved by whole tiles when
// the view scrolls during the drag. With a pixel remainder the start corner must also move by the change of the
// remainder since the drag started.
// Zoomed: the three functions work in buffer pixels (the corners come from grid_to_screen, less the ground's height;
// the start moves by whole tiles x tile_width; the selection steps through the frame by tile_width and asks
// screen_to_grid_unbounded) -- buffer mode, so none of their positions is converted to the screen and back. Only the
// frame drawn (drag_select_draw) goes to the screen (the game layer's dragDrawZoom).
struct DragCall {
    const PictureGame *g;
    int *drag;
    void (*original)(int *);
    bool draw, start;
    core::diag::Restore r;
};

void DragBody(DragCall &c)
{
    const PictureGame &g = *c.g;
    c.r.Set(&g_bufferMode, true);
    // The start corner follows the view by (view_x moved) x tile_width, but a column is tile_width + 2: 2 px per
    // column scrolled besides the remainder's change.
    const int dx = RemX() - g_dragPx0 + (c.start ? 0 : 2 * (*g.camera->vars.viewX - c.drag[0])), dy = RemY() - g_dragPy0;
    if (!c.start && (dx != 0 || dy != 0)) {
        c.r.Set(&c.drag[2], c.drag[2] - dx); // the start corner, for this call
        c.r.Set(&c.drag[3], c.drag[3] - dy);
    }
    if (c.draw) {
        g.dragDrawZoom(c.r);
    }
    c.original(c.drag);
    c.r.Undo();
}

bool DragPlain(const PictureGame &g, const int *drag)
{
    return !core::diag::Enabled(FeatureOf(g)) ||
           (RemX() == g_dragPx0 && RemY() == g_dragPy0 && *g.camera->vars.viewX == drag[0] && !Zoomed(g));
}

} // namespace

// ---------------------------------------------------------------------------------------------

int RemX()
{
    int px, py;
    camera::Remainder(&px, &py);
    return px;
}

int RemY()
{
    int px, py;
    camera::Remainder(&px, &py);
    return py;
}

bool ZoomNow(const PictureGame &g, ZoomMap *m)
{
    if (!core::diag::Enabled(FeatureOf(g))) {
        return false;
    }
    if (g_inRender) {
        if (g_map.fx <= 0) {
            return false;
        }
        *m = g_map;
        return true;
    }
    const int percent = g.zoomPercent();
    if (percent == 100 || g_extRight <= 0 || g_extBottom <= 0) {
        return false;
    }
    int bufW = 0, bufH = 0;
    g.zoomBufferSize(&bufW, &bufH);
    const int width = Width(g), height = *g.vars.ddHeight;
    m->left = *g.vars.clipLeft;
    m->top = *g.vars.clipTop;
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

bool Zoomed(const PictureGame &g)
{
    ZoomMap m;
    return ZoomNow(g, &m);
}

bool Shifts(const PictureGame &g)
{
    return g_depth == 0 && core::diag::Enabled(FeatureOf(g)) && (RemX() != 0 || RemY() != 0 || Zoomed(g));
}

void DrawCity(const PictureGame &g, int vx, int vy, int orientation)
{
    perf::Scope timed(kCityDrawPhase);
    g.drawCity(vx, vy, orientation);
}

void CityRender(const PictureGame &g, int vx, int vy, int orientation)
{
    KeepEdgesBeforeDrawing(g, &vx, &vy);
    struct Ctx {
        const PictureGame *g;
        RenderCall c;
    } ctx{&g, {vx, vy, orientation, false, {}}};
    // Three ways the game draws the city itself below: nothing to move (plain), the feature already off (after an
    // earlier fault), or once more after this drawing was abandoned. Only the last is an extra drawing (drawCityAgain:
    // Zeus does not count it).
    const bool wasOn = core::diag::Enabled(FeatureOf(g));
    if (core::diag::Guarded(
            FeatureOf(g), [](void *p) { Render(*static_cast<Ctx *>(p)->g, static_cast<Ctx *>(p)->c); }, &ctx,
            &ctx.c.r)) {
        if (!ctx.c.plain) {
            After(g, orientation);
            return;
        }
        DrawCity(g, vx, vy, orientation);
    }
    else {
        FallBack(g); // abandoned (or off): drawn again on whole tiles
        if (wasOn) {
            DrawAgain(g, vx, vy, orientation);
        }
        else {
            DrawCity(g, vx, vy, orientation);
        }
    }
    After(g, orientation);
}

namespace {

// A screen position as the zoomed buffer pixel it shows (unchanged at 100% and in the drag's buffer mode).
void ToBuffer(const PictureGame &g, int *x, int *y)
{
    ZoomMap m;
    if (g_depth == 0 && !g_bufferMode && ZoomNow(g, &m)) {
        *x = BufX(m, *x);
        *y = BufY(m, *y);
    }
}

struct XyCall {
    int (*original)(int, int, int);
    int x, y, unused, r;
};

} // namespace

int ScreenToGrid(const PictureGame &g, int x, int y, int unused, int (*original)(int, int, int))
{
    if (OffShownCity(g, x, y)) {
        return 0;
    }
    ToBuffer(g, &x, &y);
    XyCall c{original, x, y, unused, 0};
    Converted(g, true, [](void *p) {
        XyCall &c = *static_cast<XyCall *>(p);
        c.r = c.original(c.x, c.y, c.unused);
    }, &c);
    return c.r;
}

int MouseToGrid(const PictureGame &g, int unused, int (*original)(int))
{
    const PictureVars &v = g.vars;
    const int mouseX = *v.mouseX, mouseY = *v.mouseY;
    const bool clamped = core::diag::Enabled(FeatureOf(g)) && DragClampMouse(g);
    struct Call {
        int (*original)(int);
        int unused, r;
    } c{original, unused, 0};
    if (!OffShownCity(g, *v.mouseX, *v.mouseY)) {
        Converted(g, true, [](void *p) {
            Call &c = *static_cast<Call *>(p);
            c.r = c.original(c.unused);
        }, &c, true); // the mouse as the buffer pixel it shows while zoomed
    }
    if (clamped) {
        *v.mouseX = mouseX;
        *v.mouseY = mouseY;
    }
    return c.r;
}

int ScreenToGridRaw(const PictureGame &g, int x, int y, int unused, int (*original)(int, int, int))
{
    ToBuffer(g, &x, &y);
    XyCall c{original, x, y, unused, 0};
    Converted(g, true, [](void *p) {
        XyCall &c = *static_cast<XyCall *>(p);
        c.r = c.original(c.x, c.y, c.unused);
    }, &c);
    return c.r;
}

void GridToScreen(const PictureGame &g, DWORD from, int gx, int gy, int *sx, int *sy,
                  void (*original)(int, int, int *, int *))
{
    struct Call {
        void (*original)(int, int, int *, int *);
        int gx, gy;
        int *sx, *sy;
    } c{original, gx, gy, sx, sy};
    auto call = [](void *p) {
        Call &c = *static_cast<Call *>(p);
        c.original(c.gx, c.gy, c.sx, c.sy);
    };
    if ((from == g.dragStartCorner || from == g.dragDrawCorner) && core::diag::Enabled(FeatureOf(g))) {
        call(&c); // as the game does (it only writes *sx, *sy)
        if (DragCorner(g, sx, sy)) {
            return;
        }
    }
    if (g_inRender) {
        call(&c); // a position in our (unshifted) city buffer
        return;
    }
    const bool outer = g_depth == 0;
    Converted(g, false, call, &c);
    ZoomMap m;
    if (outer && !g_bufferMode && sx && sy && ZoomNow(g, &m)) { // zoomed: back to the screen
        *sx = ScreenX(m, *sx);
        *sy = ScreenY(m, *sy);
    }
}

// The "Game paused" banner is centered on the city's width: the city as shown, up to the sidebar. The game's is whole
// columns (zoomed: the buffer's). The game's own layouts (no extent) keep the game's width.
int CityWidth(const PictureGame &g, int (*original)())
{
    if (!g_inRender && g_depth == 0 && !g_bufferMode && g_extRight > 0 && core::diag::Enabled(FeatureOf(g))) {
        const Shown s = ShownCity(g);
        return s.right - s.left;
    }
    return original();
}

void DragStart(const PictureGame &g, int *drag, void (*original)(int *))
{
    const PictureVars &v = g.vars;
    // Where the drag was pressed (the frame follows the cursor only from the city), before the game takes its corner
    // (grid_to_screen, DragCorner).
    g_dragStartX = *v.mouseX;
    g_dragStartY = *v.mouseY;
    g_dragOnCity = false;
    if (core::diag::Enabled(FeatureOf(g)) && !g_inRender) {
        const Shown s = ShownCity(g);
        g_dragOnCity = g_dragStartX >= s.left && g_dragStartX < s.right && g_dragStartY >= s.top &&
                       g_dragStartY < s.bottom;
    }
    if (!core::diag::Enabled(FeatureOf(g)) || !Zoomed(g)) {
        original(drag);
    }
    else {
        DragCall c{&g, drag, original, false, true, {}};
        if (!core::diag::Guarded(FeatureOf(g), [](void *p) { DragBody(*static_cast<DragCall *>(p)); }, &c, &c.r)) {
            FallBack(g);
            original(drag);
        }
    }
    g_dragPx0 = RemX(); // only remembered (nothing that can fault)
    g_dragPy0 = RemY();
    // The smallest frame (drag_select_box_valid: wider and taller than this - 2, in the game's pixels -- the buffer's
    // while zoomed): kDragPx on the screen. Written for every drag (the game sets 4 once); the release decides frame or
    // click with it.
    if (g_dragOnCity && core::diag::Enabled(FeatureOf(g))) {
        ZoomMap m;
        const bool zoomed = ZoomNow(g, &m);
        drag[11] = static_cast<int>(kDragPx * (zoomed ? m.fx : 1.0) + 0.999) + 2; // +0x2c
        drag[12] = static_cast<int>(kDragPx * (zoomed ? m.fy : 1.0) + 0.999) + 2; // +0x30
    }
    else {
        drag[11] = drag[12] = 4; // the game's own
    }
}

void DragAdjusted(const PictureGame &g, int *drag, void (*original)(int *), bool draw)
{
    if (DragPlain(g, drag)) {
        original(drag);
        return;
    }
    DragCall c{&g, drag, original, draw, false, {}};
    if (core::diag::Guarded(FeatureOf(g), [](void *p) { DragBody(*static_cast<DragCall *>(p)); }, &c, &c.r)) {
        return;
    }
    FallBack(g);
    original(drag);
}

void UpdateEdges(const PictureGame &g)
{
    camera::SetEdgeOffsets(EastOffset(g), SouthOffset(g), *g.vars.widthTiles * 4096 + *g.vars.heightRows,
                           g.vars.heightRows.ptr());
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

void Extent(int *right, int *bottom)
{
    *right = g_extRight;
    *bottom = g_extBottom;
}

void Reset()
{
    g_dragPx0 = g_dragPy0 = 0;
    g_inRender = false;
    g_depth = 0;
}

bool DrawnCity(const PictureGame &g, int *left, int *top, int *right, int *bottom, const char **why)
{
    const PictureVars &v = g.vars;
    const BYTE *back = *v.drawBuffer;
    const int width = Width(g), height = *v.ddHeight;
    const bool zoomed = g.zoomPercent() != 100 && g_extRight > 0 && g_extBottom > 0; // the clip is our buffer's
    *left = *v.clipLeft;
    *top = *v.clipTop;
    *right = zoomed ? (g_extRight < width ? g_extRight : width) : ShownEdge(g_extRight, *v.clipRight, width);
    *bottom = zoomed ? (g_extBottom < height ? g_extBottom : height) : ShownEdge(g_extBottom, *v.clipBottom, height);
    if (!back) {
        *why = "no draw buffer";
        return false;
    }
    if (*left < 0 || *top < 0 || *left >= *right || *top >= *bottom) {
        *why = "the city's area is outside the draw buffer";
        return false;
    }
    return true;
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
    camera::ZeroRemainder(r);
    r.Set(&g_inRender, true);
    g_map.fx = 0; // no zoom in the drawing's conversions (it is 0 outside a zoomed drawing anyway)
    g_renderShown = {0, 0, right, bottom};
}

bool OnCity(const PictureGame &g, int x, int y)
{
    if (g_inRender) {
        return false;
    }
    const Shown s = ShownCity(g);
    return x >= s.left && x < s.right && y >= s.top && y < s.bottom;
}

void SaveCityBuffer(int report)
{
    if (!g_city || g_cityPitch <= 0 || g_cityRows <= 0) {
        core_log("report #%d: the smooth scrolling city buffer has not been used yet (no pixel offset drawn since the "
                 "start)",
                 report);
        return;
    }
    static core::diag::Feature save{"report: city buffer (core/scroll/picture)"};
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

void LogInstalled(const PictureGame &g)
{
    core_log("%s: smooth scrolling's picture: the city drawn on the tile grid into our buffer and copied with the pixel "
             "remainder; conversions shift the origin",
             g.log);
}

BYTE *Buffer(std::size_t size)
{
    if (size <= g_citySize) {
        return g_city;
    }
    if (g_city) {
        VirtualFree(g_city, 0, MEM_RELEASE);
    }
    g_city = static_cast<BYTE *>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_citySize = g_city ? size : 0;
    return g_city;
}

int BufferUses()
{
    return g_bufferUses;
}

void NoteBuffer(int pitch, int rows)
{
    g_cityPitch = pitch;
    g_cityRows = rows;
}

void SetRenderShown(const Shown &s)
{
    g_renderShown = s;
}

void SetRenderZoom(const ZoomMap &m)
{
    g_map = m;
}

bool *InRenderFlag()
{
    return &g_inRender;
}

bool BufferMode()
{
    return g_bufferMode;
}

} // namespace core::scroll::picture
