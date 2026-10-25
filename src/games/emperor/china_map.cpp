#include "games/emperor/china_map.h"

#include "core/diag/guard.h"
#include "core/diag/perf.h"
#include "core/display/scaled_rect.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "games/emperor/fullscreen.h"
#include "games/emperor/resolution.h"

#include <cmath>
#include <cstring>
#include <vector>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/archive/resolution_ui_r2c.md §1 (how the game draws
// the map) and §5 (this design). All five hooked functions are __cdecl with a plain ret.
namespace emperor::china_map {
namespace {

// china_map_draw: loads the map (china_map_load -> china_map_center), writes the origin (0, 0x28), draws the picture
// at (0, 0x28) and calls top_bar_piece_and_popup(1); for screen 0x34 it stops there, otherwise it draws routes,
// armies, cities and their names relative to the origin. Every frame on 0x14 / 0x16 / 0x1e; on redraws for 0x1a,
// popups over the map and 0x34 (campaign_map_background 0x430700).
const Site kMapDraw{"china_map_draw", 0x5b9c40, {0x83, 0xec, 0x54, 0x53, 0x55, 0x56, 0x57, 0xe8}};
// The top bar's piece and the menu popup (game_frame calls it too): drawn on the real buffer after our composition,
// never into the map.
const Site kTopBar{"top_bar_piece_and_popup", 0x5b6480, {0x81, 0xec, 0x04, 0x01, 0x00, 0x00, 0xb9, 0x30}};
// Screen 0x14's input: sidebar and top bar buttons (real coordinates), then hover and clicks on the map: objects
// under the mouse (0x488240: "mouse - origin + scroll" against each city, and the screen rectangles recorded while
// drawing at 0x13f81ac), clickable map icons (0x489b00, rectangles recorded while drawing).
const Site kMapInput{"china_map_input", 0x489d20, {0x83, 0xec, 0x10, 0x53, 0x56, 0x57, 0xe8, 0xc5}};
// Screen 0x34 (the campaign map): its sidebar and the campaign's city markers with their names, relative to the
// origin, hover by 0x52b2d0 (mouse inside a rectangle) -- every frame, after the map picture of the background.
const Site kCampaignDraw{"campaign_map_sidebar_draw", 0x42f900, {0x0f, 0xbe, 0x05, 0x18, 0xe1, 0x0d, 0x01, 0x83}};
// Screen 0x34's click on a marker (0x52b2d0 again).
const Site kCampaignClick{"campaign_map_click", 0x42f7d0, {0x51, 0xa0, 0x70, 0xe0, 0x0d, 0x01, 0x53, 0x55}};

// The map's sidebar (china_map_frame_draw, under 0x5b81c0; __cdecl, ret): its panels end at 768 like the city's, so
// the leather below them comes from games/emperor/resolution as on the city screen (first R2c test: a gap left of
// the help button, and black below the sidebar after a resolution change on the map).
const Site kFrameDraw{"china_map_frame_draw", 0x5b81d0, {0xa1, 0xd8, 0xd0, 0xc5, 0x00, 0x83, 0xec, 0x08}};
// anim_advance_ui (__cdecl id, frame; ret; only china_map_draw calls it): images with animation speed 0 (the allies'
// flags, group 0x25a: 5 frames, speed 0 in China_Interface.sg3) step on every main-loop pass, so how fast they look
// depends on how often a frame is shown: about 20 per second in the original, about 100 with our output (first R2c
// test: the flags flap much faster than in the original). Here speed 0 steps at most every kSpeed0Ms, as at 20 FPS.
const Site kAnim{"anim_advance_ui", 0x5b9760, {0x53, 0x8b, 0x5c, 0x24, 0x0c, 0x56, 0x57, 0x33}};
// The city map panels of screen 0x16 ("Consult Spy Map" and the like; __thiscall on the panel object, x, y, w, h;
// ret 0x10), called by 0x5ca230 with the map window (0, 40, sidebar_x, height - 40). They lay the 231 x 457 grid of
// view_to_grid centred in that rectangle, 4 x 2 px per cell; but a row before the grid's first (when the rectangle is
// higher than 914 px) is skipped without moving down, so the city is drawn too high, above its panel (first R2c
// test). The rectangle given to them is cut to 924 x 914 around the same centre; the panel's click test (0x5cc1a0)
// uses the rectangle they store, so it follows.
const Site kSpyPanel{"city_map_panel_spy", 0x5cbc30, {0x81, 0xec, 0x90, 0x00, 0x00, 0x00, 0x56, 0x8b}};
const Site kCityPanel{"city_map_panel", 0x5cb170, {0x81, 0xec, 0xe4, 0x00, 0x00, 0x00, 0x8b, 0x81}};
constexpr int kGridW = 0xe7 * 4, kGridH = 0x1c9 * 2; // 924 x 914 px

const Var<BYTE *> kDrawBuffer{0x1156df8}; // draw_buffer (RGB565)
const Var<int> kPitch{0x10de000};         // bytes per row
const Var<int> kDdWidth{0x10de008};
const Var<int> kDdHeight{0x10de004};
const Var<char> kGameScreen{0x10de045};
const Var<char> kRedraw{0x10de036};      // screen_redraw
const Var<int> kSidebarX{0x13f8e0c};
const Var<int> kMouseX{0x10de0a0};
const Var<int> kMouseY{0x10de09c};
const Var<int> kOriginX{0x82b7b0};
const Var<int> kOriginY{0x82b7b4};
const Var<int> kScrollX{0xc05900};       // saved in the savegame: never written here
const Var<int> kScrollY{0xc058fc};
const Var<int> kMapW{0x82b7a8};          // the loaded picture (china_map_load)
const Var<int> kMapH{0x82b7ac};
const Var<int> kMapNumber{0xc058d4};     // which of China_MapOfChina01..04 (the map object 0xc058d0, +4)
const Var<DWORD> kFrameTime{0x1156dec};  // timeGetTime() at the start of each game_frame
const Var<BYTE> kTimerFlag0{0x8901cc};   // frame_timer_flags[0]: set on every game_frame
constexpr DWORD kSpeed0Ms = 50;

constexpr int kTopBarH = 0x28;            // the origin's y
constexpr int kBorder = 16;               // the fret border, in map pixels
constexpr double kBlurSigma = 3.0;        // the backdrop's blur, in screen pixels (playtest: mockup bg_sigma3_bright55)
constexpr int kBrightness = 141;          // the backdrop's brightness, /256 (55 %)
constexpr char kEditorMap = 0x15;

using VoidFn = void(__cdecl *)();
using TopBarFn = int(__cdecl *)(int);
using AnimFn = unsigned(__cdecl *)(int, unsigned);
using PanelFn = void(__fastcall *)(void *, void *, int, int, int, int); // __thiscall: ecx = the panel, ret 0x10
VoidFn o_MapDraw, o_MapInput, o_CampaignDraw, o_CampaignClick, o_FrameDraw;
TopBarFn o_TopBar;
AnimFn o_Anim;
PanelFn o_SpyPanel, o_CityPanel;

core::diag::Feature g_feature{"China map layout (games/emperor/china_map)"};
const int kPhase = core::diag::perf::Register("China map layer (ours)", core::diag::perf::Thread::Game);

enum Mode { kScaled, kNative, kOriginal };
const char *const kModes[] = {"scaled", "native", "original"};
const char *const kModeLabels[] = {"scaled (fills the map area)", "native (the picture's own size, centered)",
                                   "original (as the game draws it)"};
int g_mode = kNative; // R2d.1: the picture's own size by default (playtest 2026-10-25)
bool g_integer = false;

// Our buffers.
BYTE *g_layer = nullptr; // the game draws the map here: same layout as the back buffer
size_t g_layerSize = 0;
std::vector<WORD> g_canvas; // border + map, (map + 2 border) pixels square
int g_canvasW = 0, g_canvasH = 0, g_border = 0;
std::vector<WORD> g_backdrop; // the map area's background
int g_backdropKey[5] = {};

// Where the map is shown now (for the mouse); valid only for the layout it was computed for.
struct Shown {
    bool valid;
    int ddW, ddH, sidebarX;
    int x, y;         // the framed map's (border + map) top left on the screen
    double scale;
    int border;       // in map pixels
    int canvasW, canvasH, mapW, mapH;
} g_shown{};

bool g_deferTop = false; // inside our china_map_draw call: top_bar_piece_and_popup waits
bool g_topPending = false;
int g_topArg = 0;
long g_logs = 0;
constexpr long kLogs = 12;

void Log(const char *fmt, int a = 0, int b = 0, int c = 0, int d = 0, int e = 0, int f = 0)
{
    if (++g_logs <= kLogs) {
        core_log(fmt, a, b, c, d, e, f);
    }
}

// The layout the composition needs: the whole map at its own size inside the area (so the game's scroll offsets are 0).
struct Layout {
    int ddW, ddH, pitch, sidebarX, mapW, mapH;
};

bool CurrentLayout(Layout *l)
{
    l->ddW = *kDdWidth;
    l->ddH = *kDdHeight;
    l->pitch = *kPitch;
    l->sidebarX = *kSidebarX;
    l->mapW = *kMapW;
    l->mapH = *kMapH;
    return g_mode != kOriginal && *kGameScreen != kEditorMap && l->ddW > 0 && l->ddH > 0 && l->pitch >= l->ddW * 2 &&
           l->mapW > 0 && l->mapH > 0 && l->mapW <= l->sidebarX && l->sidebarX <= l->ddW &&
           l->mapH <= l->ddH - kTopBarH && *kDrawBuffer;
}

bool LayerBuffer(size_t size)
{
    if (size <= g_layerSize && g_layer) {
        return true;
    }
    if (g_layer) {
        VirtualFree(g_layer, 0, MEM_RELEASE);
    }
    g_layer = static_cast<BYTE *>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_layerSize = g_layer ? size : 0;
    return g_layer != nullptr;
}

// The map's pixels in our layer: (0, 40), map size, rows of the back buffer's pitch.
core::display::Pixels16 MapPixels(const Layout &l)
{
    return core::display::Pixels16{reinterpret_cast<const WORD *>(g_layer + static_cast<size_t>(kTopBarH) * l.pitch),
                                   l.pitch / 2, l.mapW, l.mapH};
}

void Canvas(const Layout &l)
{
    const int b = kBorder;
    if (g_canvasW != l.mapW + 2 * b || g_canvasH != l.mapH + 2 * b) {
        g_canvasW = l.mapW + 2 * b;
        g_canvasH = l.mapH + 2 * b;
        g_border = b;
        if (!emperor::fullscreen::BorderPicture(g_canvasW, g_canvasH, &g_canvas)) {
            Log("china_map: no fret border (the panel tiles could not be read): the map without it");
            g_canvasW = l.mapW;
            g_canvasH = l.mapH;
            g_border = 0;
            g_canvas.assign(static_cast<size_t>(g_canvasW) * g_canvasH, 0);
        }
    }
    const core::display::Pixels16 m = MapPixels(l);
    for (int y = 0; y < l.mapH; ++y) {
        std::memcpy(&g_canvas[static_cast<size_t>(y + g_border) * g_canvasW + g_border], m.px + static_cast<size_t>(y) * m.stride,
                    static_cast<size_t>(l.mapW) * 2);
    }
}

void Backdrop(const Layout &l, int areaW, int areaH)
{
    const int key[5] = {*kMapNumber, l.mapW, l.mapH, areaW, areaH};
    if (std::memcmp(key, g_backdropKey, sizeof(key)) == 0 && g_backdrop.size() == static_cast<size_t>(areaW) * areaH) {
        return;
    }
    std::memcpy(g_backdropKey, key, sizeof(key));
    core::display::BlurredBackdrop(MapPixels(l), areaW, areaH, kBlurSigma, kBrightness, &g_backdrop);
    Log("china_map: backdrop for map %d (%dx%d) over the %dx%d map area", key[0], l.mapW, l.mapH, areaW, areaH);
}

double Scale(int areaW, int areaH)
{
    if (g_mode == kNative) {
        return 1.0;
    }
    double s = std::fmin(static_cast<double>(areaW) / g_canvasW, static_cast<double>(areaH) / g_canvasH);
    if (g_integer) {
        s = std::floor(s);
    }
    return s < 1.0 ? 1.0 : s;
}

// Backdrop and the framed map into `target` (the buffer the game draws to outside our call).
void Compose(const Layout &l, BYTE *target, bool backdrop)
{
    const int areaW = l.sidebarX, areaH = l.ddH - kTopBarH;
    Canvas(l);
    const double s = Scale(areaW, areaH);
    const int tw = static_cast<int>(std::lround(g_canvasW * s)), th = static_cast<int>(std::lround(g_canvasH * s));
    const int tx = (areaW - tw) / 2, ty = kTopBarH + (areaH - th) / 2;
    const core::display::Target16 dst{reinterpret_cast<WORD *>(target), l.pitch / 2, l.ddW, l.ddH};
    const RECT area{0, kTopBarH, areaW, l.ddH};
    if (backdrop) {
        Backdrop(l, areaW, areaH);
        for (int y = 0; y < areaH; ++y) {
            std::memcpy(dst.px + static_cast<size_t>(y + kTopBarH) * dst.stride, &g_backdrop[static_cast<size_t>(y) * areaW],
                        static_cast<size_t>(areaW) * 2);
        }
    }
    const bool whole = std::fabs(s - std::round(s)) < 1e-9;
    core::display::ScaleRgb565(core::display::Pixels16{g_canvas.data(), g_canvasW, g_canvasW, g_canvasH}, s, !whole, dst,
                               tx, ty, area);
    g_shown = Shown{true, l.ddW, l.ddH, l.sidebarX, tx, ty, s, g_border, g_canvasW, g_canvasH, l.mapW, l.mapH};
}

// The real mouse in the game's own map coordinates while the map is shown by us. Outside the map but inside the
// map area: a point of the game's map window right of the picture (nothing is there: a click deselects, as on the
// empty map). Elsewhere (sidebar, top bar): false, the real position.
bool MapMouse(int mx, int my, int *gx, int *gy)
{
    const Shown &s = g_shown;
    if (!s.valid || s.ddW != *kDdWidth || s.ddH != *kDdHeight || s.sidebarX != *kSidebarX) {
        return false;
    }
    int u, v; // the pixel of border + map drawn there (the same rounding as the drawing)
    if (core::display::ScaledToSource(mx, my, s.x, s.y, s.scale, s.canvasW, s.canvasH, &u, &v) && u >= s.border &&
        v >= s.border && u < s.border + s.mapW && v < s.border + s.mapH) {
        *gx = u - s.border; // the game's origin is (0, 40)
        *gy = v - s.border + kTopBarH;
        return true;
    }
    if (mx >= 0 && mx < s.sidebarX && my >= kTopBarH && my < s.ddH) {
        *gx = s.mapW + 2 < s.sidebarX ? s.mapW + 2 : -10000;
        *gy = kTopBarH + 2;
        return true;
    }
    return false;
}

struct Call {
    Layout l;
    BYTE *target;
    core::diag::Restore r;
};

void DrawLayer(void *p)
{
    Call &c = *static_cast<Call *>(p);
    const size_t size = static_cast<size_t>(c.l.pitch) * c.l.ddH;
    if (!LayerBuffer(size)) {
        Original(o_MapDraw, kMapDraw)();
        return;
    }
    c.r.Set(kDrawBuffer.ptr(), g_layer);
    c.r.Set(&g_deferTop, true);
    g_topPending = false;
    Original(o_MapDraw, kMapDraw)();
    c.r.Undo();
    if (*kOriginX != 0 || *kOriginY != kTopBarH || *kScrollX != 0 || *kScrollY != 0 || *kMapW != c.l.mapW ||
        *kMapH != c.l.mapH) {
        // Not the layout we expected (a new picture, or the game scrolls): show what it drew, as it drew it.
        Log("china_map: unexpected map state (origin %d,%d scroll %d,%d map %dx%d): drawn the game's way", *kOriginX,
            *kOriginY, *kScrollX, *kScrollY, *kMapW, *kMapH);
        const int right = c.l.sidebarX + 16 < c.l.ddW ? c.l.sidebarX + 16 : c.l.ddW; // the game's map window
        for (int y = kTopBarH; y < c.l.ddH; ++y) {
            std::memcpy(c.target + static_cast<size_t>(y) * c.l.pitch, g_layer + static_cast<size_t>(y) * c.l.pitch,
                        static_cast<size_t>(right) * 2);
        }
        g_shown.valid = false;
    }
    else {
        core::diag::perf::Scope timed(kPhase);
        Compose(c.l, c.target, true);
    }
    if (g_topPending) {
        g_topPending = false;
        Original(o_TopBar, kTopBar)(g_topArg);
    }
}

void __cdecl MapDraw_detour()
{
    Call c{};
    if (!core::diag::Enabled(g_feature) || !CurrentLayout(&c.l)) {
        g_shown.valid = false;
        Original(o_MapDraw, kMapDraw)();
        return;
    }
    c.target = *kDrawBuffer;
    if (!core::diag::Guarded(g_feature, &DrawLayer, &c, &c.r)) {
        g_shown.valid = false;
        g_topPending = false;
        Original(o_MapDraw, kMapDraw)(); // the game's own way from now on
    }
}

int __cdecl TopBar_detour(int arg)
{
    if (g_deferTop) {
        g_topPending = true;
        g_topArg = arg;
        return 0;
    }
    return Original(o_TopBar, kTopBar)(arg);
}

// Runs fn with the mouse in the game's map coordinates (when the map is ours and the mouse is over the map area).
void WithMapMouse(VoidFn original, const Site &site)
{
    int gx = 0, gy = 0;
    if (!core::diag::Enabled(g_feature) || !MapMouse(*kMouseX, *kMouseY, &gx, &gy)) {
        Original(original, site)();
        return;
    }
    struct Ctx {
        VoidFn fn;
        const Site *site;
        int gx, gy;
        core::diag::Restore r;
    } ctx{original, &site, gx, gy, {}};
    if (!core::diag::Guarded(
            g_feature,
            [](void *p) {
                Ctx &c = *static_cast<Ctx *>(p);
                c.r.Set(kMouseX.ptr(), c.gx);
                c.r.Set(kMouseY.ptr(), c.gy);
                Original(c.fn, *c.site)();
                c.r.Undo();
            },
            &ctx, &ctx.r)) {
        g_shown.valid = false;
        Original(original, site)();
    }
}

void __cdecl MapInput_detour()
{
    WithMapMouse(o_MapInput, kMapInput);
}

void __cdecl CampaignClick_detour()
{
    WithMapMouse(o_CampaignClick, kCampaignClick);
}

// Screen 0x34: the markers go into our layer (onto the map the background drew there), the sidebar to the screen;
// then the map is shown again.
void CampaignLayer(void *p)
{
    Call &c = *static_cast<Call *>(p);
    core::diag::perf::Scope timed(kPhase);
    const int pitch = c.l.pitch, x0 = c.l.sidebarX, bytes = (c.l.ddW - x0) * 2;
    for (int y = 0; y < c.l.ddH; ++y) { // the sidebar's strip as it is on the screen
        std::memcpy(g_layer + static_cast<size_t>(y) * pitch + x0 * 2, c.target + static_cast<size_t>(y) * pitch + x0 * 2, bytes);
    }
    int gx = 0, gy = 0;
    c.r.Set(kDrawBuffer.ptr(), g_layer);
    if (MapMouse(*kMouseX, *kMouseY, &gx, &gy)) {
        c.r.Set(kMouseX.ptr(), gx);
        c.r.Set(kMouseY.ptr(), gy);
    }
    Original(o_CampaignDraw, kCampaignDraw)();
    c.r.Undo();
    for (int y = 0; y < c.l.ddH; ++y) {
        std::memcpy(c.target + static_cast<size_t>(y) * pitch + x0 * 2, g_layer + static_cast<size_t>(y) * pitch + x0 * 2, bytes);
    }
    Compose(c.l, c.target, false);
}

void __cdecl CampaignDraw_detour()
{
    Call c{};
    const size_t size = static_cast<size_t>(*kPitch) * *kDdHeight;
    if (!core::diag::Enabled(g_feature) || !CurrentLayout(&c.l) || !g_shown.valid || g_shown.ddW != c.l.ddW ||
        g_shown.ddH != c.l.ddH || !g_layer || g_layerSize < size) {
        Original(o_CampaignDraw, kCampaignDraw)();
        emperor::resolution::DrawSidebarFiller();
        return;
    }
    c.target = *kDrawBuffer;
    if (!core::diag::Guarded(g_feature, &CampaignLayer, &c, &c.r)) {
        g_shown.valid = false;
        *kRedraw = 1; // the background draws the map the game's way again
        Original(o_CampaignDraw, kCampaignDraw)();
    }
    emperor::resolution::DrawSidebarFiller();
}

void __cdecl FrameDraw_detour()
{
    Original(o_FrameDraw, kFrameDraw)();
    emperor::resolution::DrawSidebarFiller();
}

DWORD g_animFrame = 0, g_animLast = 0;
bool g_animStep = true;

unsigned __cdecl Anim_detour(int id, unsigned frame)
{
    const DWORD now = *kFrameTime;
    if (now != g_animFrame) { // the first call in this game_frame decides for all of them
        g_animFrame = now;
        g_animStep = now - g_animLast >= kSpeed0Ms;
        if (g_animStep) {
            g_animLast = now;
        }
    }
    if (g_animStep || !core::diag::Enabled(g_feature)) {
        return Original(o_Anim, kAnim)(id, frame);
    }
    struct Ctx {
        int id;
        unsigned frame, result;
        core::diag::Restore r;
    } c{id, frame, 0, {}};
    if (core::diag::Guarded(
            g_feature,
            [](void *p) {
                Ctx &c = *static_cast<Ctx *>(p);
                c.r.Set(kTimerFlag0.ptr(), static_cast<BYTE>(0)); // speed 0 waits; other speeds keep their timers
                c.result = Original(o_Anim, kAnim)(c.id, c.frame);
                c.r.Undo();
            },
            &c, &c.r)) {
        return c.result;
    }
    return Original(o_Anim, kAnim)(id, frame);
}

void CutToGrid(int *x, int *y, int *w, int *h)
{
    if (*h > kGridH) {
        *y += ((*h - kGridH) / 2) & ~1;
        *h = kGridH;
    }
    if (*w > kGridW) {
        *x += ((*w - kGridW) / 2) & ~3;
        *w = kGridW;
    }
}

void __fastcall SpyPanel_detour(void *self, void *, int x, int y, int w, int h)
{
    CutToGrid(&x, &y, &w, &h);
    Original(o_SpyPanel, kSpyPanel)(self, nullptr, x, y, w, h);
}

void __fastcall CityPanel_detour(void *self, void *, int x, int y, int w, int h)
{
    CutToGrid(&x, &y, &w, &h);
    Original(o_CityPanel, kCityPanel)(self, nullptr, x, y, w, h);
}

bool ApplyMode(int, const char *v, bool initial)
{
    g_mode = kNative;
    for (int i = 0; i < 3; ++i) {
        if (std::strcmp(v, kModes[i]) == 0) {
            g_mode = i;
        }
    }
    if (!initial) {
        *kRedraw = 1; // redrawn in the new way (the campaign map's picture is drawn only on redraws)
    }
    return true;
}

bool ApplyInteger(int, const char *v, bool initial)
{
    g_integer = std::strcmp(v, "on") == 0;
    if (!initial) {
        *kRedraw = 1;
    }
    return true;
}

const Detour kDetours[] = {
    {&kMapDraw, reinterpret_cast<void *>(&MapDraw_detour)},
    {&kTopBar, reinterpret_cast<void *>(&TopBar_detour)},
    {&kMapInput, reinterpret_cast<void *>(&MapInput_detour)},
    {&kCampaignDraw, reinterpret_cast<void *>(&CampaignDraw_detour)},
    {&kCampaignClick, reinterpret_cast<void *>(&CampaignClick_detour)},
    {&kFrameDraw, reinterpret_cast<void *>(&FrameDraw_detour)},
    {&kAnim, reinterpret_cast<void *>(&Anim_detour)},
    {&kSpyPanel, reinterpret_cast<void *>(&SpyPanel_detour)},
    {&kCityPanel, reinterpret_cast<void *>(&CityPanel_detour)},
};

} // namespace

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "display", "china_map", "China map",
        "The map of China at resolutions larger than the map. scaled: as large as the map area allows, in a fret "
        "border, over the same map blurred and darkened; native: the picture's own size, centered, the same "
        "background; original: as the game draws it (top left, the rest of the screen not redrawn).",
        Type::Choice, "native", Effect::Live, true, true, 0, 0, 0, nullptr, kModes, kModeLabels, 3});
    core::settings::OnChange(i, &ApplyMode);
    i = core::settings::Register(Item{
        "display", "china_map_integer", "China map: integer scaling",
        "For China map = scaled: only whole-number scales (sharper, smaller; at 2560x1440 the map stays at its own "
        "size).",
        Type::Bool, "off", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, &ApplyInteger);
}

