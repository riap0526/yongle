#include "games/emperor/zoom.h"

#include "core/diag/guard.h"
#include "core/display/city_zoom.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/scroll/smooth_scroll.h"
#include "core/settings/settings.h"
#include "games/emperor/scroll.h"
#include "games/emperor/site.h"

#include <cstdlib>
#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/archive/resolution_ui_r5.md section 8, names and
// calling conventions in docs/symbols.md.
namespace emperor::zoom {
namespace {

// view_clamp: __cdecl, no arguments; keeps view_x / view_y within the map's range minus the view's size.
const Site kViewClamp{"view_clamp", 0x53cbd0, {0x51, 0x53, 0x8b, 0x1d, 0xe0, 0xf6, 0x15, 0x01}};
const Var<int> kMapColMin{0x115f6e8}; // view_clamp's limits
const Var<int> kMapColMax{0x115f6e4};
const Var<int> kMapRowMin{0x115f6e0};
const Var<int> kMapRowMax{0x115f6dc};
const Var<char> kGameScreen{0x10de045};
const Var<char> kRedraw{0x10de036};
// view_clamp's bottom margin (resolution_ui_r5.md section 9.1): map_bottom_margin 0x53d700 (__fastcall, ecx = 0xbebf38)
// reads this + 0x19628 + (0 for direction 0, 1 for 2, 2 for 4, 3 for any other) -- the highest tile along the edge at
// the bottom of the screen (0x535960). view_clamp's bottom limit is the map's minus (margin + 4) when it is not 0.
const Var<unsigned char> kBottomMargin{0xbebf38 + 0x19628}; // 4 bytes
const Var<int> kDirection{0x101d0d0};                       // 0, 2, 4, 6
// The game's mouse mode: drag_select_start 0x44bb00 sets 0x4d ('M') for a military drag selection (0x44bb0d), the
// drag is drawn while it is 0x4d (0x54b46f) and drag_select_finish clears it (0x44c107). R5e's first test: the wheel
// zoomed during a drag although wheel messages with MK_LBUTTON are not taken (why MK_LBUTTON was not set then is not
// known).
const Var<unsigned char> kMouseMode{0x10de041};
constexpr unsigned char kMouseModeDragSelect = 0x4d;
const Var<int> kViewX{0x101d0ac};
const Var<int> kViewY{0x101d0b0};
// The view as city_render last drew it (its arguments), which mouse_to_grid adds to the cursor's view tile for the
// view_to_grid lookup. viewport_init sets it to the map's middle (its first two arguments halved, 0xe4 -> 0x72 and
// 0xe4), so after a relayout it is written again with the current view (resolution_ui_r5.md section 11.6).
const Var<int> kViewDrawnX{0xadf8d8};
const Var<int> kViewDrawnY{0xadf8d4};
constexpr int kZoomCap = 150; // larger views cost too much speed (R5b's first test)

using Void_fn = void(__cdecl *)();
Void_fn o_ViewClamp;

core::diag::Feature g_feature{"city zoom (games/emperor/zoom)"};
bool g_attached = false;
int g_requested = 100;
bool g_average = true;
void (*g_relayout)() = nullptr;

// R5d (resolution_ui_r5.md section 11): the wheel and the middle button move g_requested towards g_target, a little
// each frame (games/emperor/scroll calls Advance before the city is drawn), with the city point under g_anchor kept
// under the cursor. Not saved: city_zoom in emperor_engine.ini is the scale at start-up.
int g_target = 100;
int g_anchorX = 0, g_anchorY = 0;
int g_wheel = 0; // wheel delta short of a notch (high-resolution wheels send less than 120)
const char *g_by = "";
core::scroll::SmoothZoom g_motion;
int g_inputLogs = 0;
constexpr int kInputLogs = 30;

core::display::ZoomLayout g_now{100, 0, 0, false, true, 0, 0, 0, 0};
int g_lastMapCols = -1, g_lastMapRows = -1, g_lastClampRows = -1;
int g_logged[7] = {-1, -1, -1, -1, -1, -1, -1}; // percent, requested, map cols, map rows, clamp rows, show w, show h
int g_loggedMargin = -1;

int MapCols()
{
    return *kMapColMax - *kMapColMin;
}

int MapRows()
{
    return *kMapRowMax - *kMapRowMin;
}

int MarginIndex()
{
    const int d = *kDirection;
    return d == 0 ? 0 : d == 2 ? 1 : d == 4 ? 2 : 3;
}

int Margin()
{
    return kBottomMargin.ptr()[MarginIndex()];
}

// The half rows view_clamp can hold now: its bottom limit minus its top one (resolution_ui_r5.md section 9.1). The
// margin is kept as the game has it: without it, the rows below raised ground at the bottom edge show leftovers of
// earlier frames (nothing is drawn there; R5b's second test, r5b_city_zoom.md section 5).
int ClampRows()
{
    const int m = Margin();
    return MapRows() - (m != 0 ? m + 4 : 0);
}

void Clamp()
{
    Original(o_ViewClamp, kViewClamp)();
}

bool Animating()
{
    return g_attached && core::diag::Enabled(g_feature) && g_target != g_requested;
}

// The view laid out again, then view_x / view_y clamped to it at once (the game's own view_clamp, not our hook): a
// larger view at the map's edge must not reach past the map until the game clamps next. Not logged while the wheel
// moves the scale (every frame then).
void Relayout(const char *why)
{
    if (g_relayout && core::diag::Enabled(g_feature)) {
        if (!Animating()) {
            core_log("zoom: the city view is laid out again (%s)", why);
        }
        g_relayout();
        Clamp();
        *kViewDrawnX = *kViewX; // viewport_init set them to the map's middle (see kViewDrawnX)
        *kViewDrawnY = *kViewY;
    }
}

// Before every view_clamp: a new map (loading a game), a turn (another bottom margin) or a view larger than the map
// gets the view laid out again first, so the view is never clamped (and then drawn) larger than the map's range.
void __cdecl ViewClamp_detour()
{
    if (g_attached && core::diag::Enabled(g_feature)) {
        const int m = Margin() | MarginIndex() << 8;
        if (m != g_loggedMargin) {
            g_loggedMargin = m;
            core_log("zoom: the bottom margin for direction %d is %d half row(s)%s", *kDirection, m & 0xff,
                     (m & 0xff) == 0 ? "" : " (+4: view_clamp keeps them below the screen)");
        }
        if (g_requested != 100 &&
            (MapCols() != g_lastMapCols || MapRows() != g_lastMapRows || ClampRows() != g_lastClampRows)) {
            Relayout(ClampRows() != g_lastClampRows && MapRows() == g_lastMapRows ? "another bottom margin"
                                                                                  : "the map's range changed");
        }
    }
    Clamp();
}

bool ApplyZoom(int, const char *v, bool initial)
{
    const int p = std::strcmp(v, "auto") == 0 ? 100 : std::atoi(v);
    g_requested = p < core::display::kZoomMin ? core::display::kZoomMin
                  : p > core::display::kZoomMax ? core::display::kZoomMax
                                                : p;
    g_target = g_requested; // the panel's value at once (a wheel movement under way ends)
    if (!initial && g_attached) {
        Relayout("city_zoom changed");
        *kRedraw = 1;
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

// R5d: the wheel and the middle button over the city as shown, on the city screens (resolution_ui_r5.md section 8.1:
// 1, the building submenu 7, the army screens 0x48 / 0x49). Elsewhere (the sidebar: its building categories, other
// screens) the game has them as always.
bool CityScreen()
{
    const char s = *kGameScreen;
    return s == 1 || s == 7 || s == 0x48 || s == 0x49;
}

void MoveTowards(int target, int x, int y, const char *by)
{
    if (!Animating()) {
        g_requested = g_now.percent; // from the scale in effect (a larger value from the ini was capped)
    }
    if (++g_inputLogs <= kInputLogs) {
        core_log("zoom: %s at (%d,%d): %d%% -> %d%%%s", by, x, y, g_requested, target,
                 target == g_requested ? " (nothing to do)" : "");
    }
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

void TakeMouse(MouseCall &c)
{
    const bool wheel = c.m == WM_MOUSEWHEEL;
    if (!wheel && c.m != WM_MBUTTONDOWN && c.m != WM_MBUTTONDBLCLK) {
        return;
    }
    const int high = g_now.limit; // the cap and the map's limit (also worked out at 100%)
    if (!g_attached || !emperor::scroll::Working() || high <= 0 || !CityScreen() ||
        !emperor::scroll::OnCity(c.x, c.y)) {
        if (wheel) {
            g_wheel = 0;
        }
        return; // the game's own (the sidebar's categories)
    }
    c.taken = true;
    if (*kMouseMode == kMouseModeDragSelect) {
        g_wheel = 0; // R5e: not during a military drag selection (its start corner is in the scale it began in)
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
    if (!g_attached || !core::diag::Enabled(g_feature)) {
        return false;
    }
    MouseCall c{m, w, x, y, false};
    if (!core::diag::Guarded(g_feature, [](void *p) { TakeMouse(*static_cast<MouseCall *>(p)); }, &c)) {
        Off("the wheel could not be handled");
        return false;
    }
    return c.taken;
}

const char *const kShrink[] = {"average", "nearest"};
const char *const kShrinkLabels[] = {"average (each screen pixel the mean of the city pixels it covers)",
                                     "nearest (one city pixel each, sharper, may shimmer)"};

} // namespace

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "display", "city_zoom", "Starting city zoom (%)",
        "The city's zoom when the game starts: 100 as the game draws it, more to see more of the city, less to see "
        "it larger. From 50 to 150, and at most what the map allows. While playing, zoom with the mouse wheel over "
        "the city; the middle mouse button goes back to 100.",
        Type::Int, "100", Effect::Live, true, true, core::display::kZoomMin, core::display::kZoomMax, 5, nullptr,
        nullptr, nullptr, 0, "%", &LimitNow});
    core::settings::OnChange(i, &ApplyZoom);
    i = core::settings::Register(Item{
        "display", "city_zoom_shrink", "City zoom: shrinking",
        "When more than 100% is seen: how the city is made smaller on the screen.",
        Type::Choice, "average", Effect::Live, true, true, 0, 0, 0, nullptr, kShrink, kShrinkLabels, 2});
    core::settings::OnChange(i, &ApplyShrink);
}

bool Attach()
{
    if (!Matches(kViewClamp)) {
        core_log("zoom: %s does not match: the city stays at 100%%", kViewClamp.name);
        return false;
    }
    Install(kViewClamp, &ViewClamp_detour, o_ViewClamp);
    core::display::SetMouseFilter(&MouseFilter); // R5d: the wheel and the middle button over the city
    g_attached = true;
    core_log("zoom: city zoom installed (asked %d%%, shrinking %s)", g_requested, g_average ? "average" : "nearest");
    return true;
}

int ViewFor(int showW, int showH, int colPx, int halfH, int *cols, int *rows)
{
    g_lastMapCols = MapCols();
    g_lastMapRows = MapRows();
    g_lastClampRows = ClampRows();
    // The zoomed city is drawn by smooth scrolling's city render (games/emperor/scroll): without it, no zoom (the game
    // would draw the zoomed view straight into the back buffer).
    if (!g_attached || !core::diag::Enabled(g_feature) || g_requested == 100 || !emperor::scroll::Working()) {
        g_now = core::display::ZoomLayout{100, 0, 0, false, false, 0, 0, 0, 0};
        if (g_attached && colPx > 0 && halfH > 0) { // the limit for the panel's slider only
            g_now.limit = core::display::ZoomFor({100, showW, showH, colPx, halfH, g_lastMapCols, g_lastMapRows,
                                                  g_lastClampRows, kZoomCap})
                              .limit;
        }
        return 100;
    }
    const core::display::ZoomInput in{g_requested,  showW,         showH,           colPx,   halfH,
                                      g_lastMapCols, g_lastMapRows, g_lastClampRows, kZoomCap};
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
    if (!Animating() && std::memcmp(key, g_logged, sizeof(key)) != 0) { // R5d: once the wheel's movement ends
        std::memcpy(g_logged, key, sizeof(key));
        // The buffer the drawing will use (games/emperor/scroll): the view plus one column and a row pair, a little
        // margin; 2 bytes a pixel.
        const double mb = (static_cast<double>(g_now.cols + 1) * colPx + 8) * ((g_now.rows + 3) * halfH + 48) * 2 /
                          (1024.0 * 1024.0);
        core_log("zoom: city %d%% (asked %d%%%s) on %d x %d px, map range %d x %d (the clamp holds %d half rows): "
                 "up to %d%% allowed (cap %d%%, Augustus' limit %d%%); view %d x %d, zoomed city %d x %d px, buffer "
                 "about %.1f MB%s",
                 g_now.percent, g_requested, g_now.capped ? ", capped" : "", showW, showH, g_lastMapCols,
                 g_lastMapRows, g_lastClampRows, g_now.limit, kZoomCap, g_now.augustus, g_now.cols, g_now.rows,
                 g_now.bufW, g_now.bufH, mb,
                 g_now.unknownMap ? " (no map yet: 100%)"
                 : g_now.limit == 0 ? " -- THE MAP IS SMALLER THAN EVEN 50%: the game's own size"
                                    : "");
    }
    return g_now.percent;
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

void MapRange(int *cols, int *rows)
{
    *cols = MapCols();
    *rows = MapRows();
}

void ViewLimits(int *left, int *right, int *top, int *bottom, int *mapRows)
{
    *left = *kMapColMin;
    *right = *kMapColMax;
    *top = *kMapRowMin;
    *bottom = *kMapRowMin + ClampRows();
    *mapRows = MapRows();
}

bool AverageShrink()
{
    return g_average;
}

bool ViewFits(int cols, int rows)
{
    return core::display::ViewFitsMap(cols, rows, MapCols(), MapRows(), ClampRows());
}

void Off(const char *why)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    if (core::diag::Enabled(g_feature)) {
        core::diag::Disable(g_feature, why); // else switched off by its guard already (the wheel)
    }
    core_log("zoom: off for the rest of the run (%s): the city at 100%%", why);
    g_target = g_requested;
    g_now = core::display::ZoomLayout{100, 0, 0, false, false, 0, 0, 0, 0};
    if (g_relayout) {
        g_relayout(); // ViewFor gives 100 now
        Clamp();
    }
}

void ResetView()
{
    g_now = core::display::ZoomLayout{100, 0, 0, false, false, 0, 0, 0, 0};
}

void Recheck()
{
    if (!g_attached || g_requested == 100 || !core::diag::Enabled(g_feature)) {
        return;
    }
    if (MapCols() != g_lastMapCols || MapRows() != g_lastMapRows || ClampRows() != g_lastClampRows) {
        Relayout("the map's range or the bottom margin changed (drawing)");
    }
    else if (g_now.percent != 100 && !ViewFits(g_now.cols, g_now.rows)) { // cannot stay so: ViewFor caps it
        Relayout("the view does not fit the map (drawing)");
    }
}

void SetRelayout(void (*fn)())
{
    g_relayout = fn;
}

bool Moving()
{
    return Animating();
}

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
    if (!Animating()) {
        *kRedraw = 1;
    }
    return true;
}

void Anchor(int *x, int *y)
{
    *x = g_anchorX;
    *y = g_anchorY;
}

int Cap()
{
    return kZoomCap;
}
void SetForTest(int percent, int bufW, int bufH)
{
    g_now = core::display::ZoomLayout{percent, 0, 0, false, false, bufW, bufH, 0, 0};
}

} // namespace emperor::zoom
