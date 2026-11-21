#include "games/emperor/zoom.h"

#include "core/log.h"
#include "core/scroll/zoom.h"
#include "games/emperor/scroll.h"
#include "games/emperor/site.h"

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): view_clamp and the viewports read from the disassembly; names and
// calling conventions checked by disassembly. The zoom itself is the core's (core/scroll/zoom.h, moved
// there for Zeus): this file gives Emperor's addresses and hooks view_clamp.
namespace emperor::zoom {
namespace {

namespace czoom = core::scroll::zoom;

// view_clamp: __cdecl, no arguments; keeps view_x / view_y within the map's range minus the view's size.
const Site kViewClamp{"view_clamp", 0x53cbd0, {0x51, 0x53, 0x8b, 0x1d, 0xe0, 0xf6, 0x15, 0x01}};
// build_cost_text_draw(): __cdecl, no arguments, ret (checked by disassembly; its only caller is screen_draw
// 0x53776c, after the city; Zeus's 0x4542b0): the building preview's cost, clipped to the city clip 0xadf890..9c.
// Seen in testing: cut off at 50% (the clip is the zoomed buffer's).
const Site kCostText{"build_cost_text_draw", 0x46e000, {0xa1, 0xf0, 0xd0, 0x01, 0x01, 0x83, 0xec, 0x08}};
// save_game_load(path, 1): __cdecl, 2 arguments (the callers push 2 and pop 8), ret; game_frame 0x536d37 (/sg=), the
// file dialog 0x401aa3, starting a mission 0x516876 and two more call it (Zeus's save_game_load 0x50e610).
const Site kSaveGameLoad{"save_game_load", 0x534a30, {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff}};
const Var<int> kMapColMin{0x115f6e8}; // view_clamp's limits
const Var<int> kMapColMax{0x115f6e4};
const Var<int> kMapRowMin{0x115f6e0};
const Var<int> kMapRowMax{0x115f6dc};
const Var<char> kGameScreen{0x10de045};
const Var<char> kRedraw{0x10de036};
// view_clamp's bottom margin: map_bottom_margin 0x53d700 (__fastcall, ecx = 0xbebf38)
// reads this + 0x19628 + (0 for direction 0, 1 for 2, 2 for 4, 3 for any other) -- the highest tile along the edge at
// the bottom of the screen (0x535960). view_clamp's bottom limit is the map's minus (margin + 4) when it is not 0.
const Var<unsigned char> kBottomMargin{0xbebf38 + 0x19628}; // 4 bytes
const Var<int> kDirection{0x101d0d0};                       // 0, 2, 4, 6
// The view as city_render last drew it (its arguments), which mouse_to_grid adds to the cursor's view tile for the
// view_to_grid lookup. viewport_init sets it to the map's middle (its first two arguments halved, 0xe4 -> 0x72 and
// 0xe4), so after a relayout it is written again with the current view.
const Var<int> kViewDrawnX{0xadf8d8};
const Var<int> kViewDrawnY{0xadf8d4};
const Var<int> kSidebarX{0x13f8e0c};
constexpr int kZoomCap = 150; // larger views cost too much speed (seen in testing)

using Void_fn = void(__cdecl *)();
using SaveGameLoad_fn = int(__cdecl *)(int, int);
Void_fn o_ViewClamp, o_CostText;
SaveGameLoad_fn o_SaveGameLoad;

void (*g_relayout)() = nullptr;
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

// The half rows view_clamp can hold now: its bottom limit minus its top one. The
// margin is kept as the game has it: without it, the rows below raised ground at the bottom edge show leftovers of
// earlier frames (nothing is drawn there; seen in testing).
int ClampRows()
{
    const int m = Margin();
    return MapRows() - (m != 0 ? m + 4 : 0);
}

void Clamp()
{
    Original(o_ViewClamp, kViewClamp)();
}

void MapNow(int *cols, int *rows, int *clampRows)
{
    *cols = MapCols();
    *rows = MapRows();
    *clampRows = ClampRows();
}

// The city screens (game_screen 1 and the army screens 0x48 / 0x49). Not 7: that is a sidebar panel's dialog
// over the city (religion, monuments, commerce ...), whose lists scroll with the wheel in the game; it was first taken
// for a city variant, so the wheel zoomed the city behind such a dialog instead of scrolling its list (0.2.0 release
// test).
bool CityScreen()
{
    const char s = *kGameScreen;
    return s == 1 || s == 0x48 || s == 0x49;
}

void Relayout()
{
    if (g_relayout) {
        g_relayout();
    }
}

// Before every view_clamp: a new map (loading a game), a turn (another bottom margin) or a view larger than the map
// gets the view laid out again first (the core's OnViewClamp).
void __cdecl ViewClamp_detour()
{
    const int m = Margin() | MarginIndex() << 8;
    if (czoom::Active() && m != g_loggedMargin) { // while the zoom works (as before the move)
        g_loggedMargin = m;
        core_log("zoom: the bottom margin for direction %d is %d half row(s)%s", *kDirection, m & 0xff,
                 (m & 0xff) == 0 ? "" : " (+4: view_clamp keeps them below the screen)");
    }
    czoom::OnViewClamp();
    Clamp();
}

// The building preview's cost text in the city as shown (the core's ShownClipCall).
void __cdecl CostText_detour()
{
    czoom::ShownClipCall([] { Original(o_CostText, kCostText)(); });
}

// A saved game or a mission loaded: the starting zoom again.
int __cdecl SaveGameLoad_detour(int path, int flag)
{
    const int r = Original(o_SaveGameLoad, kSaveGameLoad)(path, flag);
    czoom::OnGameLoaded();
    return r;
}

// The zoom's view of Emperor (core/scroll/zoom.h).
const czoom::ZoomGame &Game()
{
    static const czoom::ZoomGame kZoom{
        "emperor",   &emperor::scroll::Picture(), kViewDrawnX, kViewDrawnY, kRedraw, kSidebarX, kZoomCap, &MapNow,
        &CityScreen, &Relayout,                   &Clamp,      &emperor::scroll::Working,
    };
    return kZoom;
}

} // namespace

void RegisterSettings()
{
    czoom::RegisterSettings();
}

bool Attach()
{
    if (!Matches(kViewClamp)) {
        core_log("zoom: %s does not match: the city stays at 100%%", kViewClamp.name);
        return false;
    }
    Install(kViewClamp, &ViewClamp_detour, o_ViewClamp);
    czoom::Install(Game());
    // Not needed for the zoom to work (logged when missing).
    if (!Matches(kCostText) || !Install(kCostText, &CostText_detour, o_CostText)) {
        core_log("zoom: %s not hooked: the build cost text is clipped to the zoomed buffer", kCostText.name);
    }
    if (!Matches(kSaveGameLoad) || !Install(kSaveGameLoad, &SaveGameLoad_detour, o_SaveGameLoad)) {
        core_log("zoom: %s not hooked: a loaded game keeps the zoom it had", kSaveGameLoad.name);
    }
    return true;
}

void InstallForTest()
{
    czoom::Install(Game());
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

void SetRelayout(void (*fn)())
{
    g_relayout = fn;
}

} // namespace emperor::zoom