bool Attach()
{
    for (const Detour &d : kDetours) {
        if (!Matches(*d.site)) {
            core_log("china_map: %s at 0x%lx does not match: the China map is drawn the game's way", d.site->name,
                     d.site->address);
            return false;
        }
    }
    Install(kMapDraw, &MapDraw_detour, o_MapDraw);
    Install(kTopBar, &TopBar_detour, o_TopBar);
    Install(kMapInput, &MapInput_detour, o_MapInput);
    Install(kCampaignDraw, &CampaignDraw_detour, o_CampaignDraw);
    Install(kCampaignClick, &CampaignClick_detour, o_CampaignClick);
    Install(kFrameDraw, &FrameDraw_detour, o_FrameDraw);
    Install(kAnim, &Anim_detour, o_Anim);
    Install(kSpyPanel, &SpyPanel_detour, o_SpyPanel);
    Install(kCityPanel, &CityPanel_detour, o_CityPanel);
    core_log("china_map: installed (display.china_map %s%s)", kModes[g_mode], g_integer ? ", integer scaling" : "");
    return true;
}

const Detour *Detours(int *count)
{
    *count = static_cast<int>(sizeof(kDetours) / sizeof(kDetours[0]));
    return kDetours;
}

void SetModeForTest(int mode, bool integer)
{
    g_mode = mode;
    g_integer = integer;
}

} // namespace emperor::china_map
