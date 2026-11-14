// The world map screen at resolutions larger than the map, shared by Emperor's layer (the China map, games/emperor/
// china_map) and Zeus's (the world map, games/zeus/world_map).
//
// The game keeps drawing its map exactly as it always does -- the picture at (0, top bar), the origin there, scroll
// offsets 0 (they are saved in the savegame and stay untouched) -- but into a buffer of ours. It is then shown in the
// map area (left of the sidebar, below the top bar): the map inside the dialog panel's fret border
// (core/display/framed_screens: BorderPicture), scaled to the area (mode scaled: as large as fits, or a scale of
// one's own) or at its own size (native, the default), over the same map blurred and darkened or the city as it was
// when the map opened (background). City markers, names, routes and armies are part of the map's pixels, so they move
// and scale with it. The mouse is converted back to the game's own map coordinates only inside the map's input
// functions the game layer hands over (WithMapMouse); the sidebar, the top bar, panels and dialogs keep the real
// position. The game's top bar piece called from inside the map's drawing waits until the map is shown and is drawn on
// the screen (TopBar).
//
// The campaign map (screen 0x34 in both games: the map as a background, drawn on redraws, and every frame a sidebar
// with markers relative to the map's origin): CampaignDraw draws the frame's markers into our buffer, on the map the
// background drew there, the sidebar's strip back to the screen, and shows the map again.
//
// Nothing here knows a game address: the game layer gives its variables (core::site::Var, read at each use), its top
// bar height, when the map may be taken over at all, which screens may show the city around the map, the game's own
// map box (shown as the game drew it when the map is not where we expect it), the empty point for the mouse beside
// the map, and the calls of its own drawing, in a MapGame, every field required. The game layer hooks its functions
// and calls Draw, TopBar, WithMapMouse and CampaignDraw from its detours.
//
// A map wider than the map area is drawn the game's way. A map only taller than the area (Emperor's 848x746 below 786
// rows, e.g. 1366x768) is drawn whole into our buffer -- the game's height and its clip's bottom raised to the map's
// for the drawing and for the input's mouse -- and shown smaller, as large as fits (added for Zeus).
//
// Failure: one guard (the game layer's Feature). On an exception the map is drawn the game's own way from then on
// (top left, the real mouse).
#pragma once

#include "core/diag/guard.h"
#include "core/hook/site.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace core::display::map_screen {

// What the game gives. Every field is required (the constructor takes them all).
struct MapGame {
    using Allowed_fn = bool (*)();
    using DrawMap_fn = void (*)(core::diag::Restore &r); // the game's map drawing; changes go through r (undone after)
    using TopBar_fn = void (*)(int arg);                 // the game's own top bar piece (the unhooked function)
    using Box_fn = RECT (*)(int ddW, int ddH, int sidebarX, int mapW, int mapH);
    using Empty_fn = void (*)(int mapW, int mapH, int sidebarX, int *gx, int *gy);

    MapGame(const char *logPrefix, core::diag::Feature *feature, const char *phaseName,
            core::site::Var<BYTE *> drawBuffer, core::site::Var<int> pitch, core::site::Var<int> ddWidth,
            core::site::Var<int> ddHeight, core::site::Var<int> clipBottom, core::site::Var<char> redraw,
            core::site::Var<int> sidebarX, core::site::Var<int> mouseX, core::site::Var<int> mouseY, core::site::Var<int> originX,
            core::site::Var<int> originY, core::site::Var<int> scrollX, core::site::Var<int> scrollY,
            core::site::Var<int> mapW, core::site::Var<int> mapH, core::site::Var<int> mapNumber, int topBarH,
            int scrollSlack, Allowed_fn takeOver, Allowed_fn cityBackground, DrawMap_fn drawMap, TopBar_fn topBar, Box_fn gameBox,
            Empty_fn emptyPoint)
        : logPrefix(logPrefix), feature(feature), phaseName(phaseName), drawBuffer(drawBuffer), pitch(pitch),
          ddWidth(ddWidth), ddHeight(ddHeight), clipBottom(clipBottom), redraw(redraw), sidebarX(sidebarX),
          mouseX(mouseX), mouseY(mouseY),
          originX(originX), originY(originY), scrollX(scrollX), scrollY(scrollY), mapW(mapW), mapH(mapH),
          mapNumber(mapNumber), topBarH(topBarH), scrollSlack(scrollSlack), takeOver(takeOver), cityBackground(cityBackground),
          drawMap(drawMap), topBar(topBar), gameBox(gameBox), emptyPoint(emptyPoint)
    {
    }

