// R1: custom logical resolution (docs/resolution_ui/display_scale.md §6, §8; addresses and evidence: docs/symbols.md).
// R2b: the city screen's layout (docs/archive/resolution_ui_r2b.md).
//
// The game knows three modes (screen_mode_get_size 0x52b910: 1 = 640x480, 2 = 800x600, 3 = 1024x768). Mode 3 is
// made to return our logical resolution; screen_mode stays 3, so Emperor.inf keeps a value the original game
// reads (removing the DLL brings back plain 1024x768), and every `mode == 3` test in the game still takes the
// 1024x768 path. screen_set_mode derives the surface size, pitch and dialog centring from the returned size.
//
// Layout: about 20 places choose between an 800 and a 1024 layout with `cmp width, 1024 / jnz` and do nothing for
// any other width. For 12 of them (the sidebar's 1024 layout, panels centred in the city area) the jnz becomes jl,
// so any width of 1024 or more takes the 1024 layout (identical for 640, 800 and 1024); the others stay as they are
// (see kJumps). Hooked:
//   ui_set_sidebar_position 0x5b5a00: the sidebar starts where the city's clip ends (~60 readers of sidebar_x);
//   city_viewport_small/large 0x53ca20/0x53ca80: the city viewport (viewport_init 0x469820) gets as many columns
//                                     and rows as fit, with the formulas that give the original values at 640/800/1024;
//   city_frame_bars 0x5beb60, sidebar_draw_panels 0x53b3a0: the top bar and the space below and right of the
//                                     sidebar are filled (R2b: the sidebar backdrop's leather; no bottom edge, the
//                                     city reaches the screen's bottom);
//   info_window_place 0x5a1430: the building information window at any height; where it goes is a setting
//                                     ([ui] info_window: original / centered / cursor, R2b);
//   fullscreen_art_load 0x526040: front-end screens (fixed 1024x768 frames) run at 1024x768 (OnLoop).
// Shift+F7 (the game treats it as F7) cycles through a few settings at run time.
//
// Failure (CLAUDE.md; the table is docs/archive/resolution_ui_r2b.md §1): the game has laid itself out for the
// resolution, so R1 is not switched off as a whole. Each part runs under its own guard and only the failing part
// goes:
//   switching (mode size, changing it, Shift+F7, front-end frame) -> the game's own 1024x768 from now on (one mode
//      change on the next loop, the way the game's own F7 does it);
//   city layout (sidebar position, viewport) -> the 1024 layout's values at once, then the game's own 1024x768
//      (the game cannot lay out any other width by itself);
//   top bar / sidebar filler -> not drawn;
//   info window position -> the game's own position;
//   full-screen picture clamp -> that picture is not drawn, then the game's own 1024x768 (where it needs no clamp).
#include "games/emperor/resolution.h"

#include "games/emperor/fullscreen.h"
#include "games/emperor/scroll.h"

#include "core/assets/sg3.h"
#include "core/ddraw/surface_memory.h"
#include "core/diag/guard.h"
#include "core/diag/perf.h"
#include "core/display/logical_size.h"
#include "core/display/moved_rect.h"
#include "core/display/virtual_display.h"
#include "core/settings/settings.h"
#include "core/log.h"
#include "games/emperor/site.h"

#include <cstring>
#include <vector>

