#include "games/emperor/china_map.h"

#include "core/diag/guard.h"
#include "core/diag/perf.h"
#include "core/display/map_screen.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "core/sim/ui_anim.h"
#include "games/emperor/resolution.h"

#include <cstdio>
#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): how the game draws
// the map was read from the disassembly. All five hooked functions are __cdecl with a plain ret.
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
// the leather below them comes from games/emperor/resolution as on the city screen (seen in testing: a gap left of
// the help button, and black below the sidebar after a resolution change on the map).
const Site kFrameDraw{"china_map_frame_draw", 0x5b81d0, {0xa1, 0xd8, 0xd0, 0xc5, 0x00, 0x83, 0xec, 0x08}};
// anim_advance_ui (__cdecl id, frame; ret; only china_map_draw calls it): images with animation speed 0 (the allies'
// flags, group 0x25a: 5 frames, speed 0 in China_Interface.sg3) step on every main-loop pass, so how fast they look
// depends on how often a frame is shown: about 20 per second in the original, about 100 with our output (seen in
// test: the flags flap much faster than in the original). Here speed 0 steps at most every 50 ms, as at 20 FPS
// (core/sim/ui_anim.h).
const Site kAnim{"anim_advance_ui", 0x5b9760, {0x53, 0x8b, 0x5c, 0x24, 0x0c, 0x56, 0x57, 0x33}};
// The city map panels of screen 0x16 ("Consult Spy Map" and the like; __thiscall on the panel object, x, y, w, h;
// ret 0x10), called by 0x5ca230 with the map window (0, 40, sidebar_x, height - 40). They lay the 231 x 457 grid of
// view_to_grid centred in that rectangle, 4 x 2 px per cell; but a row before the grid's first (when the rectangle is
// higher than 914 px) is skipped without moving down, so the city is drawn too high, above its panel (seen in
// test). The rectangle given to them is cut to 924 x 914 around the same centre; the panel's click test (0x5cc1a0)
// uses the rectangle they store, so it follows.
const Site kSpyPanel{"city_map_panel_spy", 0x5cbc30, {0x81, 0xec, 0x90, 0x00, 0x00, 0x00, 0x56, 0x8b}};
const Site kCityPanel{"city_map_panel", 0x5cb170, {0x81, 0xec, 0xe4, 0x00, 0x00, 0x00, 0x8b, 0x81}};
constexpr int kGridW = 0xe7 * 4, kGridH = 0x1c9 * 2; // 924 x 914 px

const Var<BYTE *> kDrawBuffer{0x1156df8}; // draw_buffer (RGB565)
const Var<int> kPitch{0x10de000};         // bytes per row
const Var<int> kDdWidth{0x10de008};
const Var<int> kDdHeight{0x10de004};
const Var<int> kClipBottom{0x10c7318}; // screen_clip_rect's bottom (exclusive)
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

constexpr int kTopBarH = 0x28;            // the origin's y
constexpr char kEditorMap = 0x15;

using VoidFn = void(__cdecl *)();
using TopBarFn = int(__cdecl *)(int);
using AnimFn = unsigned(__cdecl *)(int, unsigned);
using PanelFn = void(__fastcall *)(void *, void *, int, int, int, int); // __thiscall: ecx = the panel, ret 0x10
VoidFn o_MapDraw, o_MapInput, o_CampaignDraw, o_CampaignClick, o_FrameDraw;
TopBarFn o_TopBar;
AnimFn o_Anim;
PanelFn o_SpyPanel, o_CityPanel;

namespace map_screen = core::display::map_screen;

core::diag::Feature g_feature{"China map layout (games/emperor/china_map)"};

// The map's layout, drawing and mouse are the core's (core/display/map_screen, shared with Zeus's world map); here
// the game's variables and functions.
bool TakeOver()
{
    return *kGameScreen != kEditorMap;
}

bool CityBackground()
{
    return true; // the campaign map (0x34) is not reached in Emperor's campaigns
}

void DrawMap(core::diag::Restore &)
{
    Original(o_MapDraw, kMapDraw)();
}

void DrawTopBar(int arg)
{
    Original(o_TopBar, kTopBar)(arg);
}

// The game's map window: the picture is drawn into (0, 40) .. (sidebar_x + 16, height).
RECT GameBox(int ddW, int ddH, int sidebarX, int, int)
{
    return RECT{0, kTopBarH, sidebarX + 16 < ddW ? sidebarX + 16 : ddW, ddH};
}

// A point of the game's map window right of the picture (nothing is there).
void EmptyPoint(int mapW, int, int sidebarX, int *gx, int *gy)
{
    *gx = mapW + 2 < sidebarX ? mapW + 2 : -10000;
    *gy = kTopBarH + 2;
}

const map_screen::MapGame kGame("china_map", &g_feature, "China map layer (ours)",
                                kDrawBuffer, kPitch, kDdWidth, kDdHeight, kClipBottom, kRedraw, kSidebarX, kMouseX, kMouseY,
                                kOriginX, kOriginY, kScrollX, kScrollY, kMapW, kMapH, kMapNumber, kTopBarH, 0, &TakeOver,
                                &CityBackground, &DrawMap, &DrawTopBar, &GameBox, &EmptyPoint);

