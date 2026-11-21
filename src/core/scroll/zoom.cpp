// The city's zoom (zoom.h). Moved from Yongle's layer (games/emperor/zoom.cpp and the zoom's part of
// games/emperor/scroll.cpp) for Zeus; the behavior is Yongle's, except that a scaling
// that fails now switches the zoom off (logged) instead of leaving the last frame's city without a word.
#include "core/scroll/zoom.h"

#include "core/diag/perf.h"
#include "core/display/city_zoom.h"
#include "core/display/scaled_rect.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/platform/parallel.h"
#include "core/scroll/camera.h"
#include "core/scroll/smooth_scroll.h"
#include "core/settings/settings.h"
#include "core/ui/notice.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core::scroll::zoom {
namespace {

namespace camera = core::scroll::camera;

core::diag::Feature g_feature{"city zoom (core/scroll/zoom)"};
const ZoomGame *g_game = nullptr; // Install
int g_requested = 100;
int g_start = 100; // [video] city_zoom: the zoom at start-up and after each loaded game
bool g_average = true;

// The wheel and the middle button move g_requested towards g_target, a little each frame (CameraStep, before the
// city is drawn), with the city point under g_anchor kept under the cursor. Not saved: city_zoom in the settings is
// the scale at start-up.
int g_target = 100;
int g_anchorX = 0, g_anchorY = 0;
int g_wheel = 0; // wheel delta short of a notch (high-resolution wheels send less than 120)
const char *g_by = "";
core::scroll::SmoothZoom g_motion;
int g_inputLogs = 0;
constexpr int kInputLogs = 30;
bool g_offDone = false;

constexpr core::display::ZoomLayout kAt100{100, 0, 0, false, false, 0, 0, 0, 0};
core::display::ZoomLayout g_now{100, 0, 0, false, true, 0, 0, 0, 0};
int g_lastMapCols = -1, g_lastMapRows = -1, g_lastClampRows = -1;
int g_logged[7] = {-1, -1, -1, -1, -1, -1, -1}; // percent, requested, map cols, map rows, clamp rows, show w, show h

bool Installed()
{
    return g_game != nullptr && core::diag::Enabled(g_feature);
}

void MapNow(int *cols, int *rows, int *clampRows)
{
    if (!g_game) {
        *cols = *rows = *clampRows = 0;
        return;
    }
    g_game->mapRange(cols, rows, clampRows);
}

bool MapChanged()
{
    int cols, rows, clampRows;
    MapNow(&cols, &rows, &clampRows);
    return cols != g_lastMapCols || rows != g_lastMapRows || clampRows != g_lastClampRows;
}

bool Animating()
{
    return Installed() && g_target != g_requested;
}

void SyncDrawnView()
{
    const camera::CameraVars &v = g_game->picture->camera->vars;
    *g_game->viewDrawnX = *v.viewX; // viewport_init set them to the map's middle (see ZoomGame::viewDrawnX)
    *g_game->viewDrawnY = *v.viewY;
}

// The view laid out again, then view_x / view_y clamped to it at once (the game's own view_clamp, not the game layer's
// hook): a larger view at the map's edge must not reach past the map until the game clamps next. Not logged while the
// wheel moves the scale (every frame then).
void Relayout(const char *why)
{
    if (Installed()) {
        if (!Animating()) {
            core_log("zoom: the city view is laid out again (%s)", why);
        }
        g_game->relayout();
        g_game->clamp();
        SyncDrawnView();
    }
}

bool ApplyZoom(int, const char *v, bool initial)
{
    const int p = std::strcmp(v, "auto") == 0 ? 100 : std::atoi(v);
    g_requested = p < core::display::kZoomMin ? core::display::kZoomMin
                  : p > core::display::kZoomMax ? core::display::kZoomMax
                                                : p;
    g_start = g_requested;
    g_target = g_requested; // the panel's value at once (a wheel movement under way ends)
    if (!initial && g_game) {
        Relayout("city_zoom changed");
        *g_game->redraw = 1;
    }
    return true;
}

bool ApplyShrink(int, const char *v, bool)
{
    g_average = std::strcmp(v, "nearest") != 0;
    return true;
}

int LimitNow()
{
    return g_now.limit > 0 ? g_now.limit : core::display::kZoomMax;
}

void MoveTowards(int target, int x, int y, const char *by)
{
    if (!Animating()) {
        g_requested = g_now.percent; // from the scale in effect (a larger value from the settings was capped)
    }
    if (++g_inputLogs <= kInputLogs) {
        core_log("zoom: %s at (%d,%d): %d%% -> %d%%%s", by, x, y, g_requested, target,
                 target == g_requested ? " (nothing to do)" : "");
    }
    core::ui::notice::Zoom(g_requested); // the zoom box at once, also when there is nothing to do (at a limit)
    g_target = target;
    g_anchorX = x;
    g_anchorY = y;
    g_by = by;
    g_motion.Start();
}

struct MouseCall {
    UINT m;
    WPARAM w;
    int x, y;
    bool taken;
};

// The wheel and the middle button over the city as shown, on the game's city screens. Elsewhere (the sidebar:
// its building categories, dialogs over the city, other screens) the game has them as always.
void TakeMouse(MouseCall &c)
{
    const bool wheel = c.m == WM_MOUSEWHEEL;
    if (!wheel && c.m != WM_MBUTTONDOWN && c.m != WM_MBUTTONDBLCLK) {
        return;
    }
    const int high = g_now.limit; // the cap and the map's limit (also worked out at 100%)
    if (!g_game->drawingWorks() || high <= 0 || !g_game->cityScreen() ||
        !picture::OnCity(*g_game->picture, c.x, c.y)) {
        if (wheel) {
            g_wheel = 0;
        }
        return; // the game's own (the sidebar's categories)
    }
    c.taken = true;
    const picture::PictureGame &p = *g_game->picture;
    if (*p.vars.mouseMode == p.mouseModeDragSelect) {
        g_wheel = 0; // not during a military drag selection (its start corner is in the scale it began in)
        return;
    }
    const int low = core::display::kZoomMin;
    if (!wheel) {
        MoveTowards(100 < high ? 100 : high, c.x, c.y, "middle button");
        return;
    }
    if (GET_KEYSTATE_WPARAM(c.w) & (MK_LBUTTON | MK_RBUTTON)) {
        g_wheel = 0; // not while dragging (a road, a selection): the drag works in the scale it started in
        return;
    }
    const int d = GET_WHEEL_DELTA_WPARAM(c.w);
    if (g_wheel != 0 && (d > 0) != (g_wheel > 0)) {
        g_wheel = 0; // turned the other way
    }
    g_wheel += d;
    const int notches = g_wheel / WHEEL_DELTA;
    g_wheel -= notches * WHEEL_DELTA;
    if (notches != 0) {
        // Wheel up (away from the player) zooms in (less of the city), as in Augustus.
        const int base = Animating() ? g_target : g_now.percent;
        MoveTowards(core::display::ZoomNotch(base, -notches, low, high), c.x, c.y, "wheel");
    }
}

bool MouseFilter(UINT m, WPARAM w, int x, int y)
{
    if (!Installed()) {
        return false;
    }
    MouseCall c{m, w, x, y, false};
    if (!core::diag::Guarded(g_feature, [](void *p) { TakeMouse(*static_cast<MouseCall *>(p)); }, &c)) {
        Off("the wheel could not be handled");
        return false;
    }
    return c.taken;
}

bool ViewFits(int cols, int rows)
{
    int mapCols, mapRows, clampRows;
    MapNow(&mapCols, &mapRows, &clampRows);
    return core::display::ViewFitsMap(cols, rows, mapCols, mapRows, clampRows);
}

// Before drawing: the view laid out again when the map's range or the bottom margin changed, or the view does not fit.
void Recheck()
{
    if (!Installed() || g_requested == 100) {
        return;
    }
    if (MapChanged()) {
        Relayout("the map's range or the bottom margin changed (drawing)");
    }
    else if (g_now.percent != 100 && !ViewFits(g_now.cols, g_now.rows)) { // cannot stay so: ViewFor caps it
        Relayout("the view does not fit the map (drawing)");
    }
}

// One step of the scale towards the wheel's target, laid out; true when the scale changed.
bool Advance(bool smooth)
{
    if (!Animating()) {
        return false;
    }
    const int next = g_motion.Step(g_requested, g_target, smooth);
    if (next == g_requested) {
        return false;
    }
    g_requested = next;
    Relayout(g_by); // logged (with the layout) only when the target is reached
    core::ui::notice::Zoom(g_now.percent); // the number counts along as the zoom moves (Augustus's way)
    if (!Animating()) {
        *g_game->redraw = 1;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The camera's step

// The scale's factors now (buffer pixels per screen pixel; 1 at 100%) and where the city starts on the screen.
bool Factors(picture::ZoomMap *m)
{
    const picture::PictureGame &p = *g_game->picture;
    if (picture::ZoomNow(p, m)) {
        return true;
    }
    int extRight, extBottom;
    picture::Extent(&extRight, &extBottom);
    if (g_now.percent != 100 || extRight <= 0 || extBottom <= 0) {
        return false;
    }
    *m = {*p.vars.clipLeft, *p.vars.clipTop, extRight, extBottom, 1.0, 1.0};
    return true;
}

double g_zoomShiftX = 0, g_zoomShiftY = 0; // the anchor's shift not moved yet (fractions; whole tiles when not smooth)

// ---------------------------------------------------------------------------------------------
// The zoomed drawing

// Where the time goes (core/diag/perf.h): our zoom's scaling (the game's drawing and the buffer copies are the
// picture's phases).
const int kCityZoomPhase = core::diag::perf::Register("city zoom scaling (ours)", core::diag::perf::Thread::Game);

int g_zoomPitchPx = 0, g_zoomRows = 0; // the buffer's last layout (cleared when it changes)
int g_zoomRight2 = 0, g_zoomBottom2 = 0;
int g_zoomBufferUses = 0;              // picture::BufferUses when the zoom's layout was last in the buffer
bool g_loggedZoomSkip = false;
bool g_failScaling = false;            // FailScalingForTest

// The drag selection's frame is drawn on the screen while the city is zoomed (DragDrawZoom, LineDraw).
picture::ZoomMap g_dragMap{}; // the drag selection's, while drag_select_draw runs (dd_height is changed then)
bool g_dragLines = false;     // drag_select_draw draws: line_draw takes buffer pixels
int g_dragScreenH = 0;        // dd_height as the screen's while drag_select_draw has it as buffer pixels

} // namespace

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "video", "city_zoom", "Starting city zoom (%)",
        "The city's zoom when the game starts: 100 as the game draws it, more to see more of the city, less to see "
        "it larger. From 50 to 150, and at most what the map allows. While playing, zoom with the mouse wheel over "
        "the city; the middle mouse button goes back to 100.",
        Type::Int, "100", Effect::Live, true, true, core::display::kZoomMin, core::display::kZoomMax, 5, nullptr,
        nullptr, nullptr, 0, "%", &LimitNow});
    core::settings::OnChange(i, &ApplyZoom);
    static const char *const kShrink[] = {"average", "nearest"};
    static const char *const kShrinkLabels[] = {"average (each screen pixel the mean of the city pixels it covers)",
                                                "nearest (one city pixel each, sharper, may shimmer)"};
    i = core::settings::Register(Item{
        "video", "city_zoom_shrink", "City zoom: shrinking",
        "When more than 100% is seen: how the city is made smaller on the screen.",
        Type::Choice, "average", Effect::Live, true, true, 0, 0, 0, nullptr, kShrink, kShrinkLabels, 2});
    core::settings::OnChange(i, &ApplyShrink);
}

