// Pixel (smooth) scrolling of the city view.
//
// The game's camera is view_x (tiles) / view_y (half rows, even). The camera (core/scroll/camera) adds a pixel
// remainder (px, py); the picture (core/scroll/picture, shared with Zeus) draws the city with it and moves the
// positions the game converts the same way. This file gives both Emperor's addresses, hooks the game's functions and
// keeps the city's zoom, which only Emperor has.
//
// Emperor's drawing (core/scroll/picture.h has the method): the game's tile blitters (0x4527d0 and the others selected
// by 0x5cd8a0) only handle a tile cut exactly in half at a clip edge (an early build wrote past the back buffer and
// crashed at 0x452931).
//   city_render_tiles 0x46ff60: columns view_x-4 .. +width+6, rows view_y-8 .. +height+21  (covers the extra area)
//   0x46a810 (selection markers): columns -4 .. width+6, rows -8 .. height+5               (covers it)
//   0x46e1b0 (overlay markers): columns 0 .. width, rows 0 .. height-1 -- so while drawing, the viewport is also
//   made 1 column wider and 2 half rows taller.
// The minimap (0x46e730/0x46e580) frames the view from the tile position, so its frame is at most one tile
// (about two minimap pixels) behind while a pixel remainder is present; it does not read the origin.
//
// Scrolling: the camera is the core's (core/scroll/camera.h, shared with Zeus): at the city call sites of
// scroll_get_direction (0x52b310) it moves view_x / view_y and the pixel remainder by elapsed time; this file gives it
// Emperor's addresses (kCamera).
//
// City zoom: the core's (core/scroll/zoom.h, shared with Zeus): the picture's and the camera's zoom callbacks are
// its functions (the zoomed city drawn into our buffer and scaled onto the screen, the drag selection's frame put on
// the screen through line_draw, the camera's speed scaled so the screen scrolls as fast as at 100%).
//
// Failure: as core/scroll/picture.h; after a fault ResetPicture also puts the zoom back to 100%.
#include "games/emperor/scroll.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/scroll/camera.h"
#include "core/scroll/drag.h"
#include "core/scroll/picture.h"
#include "core/scroll/zoom.h"
#include "games/emperor/keys.h"
#include "games/emperor/site.h"

#include <cstring>