void __cdecl MapDraw_detour()
{
    map_screen::Draw();
}

int __cdecl TopBar_detour(int arg)
{
    if (map_screen::TopBar(arg)) {
        return 0;
    }
    return Original(o_TopBar, kTopBar)(arg);
}

void __cdecl MapInput_detour()
{
    map_screen::WithMapMouse([](void *) { Original(o_MapInput, kMapInput)(); }, nullptr);
}

void __cdecl CampaignClick_detour()
{
    map_screen::WithMapMouse([](void *) { Original(o_CampaignClick, kCampaignClick)(); }, nullptr);
}

// Screen 0x34: the markers go into our layer, the sidebar to the screen (core); then the leather below the sidebar.
void __cdecl CampaignDraw_detour()
{
    map_screen::CampaignDraw([] { Original(o_CampaignDraw, kCampaignDraw)(); });
    emperor::resolution::DrawSidebarFiller();
}

void __cdecl FrameDraw_detour()
{
    Original(o_FrameDraw, kFrameDraw)();
    emperor::resolution::DrawSidebarFiller();
}

core::sim::Speed0Gate g_animGate; // shared with Zeus's world map (core/sim/ui_anim.h)

unsigned __cdecl Anim_detour(int id, unsigned frame)
{
    const DWORD now = *kFrameTime; // the frame's start time: changes once per game_frame
    if (g_animGate.Step(now, now) || !core::diag::Enabled(g_feature)) {
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
        "video", "empire_map", "Empire map",
        "The empire map at resolutions larger than the map. scaled: larger than its own size (Empire map: scale), in "
        "a fret border, over the background (Empire map: background); native: the picture's own size, centered, the "
        "same background.",
        Type::Choice, "native", Effect::Live, true, true, 0, 0, 0, nullptr, map_screen::kModes, map_screen::kModeLabels,
        map_screen::kModeChoices});
    core::settings::OnChange(i, &map_screen::ApplyMode);
    // [display] china_map* became [video] empire_map* (the campaign editor's name for the map), and
    // original is no longer a choice.
    core::settings::MoveKey("display", "china_map", "video", "empire_map");
    core::settings::MoveKey("display", "china_map_scale", "video", "empire_map_scale");
    core::settings::MoveKey("display", "china_map_background", "video", "empire_map_background");
    core::settings::RenameValue("video", "empire_map", "original", "native");
    // Whole-number scales gave only 100% below 4K; a scale of one's own covers it.
    core::settings::Retire("display", "china_map_integer", nullptr);
    i = core::settings::Register(Item{
        "video", "empire_map_scale", "Empire map: scale",
        "For Empire map = scaled: how large, in percent of the map's own size. Auto: as large as the map area allows "
        "(fit). The slider ends at what fits for the map last shown at this resolution; a larger value in "
        "emperor_engine.ini is shown as large as fits.",
        Type::Int, "fit", Effect::Live, true, true, 100, 400, 5, "fit", nullptr, nullptr, 0, "%", &map_screen::FitPercent,
        "empire_map", "scaled"});
    core::settings::OnChange(i, &map_screen::ApplyScale);
    i = core::settings::Register(Item{
        "video", "empire_map_background", "Empire map: background",
        "What is around the map (Empire map = scaled or native). map: the same map, blurred and darkened; city: your "
        "city as it was when you opened the map, as it is (the map like a window over it); it stands still while "
        "the map is shown.",
        Type::Choice, "map", Effect::Live, true, true, 0, 0, 0, nullptr, map_screen::kBackgrounds,
        map_screen::kBackgroundLabels, 2});
    core::settings::OnChange(i, &map_screen::ApplyBackground);
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
    map_screen::Configure(kGame);
    Install(kMapDraw, &MapDraw_detour, o_MapDraw);
    Install(kTopBar, &TopBar_detour, o_TopBar);
    Install(kMapInput, &MapInput_detour, o_MapInput);
    Install(kCampaignDraw, &CampaignDraw_detour, o_CampaignDraw);
    Install(kCampaignClick, &CampaignClick_detour, o_CampaignClick);
    Install(kFrameDraw, &FrameDraw_detour, o_FrameDraw);
    Install(kAnim, &Anim_detour, o_Anim);
    Install(kSpyPanel, &SpyPanel_detour, o_SpyPanel);
    Install(kCityPanel, &CityPanel_detour, o_CityPanel);
    char scale[16];
    std::snprintf(scale, sizeof(scale), map_screen::ScalePercent() ? "%d%%" : "fit", map_screen::ScalePercent());
    core_log("china_map: installed (video.empire_map %s,scale %s, background %s)", map_screen::ModeName(), scale,
             map_screen::BackgroundName());
    return true;
}

const Detour *Detours(int *count)
{
    *count = static_cast<int>(sizeof(kDetours) / sizeof(kDetours[0]));
    return kDetours;
}

void SetModeForTest(int mode, int scalePercent)
{
    map_screen::Configure(kGame);
    map_screen::SetModeForTest(mode, scalePercent);
}

} // namespace emperor::china_map