bool Mouse(UINT message, WPARAM wparam, int x, int y)
{
    return MouseFilter(message, wparam, x, y);
}

void Install(const ZoomGame &g)
{
    g_game = &g;
    core::display::SetMouseFilter(&MouseFilter); // the wheel and the middle button over the city
    core_log("zoom: city zoom installed (asked %d%%, shrinking %s)", g_requested, g_average ? "average" : "nearest");
}

void OnViewClamp()
{
    if (Installed() && g_requested != 100 && MapChanged()) {
        int cols, rows, clampRows;
        MapNow(&cols, &rows, &clampRows);
        Relayout(clampRows != g_lastClampRows && rows == g_lastMapRows ? "another bottom margin"
                                                                       : "the map's range changed");
    }
}

int ViewFor(int showW, int showH, int colPx, int halfH, int *cols, int *rows)
{
    MapNow(&g_lastMapCols, &g_lastMapRows, &g_lastClampRows);
    const int cap = g_game ? g_game->cap : 100;
    // The zoomed city is drawn by smooth scrolling's picture: without it, no zoom (the game would draw the zoomed view
    // straight into the back buffer).
    if (!Installed() || g_requested == 100 || !g_game->drawingWorks()) {
        g_now = kAt100;
        if (g_game && colPx > 0 && halfH > 0) { // the limit for the panel's slider only (also after Off)
            g_now.limit = core::display::ZoomFor({100, showW, showH, colPx, halfH, g_lastMapCols, g_lastMapRows,
                                                  g_lastClampRows, cap})
                              .limit;
        }
        return 100;
    }
    const core::display::ZoomInput in{g_requested,  showW,         showH,           colPx, halfH,
                                      g_lastMapCols, g_lastMapRows, g_lastClampRows, cap};
    g_now = core::display::ZoomFor(in);
    if (!g_now.unknownMap && g_now.limit == 0) { // not even 50% fits: the game's own size for this map
        g_now.percent = 100;
        g_now.bufW = g_now.bufH = g_now.cols = g_now.rows = 0;
    }
    if (g_now.percent != 100) {
        *cols = g_now.cols;
        *rows = g_now.rows;
    }
    const int key[7] = {g_now.percent, g_requested, g_lastMapCols, g_lastMapRows, g_lastClampRows, showW, showH};
    if (!Animating() && std::memcmp(key, g_logged, sizeof(key)) != 0) { // once the wheel's movement ends
        std::memcpy(g_logged, key, sizeof(key));
        // The buffer the drawing will use (RenderZoomed): the view plus one column and a row pair, a little margin; 2
        // bytes a pixel.
        const double mb = (static_cast<double>(g_now.cols + 1) * colPx + 8) * ((g_now.rows + 3) * halfH + 48) * 2 /
                          (1024.0 * 1024.0);
        core_log("zoom: city %d%% (asked %d%%%s) on %d x %d px, map range %d x %d (the clamp holds %d half rows): "
                 "up to %d%% allowed (cap %d%%, Augustus' limit %d%%); view %d x %d, zoomed city %d x %d px, buffer "
                 "about %.1f MB%s",
                 g_now.percent, g_requested, g_now.capped ? ", capped" : "", showW, showH, g_lastMapCols,
                 g_lastMapRows, g_lastClampRows, g_now.limit, cap, g_now.augustus, g_now.cols, g_now.rows, g_now.bufW,
                 g_now.bufH, mb,
                 g_now.unknownMap ? " (no map yet: 100%)"
                 : g_now.limit == 0 ? " -- THE MAP IS SMALLER THAN EVEN 50%: the game's own size"
                                    : "");
    }
    return g_now.percent;
}