namespace emperor::scroll {

namespace {

namespace picture = core::scroll::picture;

// ---------------------------------------------------------------------------------------------
// Addresses

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
// line_draw(x1, y1, x2, y2, colour): __cdecl, ret (the caller pops; checked by disassembly). The drag
// selection's frame is worked out in zoomed-buffer pixels and drawn on the screen.
const Site kLineDraw{"line_draw", 0x5292d0, {0x8b, 0x44, 0x24, 0x10, 0x8b, 0x4c, 0x24, 0x08}};
// city_view_width_px(): __cdecl, no arguments, ret; (tile width + 2) x viewport columns. Its only caller is the
// "Game paused" banner (0x5b6e93: x = (width - 0x1c0) / 2). Zoomed, the city's width on the screen.
const Site kCityWidth{"city_view_width_px", 0x46a1b0, {0xa1, 0xb8, 0xf8, 0xad, 0x00, 0x83, 0xc0, 0x02}};

// Return addresses of the scroll_get_direction calls whose result goes to view_scroll_step (the city view); the
// other two callers (0x539a23, 0x5f7349) scroll other things and keep the original behaviour.
const DWORD kCityScrollCalls[] = {0x538508, 0x538544, 0x5385da, 0x539f4f, 0x53800b, 0x538085, 0x54b4d4};

// Return addresses of grid_to_screen's calls for the drag frame's corner in drag_select_start / drag_select_draw
// (0x44bb38.., 0x44bbc4..: the corner less the tile's height x 40).
constexpr DWORD kDragStartCorner = 0x44bb29;
constexpr DWORD kDragDrawCorner = 0x44bbb5;

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
const Var<int> kMouseX{0x10de0a0};       // the game's mouse (mouse_to_grid reads these)
const Var<int> kMouseY{0x10de09c};
const Var<int> kScreenPitch{0x10de000};  // bytes per row
const Var<int> kDdWidth{0x10de008};
const Var<int> kDdHeight{0x10de004};
const Var<unsigned char> kMouseMode{0x10de041}; // 0x4d during a military drag selection
constexpr unsigned char kMouseModeDragSelect = 0x4d;
const Var<int> kCursorTile{0x101d0b4};             // the cursor's tile index (cursor_tile_update 0x405500)
const Var<signed char> kTileHeights{0xbebf38 + 2}; // map_tile_height 0x408cf0: this + 2 + tile, 228 x 228
constexpr int kMapTiles = 228 * 228;
constexpr int kHeightPx = 40; // the drag corner: the game takes the tile's height x 40 off (0x44bb38..)

core::diag::Feature g_feature{"smooth scrolling (games/emperor/scroll)"};

using VoidFn = void(__cdecl *)();

void ViewClamp()
{
    Function<VoidFn>(kViewClamp)();
}

// The camera's view of Emperor (core/scroll/camera.h). Its callbacks are defined below; the zoom's are the core's.
void ResetPicture();
bool KeyHeld(unsigned vk);
const core::scroll::camera::CameraGame kCamera{
    "emperor",
    &g_feature,
    core::scroll::camera::CameraVars{kViewX, kViewY, kTileWidth, kTileHalfHeight, kCursorX, kCursorY, kDesktopW,
                                     kDesktopH, kKeyLeft, kKeyRight, kKeyUp, kKeyDown, kViewScrolling, kScrollSpeed,
                                     kFrameCounter},
    &ViewClamp,
    [] { Function<VoidFn>(kSoundViewDecay)(); },
    &KeyHeld,
    &ResetPicture,
    &core::scroll::zoom::CameraMoving,
    &core::scroll::zoom::CameraStep,
    &core::scroll::zoom::CameraScale,
};

// The picture's view of Emperor (core/scroll/picture.h). The zoom's callbacks are the core's (core/scroll/zoom.h).
void DrawCityOriginal(int vx, int vy, int orientation);
const picture::PictureGame kPicture{
    "emperor",
    &kCamera,
    picture::PictureVars{kOriginX, kOriginY, kWidthTiles, kHeightRows, kClipLeft, kClipTop, kClipRight, kClipBottom,
                         kDrawBuffer, kMouseX, kMouseY, kScreenPitch, kDdWidth, kDdHeight, kMouseMode, kCursorTile,
                         kTileHeights},
    kMouseModeDragSelect,
    kHeightPx,
    kMapTiles,
    kDragStartCorner,
    kDragDrawCorner,
    &DrawCityOriginal,
    &DrawCityOriginal, // after a fault: the same (every drawing counts in Emperor)
    &core::scroll::zoom::Percent,
    &core::scroll::zoom::BufferSize,
    &core::scroll::zoom::RenderZoomed,
    &core::scroll::zoom::DragDrawZoom,
    &core::scroll::zoom::DragLinesMap,
};

void ResetPicture()
{
    picture::Reset();
    // The zoomed view must not reach the game's own drawing (straight into the back buffer): 100% first.
    core::scroll::zoom::Off("smooth scrolling was switched off after an error");
}

bool KeyHeld(unsigned vk)
{
    return emperor::keys::Held(vk); // a key bound to this direction (W, say)
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

void DrawCityOriginal(int vx, int vy, int orientation)
{
    Original(o_CityRender, kCityRender)(vx, vy, orientation);
}

void __cdecl CityRender_detour(int vx, int vy, int orientation)
{
    picture::CityRender(kPicture, vx, vy, orientation);
}

int __cdecl ScreenToGrid_detour(int x, int y)
{
    return picture::ScreenToGrid(kPicture, x, y, 0, [](int x, int y, int) {
        return Original(o_ScreenToGrid, kScreenToGrid)(x, y);
    });
}

int __cdecl MouseToGrid_detour(int unused)
{
    return picture::MouseToGrid(kPicture, unused, [](int unused) {
        return Original(o_MouseToGrid, kMouseToGrid)(unused);
    });
}

int __cdecl ScreenToGridRaw_detour(int x, int y, int unused)
{
    return picture::ScreenToGridRaw(kPicture, x, y, unused, [](int x, int y, int unused) {
        return Original(o_ScreenToGridRaw, kScreenToGridRaw)(x, y, unused);
    });
}

void __cdecl GridToScreen_detour(int gx, int gy, int *sx, int *sy)
{
    picture::GridToScreen(kPicture, GameAddress(__builtin_return_address(0)), gx, gy, sx, sy,
                          [](int gx, int gy, int *sx, int *sy) {
                              Original(o_GridToScreen, kGridToScreen)(gx, gy, sx, sy);
                          });
}

// The drag selection's frame, worked out in buffer pixels while zoomed, drawn on the screen (the core's
// LineDraw).
using Line_fn = void(__cdecl *)(int, int, int, int, int);
Line_fn o_Line;

void __cdecl Line_detour(int x1, int y1, int x2, int y2, int colour)
{
    core::scroll::zoom::LineDraw(x1, y1, x2, y2, colour, [](int x1, int y1, int x2, int y2, int colour) {
        Original(o_Line, kLineDraw)(x1, y1, x2, y2, colour);
    });
}

// The "Game paused" banner is centred on the city's width (the picture's CityWidth).
using CityWidth_fn = int(__cdecl *)();
CityWidth_fn o_CityWidth;

int __cdecl CityWidth_detour()
{
    return picture::CityWidth(kPicture, [] { return Original(o_CityWidth, kCityWidth)(); });
}

constexpr int kCityScrollCallCount = static_cast<int>(sizeof(kCityScrollCalls) / sizeof(kCityScrollCalls[0]));

int __cdecl GetDirection_detour()
{
    if (core::scroll::camera::IsCall(__builtin_return_address(0), kCityScrollCalls, kCityScrollCallCount)) {
        if (core::scroll::camera::CityDirection(kCamera)) {
            return 8; // view_scroll_step(8) does nothing: the camera has been moved already
        }
        // The game's own steps, its edge test through gameplay.edge_scroll_width.
        return core::scroll::camera::GameDirection(kCamera,
                                                   [] { return Original(o_GetDirection, kScrollGetDirection)(); });
    }
    return Original(o_GetDirection, kScrollGetDirection)();
}

// Counting only (the rate log); nothing here can fault, so it is not guarded.
void __cdecl ScrollStep_detour(int dir)
{
    core::scroll::camera::CountStep(kCamera, dir);
    Original(o_ScrollStep, kViewScrollStep)(dir);
}

// Military drag selection (core/scroll/picture.h): the three functions are __thiscall on the drag structure with no
// stack arguments (taken as __fastcall(int *)).
void __fastcall DragStart_detour(int *drag)
{
    picture::DragStart(kPicture, drag, [](int *drag) { Original(o_DragStart, kDragStart)(drag); });
}

void __fastcall DragDraw_detour(int *drag)
{
    picture::DragAdjusted(kPicture, drag, [](int *drag) { Original(o_DragDraw, kDragDraw)(drag); }, true);
}

void __fastcall DragFinish_detour(int *drag)
{
    picture::DragAdjusted(kPicture, drag, [](int *drag) { Original(o_DragFinish, kDragFinish)(drag); }, false);
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
    {&kDragDraw, reinterpret_cast<void *>(&DragDraw_detour)},
    {&kDragFinish, reinterpret_cast<void *>(&DragFinish_detour)},
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
    if (!core::scroll::camera::CallsTarget("emperor", kCityScrollCalls, kCityScrollCallCount, kScrollGetDirection)) {
        return false;
    }
    Install(kViewScrollStep, &ScrollStep_detour, o_ScrollStep); // counts steps in both modes (rate log)
    Install(kCityRender, &CityRender_detour, o_CityRender);
    Install(kScreenToGrid, &ScreenToGrid_detour, o_ScreenToGrid);
    Install(kMouseToGrid, &MouseToGrid_detour, o_MouseToGrid);
    Install(kScreenToGridRaw, &ScreenToGridRaw_detour, o_ScreenToGridRaw);
    Install(kGridToScreen, &GridToScreen_detour, o_GridToScreen);
    Install(kDragStart, &DragStart_detour, o_DragStart);
    Install(kDragDraw, &DragDraw_detour, o_DragDraw);
    Install(kDragFinish, &DragFinish_detour, o_DragFinish);
    Install(kLineDraw, &Line_detour, o_Line);
    Install(kCityWidth, &CityWidth_detour, o_CityWidth);
    Install(kScrollGetDirection, &GetDirection_detour, o_GetDirection);
    core::scroll::camera::Start(kCamera, smooth);
    if (smooth) {
        picture::LogInstalled(kPicture);
    }
    return true;
}

bool SetSmooth(bool smooth)
{
    return core::scroll::camera::SetSmooth(kCamera, smooth);
}

void OnLoop(double frameMs)
{
    core::scroll::camera::OnLoop(kCamera, frameMs);
}

void Resume()
{
    core::scroll::camera::Resume();
}

void SetCityExtent(int right, int bottom)
{
    picture::SetCityExtent(right, bottom);
}

void SaveCityBuffer(int report)
{
    picture::SaveCityBuffer(report);
}

void TakeStats(char *out, size_t size)
{
    core::scroll::camera::TakeStats(kCamera, out, size);
}

const Detour *Detours(int *count)
{
    *count = static_cast<int>(sizeof(kDetours) / sizeof(kDetours[0]));
    return kDetours;
}

void SetRemainderForTest(int px, int py)
{
    core::scroll::camera::SetRemainderForTest(kCamera, px, py);
}

void Remainder(int *px, int *py)
{
    core::scroll::camera::Remainder(px, py);
}

void SetCityDrawn(CityDrawn fn)
{
    picture::SetCityDrawn(fn);
}

void DrawCityForPicture(int vx, int vy, int orientation)
{
    picture::DrawCity(kPicture, vx, vy, orientation);
}

bool Working()
{
    return core::diag::Enabled(g_feature);
}

bool OnCity(int x, int y)
{
    return picture::OnCity(kPicture, x, y);
}

const core::scroll::picture::PictureGame &Picture()
{
    return kPicture;
}

namespace {

const Var<char> kGameScreen{0x10de045};

// Drag scrolling (both games): the zoom's city screens (game_screen 1, the army screens 0x48 / 0x49; not 7,
// a sidebar panel's dialog). Emperor's editor is another program (EmperorEdit.exe), not ours.
bool DragScreen()
{
    const char s = *kGameScreen;
    return s == 1 || s == 0x48 || s == 0x49;
}

const core::scroll::drag::DragGame kDrag{
    "emperor",
    &DragScreen,
    [](int x, int y) { return picture::OnCity(kPicture, x, y); },
    [] { return *kPicture.vars.mouseMode == kPicture.mouseModeDragSelect; },
    &Working,
    &core::scroll::zoom::Mouse,
};

} // namespace

void AttachDrag()
{
    core::scroll::drag::Install(kDrag, core::scroll::drag::RealSystem());
}

} // namespace emperor::scroll