    const char *logPrefix;   // the log lines' prefix, e.g. "china_map" or "zeus: world map"
    core::diag::Feature *feature; // the game layer's guard, e.g. "China map layout (games/emperor/china_map)"
    const char *phaseName;   // the performance phase of our composition (core/diag/perf)
    core::site::Var<BYTE *> drawBuffer; // the game's back buffer (RGB565)
    core::site::Var<int> pitch;         // its bytes per row
    core::site::Var<int> ddWidth, ddHeight;
    core::site::Var<int> clipBottom;    // the game's drawing clip, its bottom (exclusive): screen_set_mode sets it to
                                        // the screen's height; raised with ddHeight to draw a map taller than the area
    core::site::Var<char> redraw;       // screen_redraw: set when a setting changed (the campaign map's picture)
    core::site::Var<int> sidebarX;      // the map area ends there
    core::site::Var<int> mouseX, mouseY;
    core::site::Var<int> originX, originY; // where the game drew the map (expected: 0, topBarH)
    core::site::Var<int> scrollX, scrollY; // expected 0 (or down to -scrollSlack)
    core::site::Var<int> mapW, mapH;       // the loaded picture (0: none loaded yet)
    core::site::Var<int> mapNumber;        // which map (the background's cache key)
    int topBarH;                           // the map area starts below it
    int scrollSlack;                       // a map that fits may leave its scroll this far below 0 (the game draws
                                           // the picture at 0, the objects at the scroll: e.g. Zeus's -1)
    Allowed_fn takeOver;       // the game's own conditions (e.g. not the editor's map, not at the game's own sizes)
    Allowed_fn cityBackground; // background = city may be used on this screen (the campaign map: no city behind it)
    DrawMap_fn drawMap;        // the game's map drawing (the unhooked function, with what it needs around it)
    TopBar_fn topBar;          // the game's top bar piece, called later on the screen
    Box_fn gameBox;            // where the game draws its map on the screen (shown as drawn when the state is odd)
    Empty_fn emptyPoint;       // a point of the game's map coordinates where nothing is (the mouse beside the map)
};

// At attach, before any other call (the strings and functions must live as long as the DLL).
void Configure(const MapGame &game);

// From the game layer's detour of its map drawing: ours, or the game's own way (game.drawMap).
void Draw();

// From the game layer's detour of its top bar piece: true when the call waits until the map is shown (the detour
// returns without calling the game's function).
bool TopBar(int arg);

// Runs fn(ctx) with the game's mouse in its own map coordinates while the mouse is over the map area of a map we show;
// otherwise (or after an error, again) with the real mouse.
void WithMapMouse(void (*fn)(void *), void *ctx);

// From the game layer's detour of the campaign map's per-frame drawing (original: the unhooked function).
void CampaignDraw(void (*original)());

// Whether the map was shown by us on the last frame (the campaign map's frames follow it).
bool Shown();

// Settings: the choices and labels (the game layer registers the items with its own texts) and their apply
// functions (core::settings::OnChange); FitPercent ends the scale slider at what fits now.
extern const char *const kModes[3];
extern const char *const kModeLabels[2];
constexpr int kModeChoices = 2; // "original" is valid only for the offline tests
extern const char *const kBackgrounds[2];
extern const char *const kBackgroundLabels[2];
bool ApplyMode(int, const char *value, bool initial);
bool ApplyScale(int, const char *value, bool initial);
bool ApplyBackground(int, const char *value, bool initial);
int FitPercent();

// The current settings, for the game layer's log line: the mode's and background's ini spellings, the scale (0: fit).
const char *ModeName();
const char *BackgroundName();
int ScalePercent();

// For the offline tests. mode: 0 scaled, 1 native, 2 original (the game's own drawing); scalePercent 0: fit.
void SetModeForTest(int mode, int scalePercent);
void SetBackgroundForTest(bool city);

} // namespace core::display::map_screen