namespace emperor::resolution {

namespace {

// ---------------------------------------------------------------------------------------------
// Addresses

const Site kModeGetSize{"screen_mode_get_size", 0x52b910, {0x8b, 0x44, 0x24, 0x04, 0x83, 0xf8, 0x01, 0x75}};
const Site kSidebarPos{"ui_set_sidebar_position", 0x5b5a00, {0xa1, 0x1c, 0xe0, 0x0d, 0x01, 0x3d, 0x20, 0x03}};
const Site kViewportSmall{"city_viewport_small", 0x53ca20, {0xa1, 0x1c, 0xe0, 0x0d, 0x01, 0x3d, 0x80, 0x02}};
const Site kViewportLarge{"city_viewport_large", 0x53ca80, {0xa1, 0x1c, 0xe0, 0x0d, 0x01, 0x3d, 0x20, 0x03}};
const Site kChangeMode{"screen_change_mode", 0x476650, {0xa0, 0x45, 0xe0, 0x0d, 0x01, 0x3c, 0x0f, 0x74}};
const Site kViewportInit{"viewport_init", 0x469820, {0x8b, 0x4c, 0x24, 0x18, 0x8b, 0x44, 0x24, 0x08}};
const Site kInfoPlace{"info_window_place", 0x5a1430, {0x56, 0x8b, 0x35, 0x34, 0x2b, 0x31, 0x01, 0xc7}};
const Site kFrameBars{"city_frame_bars", 0x5beb60, {0xa1, 0x1c, 0xe0, 0x0d, 0x01, 0x83, 0xec, 0x08}};
const Site kSidebarDraw{"sidebar_draw_panels", 0x53b3a0, {0xa1, 0x20, 0x8e, 0x3f, 0x01, 0x85, 0xc0, 0x75}};
const Site kArtLoad{"fullscreen_art_load", 0x526040, {0x8b, 0x4c, 0x24, 0x04, 0x33, 0xc0, 0x83, 0xf9}};
const Site kImageGetId{"image_get_id", 0x408170, {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x8b, 0xc7}};
const Site kImageDraw{"image_draw", 0x413960, {0x8b, 0x44, 0x24, 0x18, 0x8b, 0x4c, 0x24, 0x14}};
const Site kImageSize{"image_get_size", 0x5cd800, {0x53, 0x55, 0x56, 0x8b, 0x74, 0x24, 0x10, 0x8b}};
// screen_select_initial_mode: the start-up settings, then Emperor.inf over them (screen_mode among them).
const Site kInitialMode{"screen_select_initial_mode", 0x4761a0, {0x53, 0x56, 0xbb, 0x01, 0x00, 0x00, 0x00, 0xbe}};

// `cmp width, 1024` ... `jnz`: the bytes from the compare to the jump opcode, and where the jump is.
// Left alone on purpose (the other width's path is the right one for a wider screen, as in the widescreen patch for
// 2560x1440 that was tested): 0x430714 (China map frame at fixed 1024 coordinates), 0x5b5a56/0x5b5bb6/0x5b5bd9
// (the strip at the bottom: 1024 px wide at 1024, the full width otherwise), 0x5b66ee (a top-bar piece at
// sidebar_x - 186, only at 1024).
struct JumpSite {
    DWORD address;  // the compare
    int jump;       // offset of the jnz (75 rel8 or 0f 85 rel32)
    BYTE expect[16];
};
const JumpSite kJumps[] = {
    {0x42fc3e, 10, {0x81, 0xf9, 0x00, 0x04, 0x00, 0x00, 0x5f, 0x5e, 0x5d, 0x5b, 0x75}},
    {0x484072, 5, {0x3d, 0x00, 0x04, 0x00, 0x00, 0x75}},
    {0x4872ae, 5, {0x3d, 0x00, 0x04, 0x00, 0x00, 0x75}},
    {0x489d7a, 9, {0x81, 0xf9, 0x00, 0x04, 0x00, 0x00, 0x8d, 0x78, 0xff, 0x75}},
    {0x50c3de, 5, {0x3d, 0x00, 0x04, 0x00, 0x00, 0x75}},
    {0x53b1d8, 12, {0x81, 0xf9, 0x00, 0x04, 0x00, 0x00, 0x89, 0x1d, 0xcc, 0x99, 0xa5, 0x00, 0x75}},
    {0x53b2c8, 6, {0x81, 0xf9, 0x00, 0x04, 0x00, 0x00, 0x75}},
    {0x53b608, 6, {0x81, 0xf9, 0x00, 0x04, 0x00, 0x00, 0x75}},
    {0x53b638, 6, {0x81, 0xf9, 0x00, 0x04, 0x00, 0x00, 0x75}},
    {0x5b6869, 5, {0x3d, 0x00, 0x04, 0x00, 0x00, 0x75}},
    {0x5b85e3, 5, {0x3d, 0x00, 0x04, 0x00, 0x00, 0x75}},
    {0x5beb6f, 5, {0x3d, 0x00, 0x04, 0x00, 0x00, 0x0f, 0x85}},
};

const Var<int> kScreenWidth{0x10de01c};
const Var<int> kScreenHeight{0x10de018};
const Var<int> kSidebarX{0x13f8e0c};
const Var<int> kSidebarX2{0x13f8e08};
const Var<char> kRedraw{0x10de036};
const Var<int> kMapColMin{0x115f6e8}; // view_clamp's limits
const Var<int> kMapColMax{0x115f6e4};
const Var<int> kMapRowMin{0x115f6e0};
const Var<int> kMapRowMax{0x115f6dc};
const Var<char> kGameScreen{0x10de045};
const Var<int> kScreenMode{0x10de0d8};
const Var<int> kSmallTiles{0x13f8e20};  // city_view_small_tiles
const Var<int> kCityClipRight{0xadf898}; // viewport_init's clip (exclusive), on the tile grid
const Var<int> kCityClipBottom{0xadf890};
const Var<int> kTileHalfH{0xadf8ac};     // tile_half_height
const Var<int> kInfoX{0x1312b40};       // info window, in pixels
const Var<int> kInfoY{0x1312b3c};
const Var<int> kInfoCols{0x1312b38};    // its width in 16 px blocks
const Var<int> kInfoRows{0x1312b34};    // its height in 16 px blocks
const Var<int> kMouseX{0x10de0a0};      // the game's mouse position (mouse_to_grid reads these)
const Var<int> kMouseY{0x10de09c};
const Var<BYTE *> kDrawBuffer{0x1156df8}; // draw_buffer (RGB565)
const Var<int> kScreenPitch{0x10de000};   // bytes per row
const Var<int> kDdWidth{0x10de008};
const Var<int> kDdHeight{0x10de004};
const DWORD kImages = 0x1c42130;          // the image manager (this)
constexpr int kGroupTopBar = 0x27e, kGroupSidebarLower = 0x274; // (the bottom edge 0x2d6 is not drawn)

constexpr int kMinW = 1024, kMinH = 768;          // the 1024 layout
constexpr int kSidebarWidth = 226, kSidebarX2Offset = 120; // sidebar_x2 - sidebar_x (918 - 798 at 1024)
constexpr int kSidebar1024 = 1024 - kSidebarWidth; // sidebar_x at 1024
constexpr int kViewCols = 231, kViewRows = 457;    // view_to_grid
constexpr int kTopBar = 40;                        // the city viewport starts below the top bar (y 0x28)
constexpr int kBottomEdge = 8; // the game's bottom edge (group 0x2d6, 8 px; drawn at 1024 only): its 768 rule

// Guards (core/diag): each part of R1 is switched off on its own.
core::diag::Feature g_switching{"resolution switching (games/emperor/resolution)"};
core::diag::Feature g_layout{"city layout at this resolution (games/emperor/resolution)"};
core::diag::Feature g_bars{"top bar (games/emperor/resolution)"};
core::diag::Feature g_filler{"sidebar filler (games/emperor/resolution)"};
core::diag::Feature g_message{"city message panel and bottom strip (games/emperor/resolution)"};
core::diag::Feature g_info{"info window position (games/emperor/resolution)"};
core::diag::Feature g_clamp{"full-screen picture clamp (games/emperor/resolution)"};

// ---------------------------------------------------------------------------------------------
// The setting in effect (game thread)

struct Setting {
    core_resolution_kind kind;
    int w, h;
    int scale; // 0 = auto
};
Setting g_setting{CORE_RESOLUTION_SCALED, 1024, 768, 100};
int g_lastW = 0, g_lastH = 0;

// Front-end screens (main menu, campaign and mission screens, loading, victory and defeat) are drawn for a fixed
// 1024x768 frame: the full-screen picture (fullscreen_art_load) is never centred on a larger screen (0x525cd0 only
// crops it to a smaller one) and their buttons sit at fixed 1024x768 positions. So while one is shown the game
// runs at 1024x768 and the output centres it; the setting applies again once the city screen is shown.
const Setting kFrontEnd{CORE_RESOLUTION_FIXED, 1024, 768, 0};
bool g_frontEnd = true; // until the first city screen

// R2e: windowed, the game is laid out for the window: its client area at rest divided by the display scale (as
// Augustus does; with a fixed resolution setting, scale 100%), never below 1024x768 (NativeSize is the client area).
Setting g_windowedSetting{CORE_RESOLUTION_SCALED, 0, 0, 100};

const Setting &Target()
{
    if (g_frontEnd) {
        return kFrontEnd;
    }
    if (core::display::Windowed()) {
        g_windowedSetting.scale = g_setting.kind == CORE_RESOLUTION_SCALED ? g_setting.scale : 100;
        return g_windowedSetting;
    }
    return g_setting;
}

bool CityScreen(char s) // the screens screen_draw draws the city for
{
    return s == 0x01 || s == 0x08 || s == 0x20 || s == 0x2d || s == 0x48 || s == 0x49;
}

core::display::LogicalSize Logical(const Setting &s)
{
    SIZE native = core::display::NativeSize();
    return core::display::ChooseLogicalSize(native.cx, native.cy, s.kind, s.w, s.h, s.scale, kMinW, kMinH);
}

void Describe(const Setting &s, char *out, int size)
{
    core::display::DescribeSetting(s.kind, s.w, s.h, s.scale, out, size);
}

using ChangeMode_fn = void(__cdecl *)(int);

// ---------------------------------------------------------------------------------------------
// Back to the game's own 1024x768

bool g_toGameSize = false; // a mode change to the game's own size is due (OnLoop)

// Switching is off from now on (mode 3 = the game's own 1024x768) and the game is laid out again at that size on
// the next loop. Used by every part whose fallback is the game's own size.
void ToGameSize(const char *why)
{
    core::diag::Disable(g_switching, why);
    if (!g_toGameSize && (*kScreenWidth != kMinW || *kScreenHeight != kMinH)) {
        g_toGameSize = true;
        core_log("resolution: back to the game's own 1024x768 on the next loop (%s)", why);
    }
}

// ---------------------------------------------------------------------------------------------
// Hooks

using ModeGetSize_fn = void(__cdecl *)(int);
using Void_fn = void(__cdecl *)();
using ViewportInit_fn = void(__cdecl *)(int, int, int, int, int, int, int, int);
ModeGetSize_fn o_ModeGetSize;
Void_fn o_SidebarPos, o_ViewportSmall, o_ViewportLarge;

struct ModeCall {
    core::diag::Restore r;
};

// Mode 3's size: ours (screen_width/height in c.r: the game's own 1024x768 comes back if this is abandoned).
void ModeSize(ModeCall &c)
{
    core::display::LogicalSize l = Logical(Target());
    c.r.Set(kScreenWidth.ptr(), l.w);
    c.r.Set(kScreenHeight.ptr(), l.h);
    if (l.w != g_lastW || l.h != g_lastH) {
        char what[64];
        Describe(Target(), what, sizeof(what));
        if (g_frontEnd) {
            std::strcat(what, ", front-end screen");
        }
        SIZE native = core::display::NativeSize();
        core_log("resolution: mode 3 = %dx%d (%s; native %ldx%ld, scale %d%%%s)", l.w, l.h, what, native.cx,
                 native.cy, l.scale_percent, l.limited ? ", limited to the 1024x768 minimum" : "");
        g_lastW = l.w;
        g_lastH = l.h;
    }
    c.r.Forget(); // the new size is the result
}

void __cdecl ModeGetSize_detour(int mode)
{
    Original(o_ModeGetSize, kModeGetSize)(mode);
    if (mode != 3 || !core::diag::Enabled(g_switching)) {
        return; // the game's own size
    }
    ModeCall c;
    if (!core::diag::Guarded(g_switching, [](void *p) { ModeSize(*static_cast<ModeCall *>(p)); }, &c, &c.r)) {
        ToGameSize("the mode size could not be worked out");
    }
}

// City columns for the 78x40 tiles: as many as fit left of the sidebar (1024 -> 10, 800 -> 7).
int LargeColumns(int w)
{
    return (w - kSidebarWidth + 2) / 80;
}

// Where the city is to be seen to (R2b.1-5 / -6, games/emperor/scroll SetCityExtent), after the viewport or the
// sidebar changed: with our layout the sidebar's left edge (large tiles; small tiles have no sidebar: the screen's
// right edge) and the screen's bottom; the game's own layouts and our fallback: just the clip. After the viewport
// (not the sidebar, which the game sets before it: the clip is still the last mode's then) also the spare rows the
// city may draw into (core/ddraw/surface_memory.h): the clip's overshoot of the screen (the game's own small tiles:
// 775 at 768) or the next half row past the bottom; never more than a row pair.
int g_lastAllowance = -1;
int g_extRight = 0, g_extBottom = 0;
bool StandardSize(int w, int h); // below

void UpdateCityExtent(bool viewport)
{
    const int w = *kScreenWidth, h = *kScreenHeight;
    const bool ours = !(StandardSize(w, h) || w < 1024) && core::diag::Enabled(g_layout);
    const bool small = *kSmallTiles == 1;
    const int right = ours ? (small ? w : *kSidebarX) : 0, bottom = ours ? h : 0;
    g_extRight = right;
    g_extBottom = bottom;
    emperor::scroll::SetCityExtent(right, bottom);
    if (!viewport) {
        return;
    }
    const int clip = *kCityClipBottom, halfH = *kTileHalfH;
    int reach = clip;
    if (bottom > clip && halfH > 0) {
        reach = clip + (bottom - clip + halfH - 1) / halfH * halfH;
    }
    int allowance = reach > h ? reach - h : 0;
    if (halfH > 0 && allowance > 2 * halfH) {
        allowance = 2 * halfH; // not a clip of this mode
    }
    core::ddraw::SetCanaryAllowance(allowance);
    if (allowance != g_lastAllowance) {
        g_lastAllowance = allowance;
        core_log("resolution: the city may draw %d row(s) below the screen (spare rows; city clip bottom %d, shown to "
                 "%d)",
                 allowance, clip, bottom ? bottom : clip);
    }
}

// The layout part failed: the 1024 layout's values now (valid on any screen of 1024x768 or more), the game's own
// 1024x768 from the next loop.
void LayoutFallBack(const char *what)
{
    ToGameSize(what);
}

// The sidebar against the screen's right edge (R2b.1-6; before: where the city's clip ends, columns x 80 - 2, which
// left up to 79 px of filler on its right at sizes that are not 224 + a multiple of 80). The city's clip stays on
// the tile grid (whole columns, LargeColumns); the gap between it and the sidebar is drawn by our city render
// (games/emperor/scroll, SetCityExtent), and filled with leather by SidebarDraw_detour when that is off.
void __cdecl SidebarPos_detour()
{
    int w = *kScreenWidth;
    if (w <= 1024) {
        Original(o_SidebarPos, kSidebarPos)();
        return;
    }
    core::diag::Restore r;
    if (core::diag::Guarded(
            g_layout,
            [](void *p) {
                auto &r = *static_cast<core::diag::Restore *>(p);
                r.Set(kSidebarX.ptr(), *kScreenWidth - kSidebarWidth);
                r.Set(kSidebarX2.ptr(), *kSidebarX + kSidebarX2Offset);
                r.Forget();
            },
            &r, &r)) {
        UpdateCityExtent(false);
        return;
    }
    *kSidebarX = kSidebar1024;
    *kSidebarX2 = kSidebar1024 + kSidebarX2Offset;
    LayoutFallBack("the sidebar position could not be set");
}

struct ViewportCall {
    int cols, rows, tileW, tileH;
};

void SetViewport(int cols, int rows, int tileW, int tileH)
{
    int maxCols = kViewCols - 11, maxRows = kViewRows - 30; // the draw loop spans cols -4..+6 and rows -8..+21
    if (cols > maxCols) cols = maxCols;
    if (rows > maxRows) rows = maxRows;
    Function<ViewportInit_fn>(kViewportInit)(0xe4, 0xe4, 0, kTopBar, cols, rows, tileW, tileH);
    int mapCols = *kMapColMax - *kMapColMin, mapRows = *kMapRowMax - *kMapRowMin;
    core_log("resolution: city viewport %d x %d (tiles %dx%d) for %dx%d; current map view range %d x %d%s", cols,
             rows, tileW, tileH, *kScreenWidth, *kScreenHeight, mapCols, mapRows,
             mapCols > 0 && (cols > mapCols || rows > mapRows) ? " -- VIEWPORT LARGER THAN THE MAP" : "");
}

// Guarded viewport_init; the fallback calls it again with the game's own 1024 values (everything viewport_init
// writes is written again, so nothing is put back first).
void Viewport(ViewportCall c, ViewportCall at1024)
{
    if (core::diag::Guarded(
            g_layout,
            [](void *p) {
                auto &v = *static_cast<ViewportCall *>(p);
                SetViewport(v.cols, v.rows, v.tileW, v.tileH);
            },
            &c)) {
        return;
    }
    Function<ViewportInit_fn>(kViewportInit)(0xe4, 0xe4, 0, kTopBar, at1024.cols, at1024.rows, at1024.tileW,
                                              at1024.tileH);
    LayoutFallBack("the city viewport could not be set");
}

// The game's own sizes, laid out by the game itself. Not the width alone (before O1): a window 1024 wide and taller
// than 768 got the game's 1024x768 viewport, and the city ended at row 760 with black below (playtest 2026-10-28).
bool StandardSize(int w, int h)
{
    return (w == 640 && h == 480) || (w == 800 && h == 600) || (w == 1024 && h == 768);
}

void __cdecl ViewportSmall_detour() // 58x30 tiles: 640/800/1024 -> 10x31, 13x39, 17x50
{
    int w = *kScreenWidth, h = *kScreenHeight;
    if (StandardSize(w, h) || w < 1024) {
        Original(o_ViewportSmall, kViewportSmall)();
    }
    else {
        Viewport({w / 60, (h - kTopBar) / 15 + 2, 0x3a, 0x1e}, {17, 50, 0x3a, 0x1e});
    }
    UpdateCityExtent(true);
}

void __cdecl ViewportLarge_detour() // 78x40 tiles: 800 -> 7x29, 1024 -> 10x37 (up to the sidebar)
{
    int w = *kScreenWidth, h = *kScreenHeight;
    if (StandardSize(w, h) || w < 1024) {
        Original(o_ViewportLarge, kViewportLarge)();
        UpdateCityExtent(true);
        return;
    }
    Viewport({LargeColumns(w), (h - kTopBar) / 20 + 1, 0x4e, 0x28}, {10, 37, 0x4e, 0x28});
    UpdateCityExtent(true);
}

// ---------------------------------------------------------------------------------------------
// Drawing the parts the 1024 layout does not cover (images of the game, drawn with its own functions)

using ImageGetId_fn = int(__thiscall *)(void *, int);
using ImageDraw_fn = void(__cdecl *)(int, int, int, int, int, int);
using ImageSize_fn = void(__thiscall *)(void *, int, int *, int *);

void *Images()
{
    return reinterpret_cast<void *>(kImages + g_imageShift);
}

int ImageId(int group)
{
    return Function<ImageGetId_fn>(kImageGetId)(Images(), group);
}

void ImageSize(int id, int *w, int *h)
{
    *w = *h = 0;
    Function<ImageSize_fn>(kImageSize)(Images(), id, w, h);
}

void Draw(int id, int x, int y)
{
    Function<ImageDraw_fn>(kImageDraw)(id, x, y, 0, 0, 0);
}

// Tiles an image along a row from x = right - width down to x <= left - width (right-aligned, as the game's
// 800x600 branch aligns the top bar to the sidebar).
void TileRowLeft(int id, int left, int right, int y)
{
    int w, h;
    ImageSize(id, &w, &h);
    if (w <= 0) {
        return;
    }
    for (int x = right - w; x > left - w; x -= w) {
        Draw(id, x, y);
    }
}

Void_fn o_FrameBars, o_SidebarDraw, o_InfoPlace;

// city_frame_bars 0x5beb60: at 1024 the top bar (group 0x27e, 798 px) at (0,0) and the city's bottom edge (0x2d6,
// 8 px high) at (0,760), the bottom of a 768 screen; at 800 the top bar right-aligned to the sidebar; nothing at
// any other width. Wider: only the top bar, tiled from the sidebar to the left edge. No bottom edge: the city's clip
// ends at the screen's bottom here (viewport_init: (rows - 1) x 20 + 40 >= the height; at 1024x768 it is 760, the
// edge's top), so the edge would only cover the city -- and only until a dialog screen, which does not redraw the
// top bar, lets the city draw over it (playtest 2026-10-24: a thin frame at the bottom the original does not have; at
// 800x600 the original has none either). The strip 0x5b5a40 blanks at the bottom (full width x 6 rows) is drawn
// over by the city every frame and by the sidebar filler.
void __cdecl FrameBars_detour()
{
    int w = *kScreenWidth;
    if (w < 1024 || (w == 1024 && *kScreenHeight == 768)) { // 1024 and taller: the bottom edge would be mid-city
        Original(o_FrameBars, kFrameBars)();
        return;
    }
    if (!core::diag::Guarded(
            g_bars,
            [](void *) {
                TileRowLeft(ImageId(kGroupTopBar), 0, *kSidebarX, 0);
            },
            nullptr)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            core_log("resolution: the top bar is no longer drawn after an error");
        }
    }
}

// The sidebar backdrop of China_Interface (entry 2045, 226x768, the light leather panel from playtests), read from the
// player's own pack in memory (core/assets/sg3) so that rows can be drawn on their own (the game's image_draw only
// draws whole images). Its rows [docs/archive/resolution_ui_r2b.md §2.1, from the pixels]: 0..14 the blue-white fret
// band (as on the top bar), 15..37 leather without the edge, 38..39 the corner where the left edge starts, 40..767
// leather with the sidebar's own left edge (column 0 dark, 1 light, 2 highlight -- the same as the sidebar panels).
// Below the sidebar: rows 40.. only (no band; playtest 2026-10-24: not drawn), and the lower panel's own last 7 rows
// (y 761..767, where its edge turns into a corner) get the edge columns too, so the line runs on to the bottom.
// Right of the sidebar: rows 0..39 (the band continues the top bar's), then rows 40.., from column 3 (plain leather:
// no edge line next to the sidebar; playtest 2026-10-24, the original has none there).
constexpr wchar_t kInterfacePack[] = L"DATA\\China_Interface.sg3"; // next to Emperor.exe
constexpr int kBackdropEntry = 2045, kEdgeFirst = 40, kEdgeCols = 3;
constexpr int kLowerPanelCorner = 768 - 7; // the lower panel (group 0x274, 226x310 at y 458) ends its edge here
struct Backdrop {
    bool tried = false, ok = false;
    std::vector<WORD> px; // kSidebarWidth x 768, RGB565
};
Backdrop g_backdrop;
const wchar_t *g_packForTest = nullptr;

bool LoadBackdrop()
{
    if (g_backdrop.tried) {
        return g_backdrop.ok;
    }
    g_backdrop.tried = true;
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (len > 0 && path[len - 1] != L'\\') {
        --len;
    }
    path[len] = L'\0';
    if (len + sizeof(kInterfacePack) / sizeof(wchar_t) >= MAX_PATH) {
        return false;
    }
    std::wcscat(path, kInterfacePack);
    if (g_packForTest) {
        std::swprintf(path, MAX_PATH, L"%ls", g_packForTest);
    }
    core::assets::Sg3Pack pack;
    int w = 0, h = 0;
    if (!pack.Open(path) || !pack.Size(kBackdropEntry, &w, &h) || w != kSidebarWidth || h != 768 ||
        !pack.Decode565(kBackdropEntry, 0, &g_backdrop.px)) {
        core_log("resolution: sidebar backdrop (China_Interface entry %d, %dx%d) not read: the filler is drawn whole "
                 "with the game's image_draw (its band then shows at y 768)",
                 kBackdropEntry, w, h);
        return false;
    }
    g_backdrop.ok = true;
    core_log("resolution: sidebar backdrop read (China_Interface entry %d, 226x768, in memory only): leather with the "
             "sidebar's edge below and right of the sidebar, no band at the bottom",
             kBackdropEntry);
    return true;
}

} // namespace