void ResetView()
{
    g_now = kAt100;
    // The map as laid out now (as ViewFor): otherwise, with a zoom asked, Recheck and OnViewClamp would see "the map
    // changed" on every frame and lay the view out again each time (the side-by-side review before acceptance).
    MapNow(&g_lastMapCols, &g_lastMapRows, &g_lastClampRows);
}

bool Active()
{
    return Installed();
}

int Percent()
{
    return g_now.percent;
}

void BufferSize(int *w, int *h)
{
    *w = g_now.bufW;
    *h = g_now.bufH;
}

bool AverageShrink()
{
    return g_average;
}

int Cap()
{
    return g_game ? g_game->cap : 100;
}

bool Moving()
{
    return Animating();
}

void Off(const char *why)
{
    if (g_offDone) {
        return;
    }
    g_offDone = true;
    if (core::diag::Enabled(g_feature)) {
        core::diag::Disable(g_feature, why); // else switched off by its guard already (the wheel)
    }
    core_log("zoom: off for the rest of the run (%s): the city at 100%%", why);
    g_target = g_requested;
    g_now = kAt100;
    if (g_game) {
        g_game->relayout(); // ViewFor gives 100 now
        g_game->clamp();    // not the drawn view (as Yongle: the next drawing writes it)
    }
}

