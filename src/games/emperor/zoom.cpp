#include "games/emperor/zoom.h"

#include "core/capture/screenshot.h"
#include "core/diag/guard.h"
#include "core/display/city_zoom.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "games/emperor/scroll.h"
#include "games/emperor/site.h"

#include <cstdlib>
#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/resolution_ui/r5_city_zoom.md section 8, names and
// calling conventions in docs/symbols.md.
namespace emperor::zoom {
namespace {

// view_clamp: __cdecl, no arguments; keeps view_x / view_y within the map's range minus the view's size.
const Site kViewClamp{"view_clamp", 0x53cbd0, {0x51, 0x53, 0x8b, 0x1d, 0xe0, 0xf6, 0x15, 0x01}};
// city_render_tiles_at(view_x, view_y, ?, arg): __cdecl, 4 arguments, ret. A small city view drawn with its own
// viewport (0x53cac0: 5 x 15 small tiles) by 0x5a25c0, for a list of buildings (0x5a2670); which screen shows it is
// logged (R5b test).
const Site kRenderAt{"city_render_tiles_at", 0x46a320, {0x8b, 0x4c, 0x24, 0x08, 0x8b, 0x44, 0x24, 0x04}};

const Var<int> kMapColMin{0x115f6e8}; // view_clamp's limits
const Var<int> kMapColMax{0x115f6e4};
const Var<int> kMapRowMin{0x115f6e0};
const Var<int> kMapRowMax{0x115f6dc};
const Var<char> kGameScreen{0x10de045};
const Var<char> kRedraw{0x10de036};
// view_clamp's bottom margin (r5_city_zoom.md section 9.1): map_bottom_margin 0x53d700 (__fastcall, ecx = 0xbebf38)
// reads this + 0x19628 + (0 for direction 0, 1 for 2, 2 for 4, 3 for any other) -- the highest tile along the edge at
// the bottom of the screen (0x535960). view_clamp's bottom limit is the map's minus (margin + 4) when it is not 0.
const Var<unsigned char> kBottomMargin{0xbebf38 + 0x19628}; // 4 bytes
const Var<int> kDirection{0x101d0d0};                       // 0, 2, 4, 6
// The game's frame (for the small city view's screenshot).
const Var<BYTE *> kDrawBuffer{0x1156df8};
const Var<int> kScreenPitch{0x10de000};
const Var<int> kDdWidth{0x10de008};
const Var<int> kDdHeight{0x10de004};

constexpr int kZoomCap = 150; // larger views cost too much speed (R5b's first test)

using Void_fn = void(__cdecl *)();
using RenderAt_fn = void(__cdecl *)(int, int, int, int);
Void_fn o_ViewClamp;
RenderAt_fn o_RenderAt;

core::diag::Feature g_feature{"city zoom (games/emperor/zoom)"};
bool g_attached = false;
int g_requested = 100;
bool g_average = true;
void (*g_relayout)() = nullptr;

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

// The half rows view_clamp can hold now: its bottom limit minus its top one (r5_city_zoom.md section 9.1). The
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

// The view laid out again, then view_x / view_y clamped to it at once (the game's own view_clamp, not our hook): a
// larger view at the map's edge must not reach past the map until the game clamps next.
void Relayout(const char *why)
{
    if (g_relayout && core::diag::Enabled(g_feature)) {
        core_log("zoom: the city view is laid out again (%s)", why);
        g_relayout();
        Clamp();
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

// The small city view (0x46a320): where it is shown is not known yet (r5_city_zoom.md section 8.4); the first time,
// the frame it is in is saved at the next present.
bool g_loggedRenderAt = false;
bool g_shotPending = false;

void __cdecl RenderAt_detour(int vx, int vy, int a, int b)
{
    if (!g_loggedRenderAt) {
        g_loggedRenderAt = true;
        g_shotPending = true;
        core_log("zoom: city_render_tiles_at (a small city view with its own viewport) drawn on screen 0x%02x at "
                 "city zoom %d%%; the frame is saved as shots\\smallview_...",
                 static_cast<unsigned char>(*kGameScreen), g_now.percent);
    }
    Original(o_RenderAt, kRenderAt)(vx, vy, a, b);
}

bool ApplyZoom(int, const char *v, bool initial)
{
    const int p = std::strcmp(v, "auto") == 0 ? 100 : std::atoi(v);
    g_requested = p < core::display::kZoomMin ? core::display::kZoomMin
                  : p > core::display::kZoomMax ? core::display::kZoomMax
                                                : p;
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
        "display", "city_zoom", "City zoom (%)",
        "How much of the city is seen, in percent of the game's own size: 100 as the game draws it, 200 twice as much "
        "of the city (everything half the size), 50 half as much (everything twice the size). The sidebar and the "
        "menus keep their size. At most 150 (more of the city costs too much speed), and at most what the current "
        "map allows (the slider ends there; a larger value in emperor_engine.ini is shown as large as allowed).",
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
    if (!Matches(kViewClamp) || !Matches(kRenderAt)) {
        core_log("zoom: %s does not match: the city stays at 100%%",
                 Matches(kViewClamp) ? kRenderAt.name : kViewClamp.name);
        return false;
    }
    Install(kViewClamp, &ViewClamp_detour, o_ViewClamp);
    Install(kRenderAt, &RenderAt_detour, o_RenderAt);
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
    if (std::memcmp(key, g_logged, sizeof(key)) != 0) {
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

bool AverageShrink()
{
    return g_average;
}

bool ViewFits(int cols, int rows)
{
    return core::display::ViewFitsMap(cols, rows, MapCols(), MapRows(), ClampRows());
}

void OnPresent()
{
    if (!g_shotPending) {
        return;
    }
    g_shotPending = false;
    core::diag::Guarded(
        g_feature,
        [](void *) {
            const BYTE *back = *kDrawBuffer;
            if (back && *kScreenPitch > 0 && *kDdWidth > 0 && *kDdHeight > 0) {
                core::capture::SaveOne(back, *kScreenPitch, *kDdWidth, *kDdHeight, "smallview", "game");
            }
        },
        nullptr);
}

void Off(const char *why)
{
    if (!core::diag::Enabled(g_feature)) {
        return;
    }
    core::diag::Disable(g_feature, why);
    core_log("zoom: off for the rest of the run (%s): the city at 100%%", why);
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

void SetForTest(int percent, int bufW, int bufH)
{
    g_now = core::display::ZoomLayout{percent, 0, 0, false, false, bufW, bufH, 0, 0};
}

} // namespace emperor::zoom