int SidebarBackdropRow(int y, int top)
{
    if (top == 0 && y < kEdgeFirst) {
        return y; // the band and the corner, continuing the top bar
    }
    const int period = 768 - kEdgeFirst, first = top == 0 ? kEdgeFirst : top;
    return kEdgeFirst + ((y - first) % period + period) % period;
}

namespace {

int BackdropRow(int y, int top, int)
{
    return SidebarBackdropRow(y, top);
}

// Draws the backdrop into draw_buffer: columns [x0, x1) from source column sx0 on, rows [top, h).
void DrawBackdrop(int x0, int x1, int sx0, int top, int h)
{
    BYTE *buf = *kDrawBuffer;
    const int pitch = *kScreenPitch, bufH = *kDdHeight;
    if (!buf || x0 >= x1 || x0 < 0 || x1 * 2 > pitch || h > bufH) {
        return;
    }
    for (int y = top; y < h; ++y) {
        const WORD *src = g_backdrop.px.data() + static_cast<size_t>(BackdropRow(y, top, h)) * kSidebarWidth;
        WORD *dst = reinterpret_cast<WORD *>(buf + static_cast<size_t>(y) * pitch);
        for (int x = x0; x < x1; ++x) {
            int sx = sx0 + (x - x0) % (kSidebarWidth - sx0);
            dst[x] = src[sx];
        }
    }
}

// The filler drawn whole with the game's image_draw (when the pack cannot be read: R1's way).
int g_fillerId = -1;
int g_fillerW = 0, g_fillerH = 0;

void FindFiller()
{
    if (g_fillerId >= 0) {
        return;
    }
    int bar = ImageId(kGroupTopBar), bw, bh;
    ImageSize(bar, &bw, &bh);
    int candidate = bar + (2045 - 1221), cw, ch;
    ImageSize(candidate, &cw, &ch);
    if (bw == 798 && bh == 40 && cw == kSidebarWidth && ch == 768) {
        g_fillerId = candidate;
    }
    else {
        g_fillerId = ImageId(kGroupSidebarLower);
    }
    ImageSize(g_fillerId, &g_fillerW, &g_fillerH);
    core_log("resolution: sidebar filler image %d (%dx%d; top bar %dx%d, candidate %dx%d)", g_fillerId, g_fillerW,
             g_fillerH, bw, bh, cw, ch);
}

// city: the city screen's sidebar (sidebar_draw_panels); otherwise another screen's (the China map's, R2c), which
// never has the city's gap left of the sidebar.
void DrawFiller(bool city)
{
    const int w = *kScreenWidth, h = *kScreenHeight, x = *kSidebarX;
    const bool below = *kSmallTiles == 0 && h > 768; // the sidebar's own panels end at 768
    if (LoadBackdrop()) {
        if (below) {
            DrawBackdrop(x, x + kEdgeCols, 0, kLowerPanelCorner, 768); // the lower panel's corner: the edge goes on
            DrawBackdrop(x, x + kSidebarWidth, 0, 768, h);
        }
        // Right of the sidebar: plain leather, without the backdrop's edge columns (dark, light, highlight). Since
        // R2b.1-6 the sidebar is against the right edge, so nothing is left here at our sizes.
        DrawBackdrop(x + kSidebarWidth, w, kEdgeCols, 0, h);
        // Left of the sidebar, between the city's clip and the sidebar: drawn by our city render; leather only when
        // that is off after an error (else this would show for a frame under the city on every background redraw).
        if (city && *kSmallTiles == 0 && !emperor::scroll::Working() && *kCityClipRight < x) {
            DrawBackdrop(*kCityClipRight, x, kEdgeCols, kTopBar, h);
        }
        return;
    }
    FindFiller();
    if (g_fillerW <= 0 || g_fillerH <= 0) {
        return;
    }
    if (below) {
        for (int y = 768; y < h; y += g_fillerH) {
            Draw(g_fillerId, x, y);
        }
    }
    for (int gx = x + kSidebarWidth; gx < w; gx += g_fillerW) {
        for (int y = 0; y < h; y += g_fillerH) {
            Draw(g_fillerId, gx, y);
        }
    }
}

// sidebar_draw_panels 0x53b3a0: the sidebar's upper panel (0x27f) at y 0 and lower panel (0x274) at y 458, which
// ends at 768. Below that and right of the sidebar the backdrop is drawn.
void __cdecl SidebarDraw_detour()
{
    Original(o_SidebarDraw, kSidebarDraw)();
    int w = *kScreenWidth, h = *kScreenHeight;
    if ((w <= 1024 && h <= 768) || !core::diag::Enabled(g_filler)) {
        return;
    }
    if (!core::diag::Guarded(g_filler, [](void *) { DrawFiller(true); }, nullptr)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            core_log("resolution: the space below and right of the sidebar is no longer filled after an error");
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The building information window (info_window_place 0x5a1430)

InfoMode g_infoMode = InfoMode::Original;

// The four kinds of building the game always opens at (0, 0x2a): info_window_place's second special case,
// FUN_005d3690 (types 0x35 Mill, 0x36 Warehouse, 0x38 Trading Quay, 0x3a Trading Station; names from
// EmperorText.eng group 0x1c). Their window code relies on that position in places (docs/archive/resolution_ui_r2b.md
// §3.2): the storage window draws its employee panel at y (rows - 4) x 16 and the trade window at (rows - 1) x 16,
// without the window's y; their goods lists are laid out once and kept. So for these the game keeps its own position
// and the window is moved only on screen: drawn into a copy of the screen and its rectangle copied to where it is
// shown, the mouse moved back by the same amount while the window draws and handles input. Everything the game
// computes stays exactly as in the original.
const Site kSpecialPlace{"info_window_fixed_kind", 0x5d3690, {0x8b, 0x44, 0x24, 0x04, 0xb9, 0x34, 0x76, 0x8c}};
const Site kStorageDraw{"storage_window_draw", 0x5d74e0, {0x83, 0xec, 0x0c, 0x53, 0x8b, 0xd9, 0x55, 0x56}};
const Site kTradeDraw{"trade_window_draw", 0x5e0610, {0x83, 0xec, 0x10, 0x56, 0x57, 0x8b, 0xf9, 0x33}};
const Site kMillDraw{"mill_window_draw", 0x556e50, {0x83, 0xec, 0x0c, 0x53, 0x55, 0x56, 0x57, 0x8b}};
// The mill window's foreground (vtable 0x7b72c8 slot 0x180, called every frame by info_window_draw 0x5a2391 with the
// game's x, y, cols, rows; __thiscall, ret 0x10) draws the food list itself (text 0x528ed0, Accept / Get buttons) at
// the places laid out by the window draw. The storage (0x5d6cb0) and trade (0x5e0bd0) foregrounds only update their
// lists and draw nothing (archive/resolution_ui_r2b.md §3.4), so only this one is moved as well.
const Site kMillForeground{"mill_window_foreground", 0x556a80, {0x81, 0xec, 0xd0, 0x01, 0x00, 0x00, 0x53, 0x55}};
// The window classes of the four kinds (factory FUN_005d3580): Mill 0x554d40 (vtable 0x7b72c8, draw 0x556e50),
// Trading Quay 0x5e1730 / Trading Station 0x5e1420 (draw 0x5e0610), Warehouse 0x5d61e0 (vtable 0x7be1bc, draw
// 0x5d74e0). The market squares (0x3b, 0x3c) come from the same factory but are placed by the game like any other
// window and draw relative to it (0x545a20).
const Site kTradePopup{"trade_popup_draw", 0x5dd810, {0xa1, 0x14, 0xe0, 0x0d, 0x01, 0x53, 0x8b, 0xd9}};
const Site kInfoInput{"info_window_input", 0x5a23a0, {0xa1, 0x1c, 0x2b, 0x31, 0x01, 0xb9, 0x34, 0x76}};
const Site kButtonsDraw{"buttons_draw", 0x449c10, {0x8b, 0x44, 0x24, 0x10, 0x85, 0xc0, 0x0f, 0x8e}};
// R2b.1-5 / -6 (docs/archive/resolution_ui_r2b1.md): the city message panel (screen 0x05) and city_draw_background's strip.
// city_message_panel_layout 0x50bf10 (__cdecl, no arguments, ret): sizes the panel to the city clip's width (in 16 px
// steps), centres it in the clip and puts it on the clip's bottom; it also draws the city (city_draw_view) and then,
// on a redraw, city_draw_background. fill_rect 0x5296f0 (__cdecl x, y, w, h, colour; ret): city_draw_background
// 0x5b5a40 clears the screen's bottom 6 rows with it (the call at 0x5b5a80, returning to 0x5b5a85; 9 rows at 1024
// wide), which in screen 0x05 comes after the city and stays black.
const Site kMessagePanel{"city_message_panel_layout", 0x50bf10, {0x81, 0xec, 0xb8, 0x05, 0x00, 0x00, 0x55, 0x57}};
const Site kFillRect{"fill_rect", 0x5296f0, {0x8b, 0x54, 0x24, 0x0c, 0x53, 0x56, 0x57, 0x85}};
constexpr DWORD kBackgroundStripCall = 0x5b5a85;
constexpr DWORD kInfoButtonsCall = 0x5378ef;       // screen_draw case 9: buttons_draw(x, y + rows*16 - 0x28, ...)
const Var<char> kTradePopupOpen{0x1e50a3c};        // the trade window's modal popup is open (FUN_005e0bc0)
const Var<int> kInfoKind{0x1312b04};               // 2: the building's own window class
const Var<int> kInfoBuilding{0x1312b1c};
const Var<char> kInfoPrevScreen{0x1312b44}; // game_screen before 9 (0x5a15e0 at 0x5a164e; restored when it closes)
constexpr char kScreenInfo = 0x09;
// building_get 0x47f1b0: __thiscall on the building list 0x8c7634 (as its callers, e.g. 0x5a1e17), ret 4; returns
// &list->array[id] (`mov eax, [ecx+4]; lea eax, [eax+id*4]`), a slot holding the building object's pointer.
const Site kBuildingGet{"building_get", 0x47f1b0, {0xe8, 0x5b, 0x90, 0x07, 0x00, 0x8b, 0x4c, 0x24}};
const DWORD kBuildingList = 0x8c7634;

// The building's window draw (vtable slot 0x178, called by screen 9's background draw 0x5a20c0) is one of ours: only
// then can the window be shown elsewhere (its drawing, input and buttons move together).
bool DrawIsHooked()
{
    void *list = reinterpret_cast<void *>(kBuildingList + g_imageShift);
    void **holder = Function<void **(__thiscall *)(void *, int)>(kBuildingGet)(list, *kInfoBuilding);
    if (!holder || !*holder) {
        return false;
    }
    void **vtable = *static_cast<void ***>(*holder);
    const DWORD draw = GameAddress(vtable[0x178 / 4]);
    return draw == 0x5d74e0 || draw == 0x5e0610 || draw == 0x556e50;
}
constexpr int kFixedX = 0, kFixedY = 0x2a;

// The on-screen shift of a fixed-position window (0, 0 when it is shown where the game has it).
struct Shift {
    bool on = false;
    int dx = 0, dy = 0;
};
Shift g_shift;

bool Shifted()
{
    return g_shift.on && (g_shift.dx || g_shift.dy) && *kGameScreen == kScreenInfo && core::diag::Enabled(g_info);
}

// info_window_place sets the window's position only for heights 600 and 768 (at 768: x 188 (78x40 tiles) or 248
// (58x30 tiles), y at the bottom (760 - rows x 16) while the cursor is in the upper part of the city, else 42); any
// other height keeps its defaults (8, 32), the top left corner. Original: the same rule at any height, x moved with
// the centre of the city area (R1). Centered / cursor: InfoPosition; for the fixed-position kinds the game keeps
// (0, 0x2a) and the window is shifted on screen.
void PlaceInfo(core::diag::Restore &r)
{
    r.Save(kInfoX.ptr()); // the game's own position comes back if this is abandoned
    r.Save(kInfoY.ptr());
    g_shift = Shift{};
    const int w = *kScreenWidth, h = *kScreenHeight;
    const bool small = *kSmallTiles == 1;
    const bool fixed = *kInfoKind == 2 && *kInfoX == kFixedX && *kInfoY == kFixedY &&
                       (Function<int(__cdecl *)(int)>(kSpecialPlace)(*kInfoBuilding) & 0xff) != 0;
    if (g_infoMode != InfoMode::Original) {
        // Opened on the China map (the trade window from the sidebar's Order button, R2d): in the middle of the map
        // area, whatever the setting (beside the cursor it would sit at the sidebar's edge).
        const char from = *kInfoPrevScreen;
        const bool map = from == 0x14 || from == 0x16 || from == 0x1e;
        InfoArea a{0, kTopBar, small ? w : *kSidebarX, h}; // the city reaches the bottom (no bottom edge)
        POINT p = InfoPosition(map ? InfoMode::Centered : g_infoMode, a, *kInfoCols * 16, *kInfoRows * 16, *kMouseX,
                               *kMouseY);
        if (fixed) {
            if (DrawIsHooked()) {
                g_shift = Shift{true, static_cast<int>(p.x) - kFixedX, static_cast<int>(p.y) - kFixedY};
            }
            // else: an unknown window class; it stays where the game has it
        }
        else {
            *kInfoX = p.x;
            *kInfoY = p.y;
        }
    }
    else if (!(w <= 1024 && (h == 600 || h == 768))) { // the game's own sizes keep the game's own rule
        const int x768 = small ? 0xf8 : 0xbc;
        const int shift = small ? (w - 1024) / 2 : (*kSidebarX - (1024 - kSidebarWidth)) / 2;
        if (h == 768 && *kInfoX == x768) { // the game's 768 rule ran: only the centre of the city area moved
            *kInfoX = x768 + shift;
        }
        else if (*kInfoX == 8 && *kInfoY == 0x20) { // not one of the game's special cases
            int bottom = h - kBottomEdge; // as the game's 768 rule (760 - rows x 16)
            *kInfoY = *kMouseY < (kTopBar + bottom) / 2 - 3 ? bottom - *kInfoRows * 16 : 0x2a;
            *kInfoX = x768 + shift;
        }
    }
    r.Forget(); // the position is the result
}

void __cdecl InfoPlace_detour()
{
    Original(o_InfoPlace, kInfoPlace)();
    core::diag::Restore r;
    if (!core::diag::Guarded(g_info, [](void *p) { PlaceInfo(*static_cast<core::diag::Restore *>(p)); }, &r, &r)) {
        g_shift = Shift{};
        static bool logged = false;
        if (!logged) {
            logged = true;
            core_log("resolution: the info window keeps the game's own position after an error");
        }
    }
}

// Drawing a shifted window: the game draws into a copy of the screen (draw_buffer) where the window's place
// (`from`) holds what is on screen where it is shown (`to`); afterwards everything outside `from` is copied back
// 1:1 (anything else the call drew) and `from` is copied to `to`.
struct Redirect {
    bool active = false;
    core::display::Surface16 back{}; // the screen as it was when the window started drawing
    RECT from{}, to{};
    core::diag::Restore r; // draw_buffer and the mouse
};
Redirect g_redirect;
BYTE *g_copy = nullptr;
size_t g_copySize = 0;

const int kWindowMovePhase = core::diag::perf::Register("info window move (ours)", core::diag::perf::Thread::Game);

bool BeginRedirect(int x, int y, int cols, int rows)
{
    core::diag::perf::Scope timed(kWindowMovePhase);
    const int pitch = *kScreenPitch, w = *kDdWidth, h = *kDdHeight;
    core::display::Surface16 back{*kDrawBuffer, pitch, w, h,
                                  pitch > 0 && h > 0 ? static_cast<size_t>(pitch) * static_cast<size_t>(h) : 0};
    RECT from{x, y, x + cols * 16, y + rows * 16};
    RECT to{from.left + g_shift.dx, from.top + g_shift.dy, from.right + g_shift.dx, from.bottom + g_shift.dy};
    // Both rectangles inside the screen (the placement keeps them in the city area; anything else: not moved).
    if (!core::display::MovedRectValid(back, from, to)) {
        static long logs = 0;
        if (++logs <= 4) {
            core_log("resolution: info window not moved: (%ld,%ld)-(%ld,%ld) to (%ld,%ld) does not fit a %dx%d screen "
                     "(pitch %d)",
                     from.left, from.top, from.right, from.bottom, to.left, to.top, w, h, pitch);
        }
        return false;
    }
    if (back.bytes > g_copySize) {
        if (g_copy) {
            VirtualFree(g_copy, 0, MEM_RELEASE);
        }
        g_copy = static_cast<BYTE *>(VirtualAlloc(nullptr, back.bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        g_copySize = g_copy ? back.bytes : 0;
    }
    if (!g_copy || !core::display::MovedRectPrepare(g_copy, back, from, to)) {
        return false;
    }
    Redirect &d = g_redirect;
    d.back = back;
    d.from = from;
    d.to = to;
    d.r.Set(kDrawBuffer.ptr(), g_copy);
    d.r.Set(kMouseX.ptr(), *kMouseX - g_shift.dx); // hover in the game's own place of the window
    d.r.Set(kMouseY.ptr(), *kMouseY - g_shift.dy);
    d.active = true;
    return true;
}

void EndRedirect()
{
    core::diag::perf::Scope timed(kWindowMovePhase);
    Redirect &d = g_redirect;
    if (!d.active) {
        return;
    }
    d.active = false;
    d.r.Undo(); // draw_buffer and the mouse
    // The screen it started with (the game does not change modes inside a window draw; checked again inside).
    core::display::MovedRectCompose(d.back, g_copy, d.from, d.to);
}

using WindowDraw_fn = void(__thiscall *)(void *, int, int, int, int, int);
WindowDraw_fn o_StorageDraw, o_TradeDraw, o_MillDraw;

struct WindowDrawCall {
    WindowDraw_fn original;
    void *self;
    int x, y, cols, rows, flag;
};

template <WindowDraw_fn *Trampoline, const Site *S>
void __fastcall WindowDraw_detour(void *self, void *, int x, int y, int cols, int rows, int flag)
{
    const WindowDraw_fn original = Original(*Trampoline, *S);
    WindowDrawCall c{original, self, x, y, cols, rows, flag};
    if (!Shifted()) {
        original(self, x, y, cols, rows, flag);
        return;
    }
    if (core::diag::Guarded(
            g_info,
            [](void *p) {
                WindowDrawCall &c = *static_cast<WindowDrawCall *>(p);
                if (!BeginRedirect(c.x, c.y, c.cols, c.rows)) {
                    g_shift = Shift{}; // cannot be shown elsewhere: where the game has it (buttons and input too)
                    c.original(c.self, c.x, c.y, c.cols, c.rows, c.flag);
                    return;
                }
                c.original(c.self, c.x, c.y, c.cols, c.rows, c.flag);
                EndRedirect();
            },
            &c, &g_redirect.r)) {
        return;
    }
    g_redirect.active = false; // put back by the guard; the window is drawn where the game has it from now on
    g_shift = Shift{};
    original(self, x, y, cols, rows, flag);
}

// The mill window's foreground: the same move as its window draw (the list it draws is inside the game's place of
// the window).
using Foreground_fn = void(__thiscall *)(void *, int, int, int, int);
Foreground_fn o_MillForeground;

struct ForegroundCall {
    Foreground_fn original;
    void *self;
    int x, y, cols, rows;
};

void __fastcall MillForeground_detour(void *self, void *, int x, int y, int cols, int rows)
{
    const Foreground_fn original = Original(o_MillForeground, kMillForeground);
    ForegroundCall c{original, self, x, y, cols, rows};
    if (!Shifted()) {
        original(self, x, y, cols, rows);
        return;
    }
    if (core::diag::Guarded(
            g_info,
            [](void *p) {
                ForegroundCall &c = *static_cast<ForegroundCall *>(p);
                if (!BeginRedirect(c.x, c.y, c.cols, c.rows)) {
                    g_shift = Shift{};
                    c.original(c.self, c.x, c.y, c.cols, c.rows);
                    return;
                }
                c.original(c.self, c.x, c.y, c.cols, c.rows);
                EndRedirect();
            },
            &c, &g_redirect.r)) {
        return;
    }
    g_redirect.active = false;
    g_shift = Shift{};
    original(self, x, y, cols, rows);
}

// The trade window's modal popup (drawn last in its window draw, at the screen's centre): on the real screen, with
// the real mouse.
using Popup_fn = void(__fastcall *)(void *);
Popup_fn o_TradePopup;

void __fastcall TradePopup_detour(void *self)
{
    if (g_redirect.active && !core::diag::Guarded(g_info, [](void *) { EndRedirect(); }, nullptr, &g_redirect.r)) {
        g_redirect.active = false;
        g_shift = Shift{};
    }
    Original(o_TradePopup, kTradePopup)(self);
}

// info_window_input 0x5a23a0 (screen 9's input, called by screen_handle_input): the mouse in the game's own place of
// the window. A point outside the shown window lands outside the game's window, so nothing hidden can be clicked.
// Not while the trade popup is open (it takes the input, at its own place on the screen).
Void_fn o_InfoInput;

void __cdecl InfoInput_detour()
{
    if (!Shifted() || *kTradePopupOpen) {
        Original(o_InfoInput, kInfoInput)();
        return;
    }
    core::diag::Restore r;
    if (core::diag::Guarded(
            g_info,
            [](void *p) {
                auto &r = *static_cast<core::diag::Restore *>(p);
                r.Set(kMouseX.ptr(), *kMouseX - g_shift.dx);
                r.Set(kMouseY.ptr(), *kMouseY - g_shift.dy);
                Original(o_InfoInput, kInfoInput)();
                r.Undo();
            },
            &r, &r)) {
        return;
    }
    g_shift = Shift{};
    Original(o_InfoInput, kInfoInput)();
}

// buttons_draw 0x449c10, for screen_draw's help / OK buttons of the window only (by return address): shown with the
// window. Their hover compares the real mouse with these on-screen positions; their clicks go through
// InfoInput_detour in the game's own place.
using ButtonsDraw_fn = void(__cdecl *)(int, int, void *, int);
ButtonsDraw_fn o_ButtonsDraw;

void __cdecl ButtonsDraw_detour(int x, int y, void *buttons, int count)
{
    if (GameAddress(__builtin_return_address(0)) == kInfoButtonsCall && Shifted()) {
        x += g_shift.dx;
        y += g_shift.dy;
    }
    Original(o_ButtonsDraw, kButtonsDraw)(x, y, buttons, count);
}

// The city is shown past its clip just now: our layout, and our city render working (games/emperor/scroll).
bool CityExtended()
{
    return g_extRight > 0 && g_extBottom > 0 && emperor::scroll::Working() && core::diag::Enabled(g_message);
}

// city_draw_background's bottom strip: not cleared while the city is shown down to the screen's bottom (it covers the
// strip in the city screen anyway; in screen 0x05 the city is drawn before it, so the clear would stay black).
using FillRect_fn = void(__cdecl *)(int, int, int, int, int);
FillRect_fn o_FillRect;

void __cdecl FillRect_detour(int x, int y, int w, int h, int colour)
{
    if (GameAddress(__builtin_return_address(0)) == kBackgroundStripCall && CityExtended()) {
        return;
    }
    Original(o_FillRect, kFillRect)(x, y, w, h, colour);
}

// The city message panel laid out in the city as it is shown: while 0x50bf10 runs, the clip's right and bottom are
// the extent (the sidebar's left edge, the screen's bottom), so the panel is as wide as the city and on the screen's
// bottom. The city it draws meanwhile still uses the grid clip (scroll's Render works it out from the viewport).
using Void_fn2 = void(__cdecl *)();
Void_fn2 o_MessagePanel;

struct PanelCall {
    core::diag::Restore r;
};

// R2d: the help window's contents panels (topic kind 0, e.g. Help -> Table of contents) are put at x = 10 by the
// game (0x50c17c `mov ecx, 10`, also at 1024x768); every other kind is placed in the 640x480 dialog frame. Above
// 1024 wide the immediate becomes the x that centres the panel in the city area (left of the sidebar). The width is
// the topic's (0x10c7622 + topic x 0x50, in 16 px blocks; the topic is 0x10bf504, set by help_window_open 0x50b370).
// On the China map the game centres it itself afterwards (0x50bf10's 0x10bf568 == 0x14 branch).
constexpr DWORD kHelpTocX = 0x50c17c; // b9 0a 00 00 00 (mov ecx, imm32), then c7 05 24 f5 0b 01 (0x10bf524 = 0x30)
const BYTE kHelpTocBytes[] = {0xb9, 0x0a, 0x00, 0x00, 0x00, 0xc7, 0x05, 0x24, 0xf5, 0x0b, 0x01};
core::diag::Feature g_helpX{"help contents panel position (games/emperor/resolution)"};
bool g_helpXReady = false; // the bytes matched at install
int g_helpXWritten = 10;

void SetHelpX(int x)
{
    if (x == g_helpXWritten) {
        return;
    }
    BYTE imm[4];
    std::memcpy(imm, &x, 4);
    if (PatchCode(kHelpTocX + 1 + g_imageShift, imm, 4)) {
        g_helpXWritten = x;
    }
}

void PlaceHelpContents()
{
    const int w = *kScreenWidth;
    const int topic = *reinterpret_cast<volatile int *>(0x10bf504 + g_imageShift);
    const int cols = *reinterpret_cast<volatile short *>(0x10c7622 + g_imageShift + topic * 0x50);
    const int right = *kSmallTiles == 1 ? w : *kSidebarX;
    int x = 10;
    if (w > 1024 && topic >= 0 && cols > 0 && cols * 16 < right) {
        x = (right - cols * 16) / 2;
    }
    SetHelpX(x);
}

void __cdecl MessagePanel_detour()
{
    if (g_helpXReady && core::diag::Enabled(g_helpX) &&
        !core::diag::Guarded(g_helpX, [](void *) { PlaceHelpContents(); }, nullptr)) {
        SetHelpX(10); // the game's own place from now on
        core_log("resolution: the help contents panel stays at the game's own x after an error");
    }
    if (!CityExtended()) {
        Original(o_MessagePanel, kMessagePanel)();
        return;
    }
    PanelCall c;
    if (core::diag::Guarded(
            g_message,
            [](void *p) {
                auto &c = *static_cast<PanelCall *>(p);
                c.r.Set(kCityClipRight.ptr(), g_extRight);
                c.r.Set(kCityClipBottom.ptr(), g_extBottom);
                Original(o_MessagePanel, kMessagePanel)();
                c.r.Undo();
            },
            &c, &c.r)) {
        return;
    }
    Original(o_MessagePanel, kMessagePanel)(); // put back by the guard: the game's own layout from now on
}

// image_draw 0x413960 for the full-screen picture (group 0x1a01: the menu pictures and the China map) copies a
// rectangle of 0x10c733c x 0x10c7338 starting at fullscreen_art_offset (0x5cd8a0 -> 0x4194c0, no checks). Its
// callers size that rectangle from the screen or the map window, which is larger than the picture on a larger
// screen: the China map clamps the width to the picture but not the height (dd_height - 40 > 746 rows above 786),
// and the first R1 test crashed in 0x4194c0. The rectangle is clamped to the picture here.
constexpr int kGroupFullscreenArt = 0x1a01;
const Var<int> kArtOffsetX{0x10c7334}; // fullscreen_art_offset_x
const Var<int> kArtOffsetY{0x10c7330};
const Var<int> kArtCopyW{0x10c733c};
const Var<int> kArtCopyH{0x10c7338};

using ImageDrawHook_fn = int(__cdecl *)(int, int, int, int, int, int);
ImageDrawHook_fn o_ImageDraw;
int g_artId = -1;
long g_clampLogs = 0;

struct ClampCall {
    int id;
    core::diag::Restore r;
};

void Clamp(ClampCall &c)
{
    if (g_artId < 0) {
        g_artId = ImageId(kGroupFullscreenArt);
    }
    if (c.id != g_artId) {
        return;
    }
    int iw, ih;
    ImageSize(c.id, &iw, &ih);
    int ox = *kArtOffsetX, oy = *kArtOffsetY, w = *kArtCopyW, h = *kArtCopyH;
    int ox2 = ox < 0 ? 0 : ox, oy2 = oy < 0 ? 0 : oy;
    int w2 = w > iw - ox2 ? iw - ox2 : w, h2 = h > ih - oy2 ? ih - oy2 : h;
    if (iw > 0 && ih > 0 && (ox2 != ox || oy2 != oy || w2 != w || h2 != h)) {
        c.r.Set(kArtOffsetX.ptr(), ox2);
        c.r.Set(kArtOffsetY.ptr(), oy2);
        c.r.Set(kArtCopyW.ptr(), w2 < 0 ? 0 : w2);
        c.r.Set(kArtCopyH.ptr(), h2 < 0 ? 0 : h2);
        if (++g_clampLogs <= 8) {
            core_log("resolution: full-screen picture %dx%d: copy %dx%d from (%d,%d) clamped to %dx%d from (%d,%d) "
                     "(screen 0x%02x)",
                     iw, ih, w, h, ox, oy, *kArtCopyW, *kArtCopyH, ox2, oy2,
                     static_cast<unsigned char>(*kGameScreen));
        }
    }
    c.r.Forget(); // the clamped rectangle is the result
}

int __cdecl ImageDraw_detour(int id, int x, int y, int a, int b, int c)
{
    if (core::diag::Enabled(g_clamp)) {
        if (g_artId < 0 || id == g_artId) { // other images: nothing to do (no guard on every image the game draws)
            ClampCall call{id, {}};
            if (!core::diag::Guarded(g_clamp, [](void *p) { Clamp(*static_cast<ClampCall *>(p)); }, &call,
                                     &call.r)) {
                // Abandoned: the copy might run past the picture (the crash at 0x4194c0), so this image is not drawn
                // and the game goes back to its own 1024x768, where the picture fits.
                ToGameSize("the full-screen picture could not be clamped");
                return 0;
            }
        }
    }
    else if (id == g_artId && (*kScreenWidth > kMinW || *kScreenHeight > kMinH)) {
        return 0; // until the game is back at its own size
    }
    return Original(o_ImageDraw, kImageDraw)(id, x, y, a, b, c);
}

using ArtLoad_fn = int(__cdecl *)(int, int, int);
ArtLoad_fn o_ArtLoad;

long g_switchLogs = 0;
constexpr long kSwitchLogs = 40;

// Only our own flag (nothing that can fault), so not guarded; with switching off it is left alone.
int __cdecl ArtLoad_detour(int index, int x, int y)
{
    char s = *kGameScreen;
    if (!g_frontEnd && !CityScreen(s) && core::diag::Enabled(g_switching)) {
        g_frontEnd = true; // a full-screen picture: OnLoop switches to the 1024x768 frame
        if (++g_switchLogs <= kSwitchLogs) {
            core_log("resolution: full-screen picture %d on screen 0x%02x -> 1024x768 frame", index,
                     static_cast<unsigned char>(s));
        }
    }
    int r = Original(o_ArtLoad, kArtLoad)(index, x, y);
    emperor::fullscreen::OnArtLoaded(index, x, y);
    return r;
}

bool g_installed = false;
DWORD g_lastTry = 0;

// ---------------------------------------------------------------------------------------------
// Shift+F7

const Setting kCycle[] = {
    {CORE_RESOLUTION_FIXED, 1024, 768, 0}, {CORE_RESOLUTION_SCALED, 0, 0, 0},   {CORE_RESOLUTION_SCALED, 0, 0, 100},
    {CORE_RESOLUTION_SCALED, 0, 0, 125},   {CORE_RESOLUTION_SCALED, 0, 0, 150}, {CORE_RESOLUTION_SCALED, 0, 0, 175},
    {CORE_RESOLUTION_SCALED, 0, 0, 200},   {CORE_RESOLUTION_SCALED, 0, 0, 250},
};
int g_cycle = -1; // -1: the setting from emperor_engine.ini

// The mode change (the game's own, as its F7 does it). Guarded with switching: if it is abandoned, the game's own
// size comes back on the next loop (screen_change_mode builds everything again then).
void ChangeMode()
{
    Function<ChangeMode_fn>(kChangeMode)(3);
    *kRedraw = 1;
}

void CycleSetting()
{
    const int n = sizeof(kCycle) / sizeof(kCycle[0]);
    for (int tries = 0; tries < n; ++tries) { // the next setting that gives another size on this screen
        g_cycle = (g_cycle + 1) % n;
        core::display::LogicalSize next = Logical(kCycle[g_cycle]);
        if (next.w != g_lastW || next.h != g_lastH) {
            break;
        }
    }
    g_setting = kCycle[g_cycle];
    char what[48];
    Describe(g_setting, what, sizeof(what));
    core_log("resolution: Shift+F7 -> %s (until the game ends; emperor_engine.ini is not changed)", what);
    ChangeMode();
}

// Modes 1 (640x480) and 2 (800x600) have no use with our resolutions (playtest 2026-10-25: always 1024x768, i.e. mode 3,
// which carries our size). The game falls back to mode 1 when DirectDraw refuses 1024x768 and 800x600 (dd_init), so
// a run of the original can leave it in Emperor.inf; F6 and the Display options button "800 x 600" ask for mode 2.
// Videos still switch modes for themselves (FUN_00420ae0) and switch back.
using InitialMode_fn = int(__cdecl *)();
InitialMode_fn o_InitialMode;

int __cdecl InitialMode_detour()
{
    const int r = Original(o_InitialMode, kInitialMode)();
    if (*kScreenMode != 3) {
        core_log("resolution: Emperor.inf has mode %d (%s); the game starts in mode 3 (1024x768, our resolution) "
                 "instead",
                 *kScreenMode, *kScreenMode == 1 ? "640x480" : *kScreenMode == 2 ? "800x600" : "unknown");
        *kScreenMode = 3;
    }
    return r;
}

bool ModeKey(UINT message, WPARAM key, LPARAM)
{
    if (message != WM_KEYDOWN || key != VK_F6) {
        return false;
    }
    static long logs = 0;
    if (++logs <= 5) {
        core_log("resolution: F6 (800x600) ignored: with our resolutions the game stays in mode 3");
    }
    return true;
}

// F5 (and the Display options button "Windowed screen", which posts F5) and Alt+Enter: our window mode instead of the
// game's
// (R2e, docs/resolution_ui/r2e.md). The game never sees it, so screen_fullscreen stays 1 and Emperor.inf only ever
// holds the original's own value; the change goes through the setting (saved, applied on the game thread).
bool WindowKey(UINT message, WPARAM key, LPARAM l)
{
    const bool f5 = message == WM_KEYDOWN && key == VK_F5 && !(GetKeyState(VK_SHIFT) & 0x8000) &&
                    !(GetKeyState(VK_CONTROL) & 0x8000);
    const bool altEnter = message == WM_SYSKEYDOWN && key == VK_RETURN && (l & (1 << 29)); // the usual key elsewhere
    if (!f5 && !altEnter) {
        return false;
    }
    if (!(l & (1 << 30))) { // not an auto-repeat
        const bool to = !core::display::Windowed();
        core_log("resolution: %s -> %s (the game does not see the key)", f5 ? "F5" : "Alt+Enter",
                 to ? "windowed" : "fullscreen");
        core::settings::Request(core::settings::Find("display", "window"), to ? "windowed" : "fullscreen", true);
    }
    return true;
}

bool KeyFilter(UINT message, WPARAM key, LPARAM l)
{
    if (WindowKey(message, key, l) || ModeKey(message, key, l)) {
        return true;
    }
    if (message != WM_KEYDOWN || key != VK_F7 || !(GetKeyState(VK_SHIFT) & 0x8000)) {
        return false;
    }
    if ((l & (1 << 30)) || !core::diag::Enabled(g_switching)) {
        return true; // auto-repeat: one step per press; off: Shift+F7 does nothing (the game does not see it)
    }
    if (!core::diag::Guarded(g_switching, [](void *) { CycleSetting(); }, nullptr)) {
        ToGameSize("Shift+F7 could not change the resolution");
    }
    return true;
}

void Switch()
{
    char s = *kGameScreen;
    if (g_frontEnd && CityScreen(s)) {
        g_frontEnd = false;
        if (++g_switchLogs <= kSwitchLogs) {
            core_log("resolution: city screen 0x%02x -> the configured resolution", static_cast<unsigned char>(s));
        }
    }
    if (*kScreenMode != 3) {
        return; // F6 (800x600) or a video: the game's own mode
    }
    core::display::LogicalSize t = Logical(Target());
    if (t.w == *kScreenWidth && t.h == *kScreenHeight) {
        return;
    }
    DWORD now = GetTickCount();
    if (now - g_lastTry < 500) {
        return; // screen_change_mode refuses during videos and a few other screens; try again later
    }
    g_lastTry = now;
    ChangeMode();
}

} // namespace

POINT InfoPosition(InfoMode mode, const InfoArea &a, int w, int h, int mouseX, int mouseY)
{
    constexpr int kGap = 24; // between the cursor and the window
    auto clamp = [](int v, int lo, int hi) { return v > hi ? (hi > lo ? hi : lo) : v < lo ? lo : v; };
    POINT p{a.left, a.top};
    if (mode == InfoMode::Centered) {
        p.x = a.left + (a.right - a.left - w) / 2;
        p.y = a.top + (a.bottom - a.top - h) / 2;
    }
    else if (mode == InfoMode::Cursor) {
        // Beside the cursor (right of it, or left when it does not fit), centred on it vertically; never past the
        // city area.
        p.x = mouseX + kGap + w <= a.right ? mouseX + kGap : mouseX - kGap - w;
        p.y = mouseY - h / 2;
    }
    p.x = clamp(static_cast<int>(p.x), a.left, a.right - w);
    p.y = clamp(static_cast<int>(p.y), a.top, a.bottom - h);
    return p;
}

void SetInfoMode(InfoMode mode)
{
    static const char *const kNames[] = {"original", "centered", "cursor"};
    g_infoMode = mode;
    core_log("resolution: info window position %s (from the next window opened)", kNames[static_cast<int>(mode)]);
}

void OnLoop()
{
    if (!g_installed) {
        return;
    }
    if (g_toGameSize) {
        // Back to the game's own size: mode 3 is 1024x768 again (switching is off); the game's own mode change.
        // Not guarded: from here on everything is the game's own (our layout hooks pass 1024 through).
        if (*kScreenMode == 3 && (*kScreenWidth != kMinW || *kScreenHeight != kMinH)) {
            DWORD now = GetTickCount();
            if (now - g_lastTry >= 500) {
                g_lastTry = now;
                Function<ChangeMode_fn>(kChangeMode)(3);
                *kRedraw = 1;
            }
            return;
        }
        g_toGameSize = false;
        core_log("resolution: the game runs at its own %dx%d", *kScreenWidth, *kScreenHeight);
        return;
    }
    if (!core::diag::Enabled(g_switching)) {
        return;
    }
    if (!core::diag::Guarded(g_switching, [](void *) { Switch(); }, nullptr)) {
        ToGameSize("the resolution could not be changed");
    }
}

void DrawSidebarFiller()
{
    const int w = *kScreenWidth, h = *kScreenHeight;
    if ((w <= 1024 && h <= 768) || !core::diag::Enabled(g_filler)) {
        return;
    }
    if (!core::diag::Guarded(g_filler, [](void *) { DrawFiller(false); }, nullptr)) {
        core_log("resolution: the space below the sidebar is no longer filled after an error");
    }
}

bool FrontEnd()
{
    return g_installed && g_frontEnd;
}

bool SettingsChanged(const core_config *cfg)
{
    if (!g_installed) {
        return true;
    }
    if (!core::diag::Enabled(g_switching)) {
        core_log("resolution: setting not applied: resolution switching was switched off after an error (the game "
                 "runs at its own 1024x768)");
        return false;
    }
    g_setting = Setting{cfg->resolution, cfg->resolution_w, cfg->resolution_h, cfg->scale_percent};
    g_cycle = -1; // Shift+F7 starts again from the setting
    g_lastTry = 0;
    char what[48];
    Describe(g_setting, what, sizeof(what));
    core_log("resolution: setting changed to %s%s", what, g_frontEnd ? " (applies on the city screen)" : "");
    if (core::display::Windowed() && g_setting.kind == CORE_RESOLUTION_FIXED) {
        // Windowed the game is laid out for the window: a size chosen here becomes the window's client area.
        core::display::SetWindowClient(g_setting.w, g_setting.h);
    }
    return true;
}

const Detour *Detours(int *count)
{
    static const Detour kDetours[] = {
        {&kModeGetSize, reinterpret_cast<void *>(&ModeGetSize_detour)},
        {&kSidebarPos, reinterpret_cast<void *>(&SidebarPos_detour)},
        {&kViewportSmall, reinterpret_cast<void *>(&ViewportSmall_detour)},
        {&kViewportLarge, reinterpret_cast<void *>(&ViewportLarge_detour)},
        {&kInfoPlace, reinterpret_cast<void *>(&InfoPlace_detour)},
        {&kFrameBars, reinterpret_cast<void *>(&FrameBars_detour)},
        {&kSidebarDraw, reinterpret_cast<void *>(&SidebarDraw_detour)},
        {&kArtLoad, reinterpret_cast<void *>(&ArtLoad_detour)},
        {&kImageDraw, reinterpret_cast<void *>(&ImageDraw_detour)},
        {&kStorageDraw, reinterpret_cast<void *>(&WindowDraw_detour<&o_StorageDraw, &kStorageDraw>)},
        {&kTradeDraw, reinterpret_cast<void *>(&WindowDraw_detour<&o_TradeDraw, &kTradeDraw>)},
        {&kMillDraw, reinterpret_cast<void *>(&WindowDraw_detour<&o_MillDraw, &kMillDraw>)},
        {&kMillForeground, reinterpret_cast<void *>(&MillForeground_detour)},
        {&kTradePopup, reinterpret_cast<void *>(&TradePopup_detour)},
        {&kInfoInput, reinterpret_cast<void *>(&InfoInput_detour)},
        {&kButtonsDraw, reinterpret_cast<void *>(&ButtonsDraw_detour)},
        {&kMessagePanel, reinterpret_cast<void *>(&MessagePanel_detour)},
        {&kFillRect, reinterpret_cast<void *>(&FillRect_detour)},
    };
    *count = static_cast<int>(sizeof(kDetours) / sizeof(kDetours[0]));
    return kDetours;
}

void InstalledForTest(const core_config *cfg)
{
    g_setting = Setting{cfg->resolution, cfg->resolution_w, cfg->resolution_h, cfg->scale_percent};
    g_installed = true;
}

bool KeyFilterForTest(UINT message, WPARAM key, LPARAM l)
{
    return KeyFilter(message, key, l);
}

void SetInterfacePackForTest(const wchar_t *path)
{
    g_packForTest = path;
    g_backdrop = Backdrop{};
}

bool Attach(const core_config *cfg)
{
    if (cfg->backend != CORE_BACKEND_EMULATED) {
        core_log("resolution: real DirectDraw (display.backend=system): the game's own modes only");
        return false;
    }
    const Site *sites[] = {&kModeGetSize, &kSidebarPos, &kViewportSmall, &kViewportLarge, &kChangeMode,
                           &kViewportInit, &kInfoPlace, &kFrameBars, &kSidebarDraw, &kArtLoad,
                           &kImageGetId, &kImageDraw, &kImageSize, &kSpecialPlace, &kStorageDraw,
                           &kTradeDraw, &kMillDraw, &kMillForeground, &kTradePopup, &kInfoInput, &kButtonsDraw,
                           &kBuildingGet, &kMessagePanel, &kFillRect};
    for (const Site *s : sites) {
        if (!Matches(*s)) {
            core_log("resolution: %s at 0x%lx does not match; custom resolutions not installed", s->name, s->address);
            return false;
        }
    }
    for (const JumpSite &j : kJumps) {
        int n = j.jump + (j.expect[j.jump] == 0x0f ? 2 : 1);
        if (std::memcmp(reinterpret_cast<const void *>(j.address), j.expect, n) != 0) {
            core_log("resolution: layout compare at 0x%lx does not match; custom resolutions not installed",
                     j.address);
            return false;
        }
    }
    int patched = 0;
    for (const JumpSite &j : kJumps) {
        DWORD at = j.address + j.jump;
        if (j.expect[j.jump] == 0x0f) {
            const BYTE jl[2] = {0x0f, 0x8c}; // jnz rel32 -> jl rel32
            patched += PatchCode(at, jl, 2);
        }
        else {
            const BYTE jl = 0x7c;            // jnz rel8 -> jl rel8
            patched += PatchCode(at, &jl, 1);
        }
    }
    g_setting = Setting{cfg->resolution, cfg->resolution_w, cfg->resolution_h, cfg->scale_percent};
    Install(kModeGetSize, &ModeGetSize_detour, o_ModeGetSize);
    if (Matches(kInitialMode)) {
        Install(kInitialMode, &InitialMode_detour, o_InitialMode);
    }
    else {
        core_log("resolution: %s at 0x%lx does not match: Emperor.inf's mode is used as it is", kInitialMode.name,
                 kInitialMode.address);
    }
    Install(kSidebarPos, &SidebarPos_detour, o_SidebarPos);
    Install(kViewportSmall, &ViewportSmall_detour, o_ViewportSmall);
    Install(kViewportLarge, &ViewportLarge_detour, o_ViewportLarge);
    Install(kInfoPlace, &InfoPlace_detour, o_InfoPlace);
    Install(kFrameBars, &FrameBars_detour, o_FrameBars);
    Install(kSidebarDraw, &SidebarDraw_detour, o_SidebarDraw);
    Install(kArtLoad, &ArtLoad_detour, o_ArtLoad);
    Install(kImageDraw, &ImageDraw_detour, o_ImageDraw);
    core_hook_install(kStorageDraw.name, reinterpret_cast<void *>(kStorageDraw.address),
                      reinterpret_cast<void *>(&WindowDraw_detour<&o_StorageDraw, &kStorageDraw>),
                      reinterpret_cast<void **>(&o_StorageDraw));
    core_hook_install(kTradeDraw.name, reinterpret_cast<void *>(kTradeDraw.address),
                      reinterpret_cast<void *>(&WindowDraw_detour<&o_TradeDraw, &kTradeDraw>),
                      reinterpret_cast<void **>(&o_TradeDraw)); // the detour is __fastcall for the game's __thiscall
    core_hook_install(kMillDraw.name, reinterpret_cast<void *>(kMillDraw.address),
                      reinterpret_cast<void *>(&WindowDraw_detour<&o_MillDraw, &kMillDraw>),
                      reinterpret_cast<void **>(&o_MillDraw));
    core_hook_install(kMillForeground.name, reinterpret_cast<void *>(kMillForeground.address),
                      reinterpret_cast<void *>(&MillForeground_detour), reinterpret_cast<void **>(&o_MillForeground));
    Install(kTradePopup, &TradePopup_detour, o_TradePopup);
    Install(kInfoInput, &InfoInput_detour, o_InfoInput);
    Install(kButtonsDraw, &ButtonsDraw_detour, o_ButtonsDraw);
    Install(kMessagePanel, &MessagePanel_detour, o_MessagePanel);
    g_helpXReady = std::memcmp(reinterpret_cast<const void *>(kHelpTocX + g_imageShift), kHelpTocBytes,
                               sizeof(kHelpTocBytes)) == 0;
    if (!g_helpXReady) {
        core_log("resolution: help contents panel code at 0x%lx is not as expected; left as the game has it",
                 static_cast<unsigned long>(kHelpTocX));
    }
    Install(kFillRect, &FillRect_detour, o_FillRect);
    core::display::SetKeyFilter(&KeyFilter);
    core::display::SetGameMinimum(kMinW, kMinH);
    g_installed = true;
    char what[48];
    Describe(g_setting, what, sizeof(what));
    core_log("resolution: installed (display.resolution %s; %d of %d layout compares now take the 1024 layout for "
             "any width >= 1024; front-end screens at 1024x768; Shift+F7 cycles the resolution; each part guarded)",
             what, patched, static_cast<int>(sizeof(kJumps) / sizeof(kJumps[0])));
    return true;
}

} // namespace emperor::resolution