bool CameraMoving()
{
    return Animating();
}

// The scale one step towards the wheel's target, laid out, and the view moved so the city point under the anchor
// (the cursor at the last notch) stays under it. At the map's edges view_clamp has the last word (the point then moves
// a little, as in Augustus). The camera's zoomFrame.
void CameraStep()
{
    if (!g_game) {
        return;
    }
    const camera::CameraGame &cam = *g_game->picture->camera;
    const bool smooth = camera::Smooth();
    picture::ZoomMap a{}, b{};
    const bool before = Factors(&a);
    // The view before the step's layout: Relayout clamps it to the new layout's limits at once (whole tiles), and the
    // anchor's shift below would move it a second time -- at the east and south edges, whose limits move with the
    // layout, the view then left the edge. The shift is applied to this view instead,
    // and the camera clamps once.
    const int viewX = *cam.vars.viewX, viewY = *cam.vars.viewY;
    if (!Advance(smooth)) {
        return;
    }
    const bool after = Factors(&b);
    if (before && after) {
        g_zoomShiftX += core::display::ZoomAnchorShift(g_anchorX, a.left, a.fx, b.fx);
        g_zoomShiftY += core::display::ZoomAnchorShift(g_anchorY, a.top, a.fy, b.fy);
    }
    // The east and south lines for the new layout before the camera moves: a step that
    // shrinks the city at the east or south edge keeps it pinned there, as the west and north edges are.
    picture::UpdateEdges(*g_game->picture);
    if (smooth) {
        const int dx = static_cast<int>(std::lround(g_zoomShiftX)), dy = static_cast<int>(std::lround(g_zoomShiftY));
        g_zoomShiftX -= dx;
        g_zoomShiftY -= dy;
        camera::MoveFrom(cam, viewX, viewY, dx, dy);
    }
    else { // the game's whole tiles: view_x by columns, view_y by row pairs
        const int colPx = *cam.vars.tileWidth + 2, rowPx = 2 * *cam.vars.tileHalfHeight;
        const int tx = static_cast<int>(std::lround(g_zoomShiftX / colPx));
        const int ty = static_cast<int>(std::lround(g_zoomShiftY / rowPx));
        g_zoomShiftX -= tx * colPx;
        g_zoomShiftY -= ty * rowPx;
        camera::ClearRemainder();
        *cam.vars.viewX = viewX + tx;
        *cam.vars.viewY = viewY + 2 * ty;
        cam.clampView(); // through the game layer's hook, as the camera clamps
        camera::SnapEdges(cam); // at the game's limit: on the east / south line
    }
    if (!Animating()) {
        g_zoomShiftX = g_zoomShiftY = 0;
    }
    camera::NoteView(cam); // also when only the layout's clamp moved it: not "someone else"
    // viewport_init (the relayout) set the drawn view's copy to the map's middle; mouse_to_grid looks tiles up from it
    // until city_render writes it again, so the cursor tile (the building preview) jumped meanwhile (found in
    // testing). The view as city_render will draw it.
    SyncDrawnView();
}

// The camera moves in buffer pixels while zoomed; on the screen as fast as at 100%. The camera's zoomScale.
bool CameraScale(double *fx, double *fy)
{
    picture::ZoomMap m;
    if (!g_game || !picture::ZoomNow(*g_game->picture, &m)) {
        return false;
    }
    *fx = m.fx;
    *fy = m.fy;
    return true;
}

// The game's view is the zoomed one (ViewFor through the layout), bufW x bufH pixels of city for the showW x showH
// pixels on the screen. The game draws it into our buffer, on the tile grid as always, with one more column and two
// more half rows for the remainder; while it draws, draw_buffer, the pitch, dd_width and dd_height are our buffer's
// (the blitters address rows with the pitch, and the image drawing culls against dd_width), and all are put back
// after. The visible part (from the remainder) is then shrunk (average or nearest) or enlarged (sharp bilinear) onto
// the screen. Before drawing, the view is checked against the map's range again (Recheck); a view that does not fit
// is not drawn at all (never larger than the map). A scaling that fails switches the zoom off: this frame shows the
// last frame's city (the game's drawing is not made again: in Zeus that drawing would count), the next is laid out at
// 100.
bool RenderZoomed(picture::RenderCall &c)
{
    if (!g_game) {
        return false;
    }
    Recheck();
    const int percent = g_now.percent;
    int extRight, extBottom;
    picture::Extent(&extRight, &extBottom);
    if (percent == 100 || extRight <= 0 || extBottom <= 0) {
        return false; // not zoomed: the usual drawing
    }
    const picture::PictureGame &p = *g_game->picture;
    const picture::PictureVars &v = p.vars;
    const camera::CameraVars &cv = p.camera->vars;
    if (picture::BufferUses() != g_zoomBufferUses) {
        g_zoomBufferUses = picture::BufferUses();
        g_zoomPitchPx = 0; // the usual drawing used the buffer since: cleared again below
    }
    BYTE *back = *v.drawBuffer;
    const int pitch = *v.screenPitch, width = pitch / 2, height = *v.ddHeight;
    const int halfH = *cv.tileHalfHeight, colPx = *cv.tileWidth + 2, rowPx = 2 * halfH;
    const int left = *v.clipLeft, top = *v.clipTop;
    const int cols = *v.widthTiles, rows = *v.heightRows;
    const int showRight = extRight < width ? extRight : width;
    const int showBottom = extBottom < height ? extBottom : height;
    const int showW = showRight - left, showH = showBottom - top;
    int bufW = 0, bufH = 0;
    BufferSize(&bufW, &bufH);
    const int right2 = left + (cols + 1) * colPx - 2, bottom2 = top + (rows + 1) * halfH;
    const int px = picture::RemX(), py = picture::RemY();
    const bool fits = ViewFits(cols, rows);
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
    // While the wheel moves the scale, the layout changes every frame. The buffer then keeps the largest scale's
    // pitch and rows, so it is not cleared whole every frame (36 MB at 150%); only what a larger layout adds is
    // cleared. When the movement ends the layout is the exact one again and the buffer is cleared once.
    const bool moving = Animating();
    if (moving) {
        const int capW = left + core::display::ZoomedSize(showW, g_game->cap) + 3 * colPx;
        const int capRows = top + core::display::ZoomedSize(showH, g_game->cap) + 8 * halfH;
        pitchPx = pitchPx > ((capW + 15) & ~15) ? pitchPx : (capW + 15) & ~15;
        pitchPx = pitchPx > g_zoomPitchPx ? pitchPx : g_zoomPitchPx;
        totalRows = totalRows > capRows ? totalRows : capRows;
        totalRows = totalRows > g_zoomRows ? totalRows : g_zoomRows;
    }
    const size_t size = static_cast<size_t>(pitchPx) * 2 * totalRows;
    BYTE *city = picture::Buffer(size);
    if (!city) {
        char why[96];
        std::snprintf(why, sizeof(why), "no memory for a %.1f MB city buffer", size / (1024.0 * 1024.0));
        Off(why); // laid out at 100 now; this frame is drawn the usual way (the game has not drawn yet)
        return false;
    }
    const bool newLayout = pitchPx != g_zoomPitchPx || totalRows != g_zoomRows || right2 != g_zoomRight2 ||
                           bottom2 != g_zoomBottom2;
    if (pitchPx != g_zoomPitchPx || (newLayout && !moving)) {
        std::memset(city, 0, size); // a new layout: no rows of the last one left around the map
        if (!moving) {
            core_log("zoom: city buffer %d x %d px (%.1f MB) for %d%%: view %d x %d (+1, +2 while drawing), clip "
                     "(%d,%d)-(%d,%d), shown %d x %d of it on %d x %d px",
                     pitchPx, totalRows, size / (1024.0 * 1024.0), percent, cols, rows, left, top, right2, bottom2,
                     bufW, bufH, showW, showH);
        }
    }
    else if (newLayout) { // moving: only the columns and rows the larger view adds
        WORD *rowsAt = reinterpret_cast<WORD *>(city);
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
    picture::NoteBuffer(pitchPx * 2, totalRows);
    c.r.Set(v.drawBuffer.ptr(), city);
    c.r.Set(v.screenPitch.ptr(), pitchPx * 2);
    c.r.Set(v.ddWidth.ptr(), pitchPx);
    c.r.Set(v.ddHeight.ptr(), totalRows);
    c.r.Set(v.clipRight.ptr(), right2);
    c.r.Set(v.clipBottom.ptr(), bottom2);
    c.r.Set(v.widthTiles.ptr(), cols + 1);
    c.r.Set(v.heightRows.ptr(), rows + 2);
    picture::SetRenderShown({left, top, showRight, showBottom}); // on the screen (conversions take screen positions)
    // Conversions made while the game draws (their input is a screen position) use the drawing's zoom.
    picture::SetRenderZoom({left, top, showRight, showBottom,
                            percent > 100 ? static_cast<double>(bufW) / showW : percent / 100.0,
                            percent > 100 ? static_cast<double>(bufH) / showH : percent / 100.0});
    c.r.Set(picture::InRenderFlag(), true);
    picture::DrawCity(p, c.vx, c.vy, c.orientation);
    c.r.Undo();
    picture::SetRenderZoom({});
    const std::int64_t start = core::diag::perf::Now();
    const WORD *src16 = reinterpret_cast<const WORD *>(city);
    const core::display::Pixels16 src{src16 + static_cast<size_t>(top + py) * pitchPx + left + px, pitchPx, bufW,
                                      bufH};
    WORD *screen = reinterpret_cast<WORD *>(back);
    bool scaled;
    if (percent > 100) {
        const core::display::Target16 dst{screen + static_cast<size_t>(top) * width + left, width, showW, showH};
        scaled = !g_failScaling && core::display::ShrinkRgb565(src, showW, showH, g_average, dst,
                                                               &core::platform::parallel::ForRows);
    }
    else {
        const core::display::Target16 dst{screen, width, width, height};
        const RECT clip{left, top, showRight, showBottom};
        scaled = !g_failScaling && core::display::ScaleRgb565(src, 100.0 / percent, true, dst, left, top, clip,
                                                              &core::platform::parallel::ForRows);
    }
    core::diag::perf::Add(kCityZoomPhase, core::diag::perf::Now() - start);
    if (!scaled) {
        char why[128];
        std::snprintf(why, sizeof(why), "the %d x %d px city could not be %s to %d x %d px", bufW, bufH,
                      percent > 100 ? "shrunk" : "enlarged", showW, showH);
        Off(why); // this frame keeps the last frame's city; the next is drawn at 100
    }
    return true;
}

void DragDrawZoom(core::diag::Restore &r)
{
    picture::ZoomMap m;
    if (g_game && picture::ZoomNow(*g_game->picture, &m)) {
        const picture::PictureVars &v = g_game->picture->vars;
        g_dragMap = m;
        g_dragScreenH = *v.ddHeight;
        r.Set(g_game->sidebarX.ptr(), core::display::ZoomToBuffer(*g_game->sidebarX, m.left, m.fx));
        r.Set(v.ddHeight.ptr(), core::display::ZoomToBuffer(*v.ddHeight, m.top, m.fy));
        r.Set(&g_dragLines, true);
    }
}

// While drag_select_draw draws zoomed (dd_height is the buffer's then): the drag's own map, taken before (with the
// city as shown the corner stopped at the buffer's height below 100% in testing).
bool DragLinesMap(picture::ZoomMap *m)
{
    if (!g_dragLines) {
        return false;
    }
    *m = g_dragMap;
    return true;
}

void LineDraw(int x1, int y1, int x2, int y2, int colour, void (*original)(int, int, int, int, int))
{
    if (!g_dragLines || !g_game) {
        original(x1, y1, x2, y2, colour);
        return;
    }
    x1 = core::display::ZoomToScreen(x1, g_dragMap.left, g_dragMap.fx);
    x2 = core::display::ZoomToScreen(x2, g_dragMap.left, g_dragMap.fx);
    y1 = core::display::ZoomToScreen(y1, g_dragMap.top, g_dragMap.fy);
    y2 = core::display::ZoomToScreen(y2, g_dragMap.top, g_dragMap.fy);
    // line_draw's pixels are dropped at dd_width / dd_height: the screen's for screen positions (found in testing: at
    // 50% the frame's lower part was cut off at the buffer's height). Put back by the drag's Restore too, if this
    // faults.
    const picture::PictureVars &v = g_game->picture->vars;
    const int bufferH = *v.ddHeight;
    *v.ddHeight = g_dragScreenH;
    original(x1, y1, x2, y2, colour);
    *v.ddHeight = bufferH;
}

void ShownClipCall(void (*original)())
{
    picture::ZoomMap m;
    if (!Installed() || !picture::ZoomNow(*g_game->picture, &m)) {
        original(); // not zoomed: the city clip is the screen's city (or the game's own layout)
        return;
    }
    struct Call {
        void (*original)();
        picture::ZoomMap m;
        core::diag::Restore r;
    } c{original, m, {}};
    if (core::diag::Guarded(
            g_feature,
            [](void *p) {
                Call &c = *static_cast<Call *>(p);
                const picture::PictureVars &v = g_game->picture->vars;
                c.r.Set(v.clipRight.ptr(), c.m.showRight);
                c.r.Set(v.clipBottom.ptr(), c.m.showBottom);
                c.original();
                c.r.Undo();
            },
            &c, &c.r)) {
        return;
    }
    Off("the build cost text could not be drawn");
    original();
}

void OnGameLoaded()
{
    const int was = g_requested;
    g_requested = g_target = g_start;
    g_wheel = 0;
    g_zoomShiftX = g_zoomShiftY = 0;
    if (!Installed()) {
        return;
    }
    core_log("zoom: a game was loaded: the starting zoom %d%% (was %d%%)", g_start, was);
    if (!core::diag::Guarded(g_feature, [](void *) {
            Relayout("a game was loaded");
            *g_game->redraw = 1;
        }, nullptr)) {
        Off("the view could not be laid out after loading a game");
    }
}

void SetForTest(int percent, int bufW, int bufH)
{
    g_now = core::display::ZoomLayout{percent, 0, 0, false, false, bufW, bufH, 0, 0};
}

void ResetForTest()
{
    g_feature.off.store(false);
    g_game = nullptr;
    g_requested = g_target = g_start = 100;
    g_average = true;
    g_wheel = 0;
    g_offDone = false;
    g_inputLogs = 0;
    g_now = core::display::ZoomLayout{100, 0, 0, false, true, 0, 0, 0, 0};
    g_lastMapCols = g_lastMapRows = g_lastClampRows = -1;
    for (int &k : g_logged) {
        k = -1;
    }
    g_zoomShiftX = g_zoomShiftY = 0;
    g_zoomPitchPx = g_zoomRows = g_zoomRight2 = g_zoomBottom2 = 0;
    g_zoomBufferUses = 0;
    g_loggedZoomSkip = false;
    g_failScaling = false;
    g_dragLines = false;
    g_dragMap = {};
}

void FailScalingForTest(bool fail)
{
    g_failScaling = fail;
}

} // namespace core::scroll::zoom
