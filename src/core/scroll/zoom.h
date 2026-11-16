// The city's zoom for this game family (Emperor and Zeus): the scale from the
// settings ([video] city_zoom 50..150, city_zoom_shrink), the wheel over the city (25 a notch around the cursor,
// smooth) and the middle button (back to 100), the layout's view for a scale, and the zoomed city drawn into our buffer
// and scaled onto the screen. Nothing here knows a game's addresses: the game layer gives its variables and callbacks
// in a ZoomGame (Install), keeps the hooks (view_clamp, line_draw) and calls the functions below from them and from its
// layout.
//
// The game's view is the zoomed one all the time (the layout asks ViewFor), so view_clamp, centring, rotation, the
// minimap's frame and bookmarks follow it; the zoomed city is drawn by smooth scrolling's picture
// (core/scroll/picture.h: its zoomPercent, zoomBufferSize, renderZoomed, dragDrawZoom and dragLinesMap are
// Percent, BufferSize, RenderZoomed, DragDrawZoom and DragLinesMap here), the camera's zoom callbacks
// (core/scroll/camera.h: zoomMoving, zoomFrame, zoomScale) are CameraMoving, CameraStep and CameraScale here. The game
// layer passes them in its PictureGame and CameraGame; before Install (or without it: smooth scrolling not installed)
// they say "100%" / "not moving".
//
// The game's view must never be larger than the map's range (core/display/city_zoom.h): the scale is capped by it,
// worked out again when the map or the screen changes, and checked once more before every zoomed frame (a view that
// does not fit is not drawn that frame).
//
// Failure: the wheel's handling runs under this file's guard; any failure of the zoom (the wheel, no memory for the
// buffer, the scaling, smooth scrolling switched off after a fault) switches the zoom off for the rest of the run: the
// view laid out at 100 first, then the city is drawn the game's way. Game thread only.
#pragma once

#include "core/diag/guard.h"
#include "core/hook/site.h"
#include "core/scroll/picture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace core::scroll::zoom {

// What the zoom needs from the game (a constant of the game layer; every field required).
struct ZoomGame {
    ZoomGame(const char *log, const picture::PictureGame *picture, core::site::Var<int> viewDrawnX,
             core::site::Var<int> viewDrawnY, core::site::Var<char> redraw, core::site::Var<int> sidebarX, int cap,
             void (*mapRange)(int *cols, int *rows, int *clampRows), bool (*cityScreen)(), void (*relayout)(),
             void (*clamp)(), bool (*drawingWorks)())
        : log(log), picture(picture), viewDrawnX(viewDrawnX), viewDrawnY(viewDrawnY), redraw(redraw),
          sidebarX(sidebarX), cap(cap), mapRange(mapRange), cityScreen(cityScreen), relayout(relayout), clamp(clamp),
          drawingWorks(drawingWorks)
    {
    }

    const char *log;                       // the prefix of the log lines that say which game, e.g. "zeus"
    const picture::PictureGame *picture;   // smooth scrolling's picture (its camera, variables, OnCity, DrawCity)
    core::site::Var<int> viewDrawnX;       // the view as city_render last drew it (mouse_to_grid's table lookup);
    core::site::Var<int> viewDrawnY;       // viewport_init sets it to the map's middle, so a relayout writes it again
    core::site::Var<char> redraw;          // screen_redraw: the background (bottom strip, sidebar) drawn again
    core::site::Var<int> sidebarX;         // the sidebar's left edge (the drag selection's limit)
    int cap;                               // the largest percent used (speed)
    // The map's range now (view_clamp's limits: columns, half rows; 0 or less before a map) and the half rows the
    // clamp can hold (the map's minus the bottom margin the game keeps; each game has its own rule).
    void (*mapRange)(int *cols, int *rows, int *clampRows);
    bool (*cityScreen)();   // the game's screen is a city screen (the wheel and the middle button are ours there)
    void (*relayout)();     // the city's viewport laid out again (the layout's viewport hook; it calls ViewFor)
    void (*clamp)();        // the game's own view_clamp (the original function, not the game layer's hook)
    bool (*drawingWorks)(); // smooth scrolling's picture is installed and working (it draws the zoomed city)
};

// Start-up (the game layer's settings registration, at the place the items should take in the panel): [video]
// city_zoom and city_zoom_shrink.
void RegisterSettings();

// From the game layer's Attach once its hooks are in: the mouse filter (the wheel, the middle button) and the log line.
void Install(const ZoomGame &g);

// The zoom's mouse filter (the wheel, a middle press over the city): true when it took the message. Installed as
// core::display's filter by Install; core/scroll/drag's filter, once installed, passes on what it does not take.
bool Mouse(UINT message, WPARAM wparam, int x, int y);

// From the game layer's view_clamp detour, before the game's own: a new map, another bottom margin or a view larger
// than the map gets the view laid out again first, so the view is never clamped (and then drawn) larger than the map.
void OnViewClamp();

// The scale in effect for the city on the screen (showW x showH pixels, columns of colPx, half rows of halfH) and the
// current map: the game's view in columns and half rows for it (cols / rows changed only when not 100). Logged when it
// changes (not while the wheel moves the scale). 100 when zoom is not available (not installed, switched off, smooth
// scrolling not working, no map yet).
int ViewFor(int showW, int showH, int colPx, int halfH, int *cols, int *rows);
// The game's own layouts (and Emperor's small tiles, Pericles's editors): no zoom (100) until the next ViewFor.
void ResetView();
// Installed and not switched off (the game layer's own zoom logs).
bool Active();

// The percent and the zoomed buffer size last worked out (100 / 0 when off): the picture's zoomPercent and
// zoomBufferSize.
int Percent();
void BufferSize(int *w, int *h);
// Seeing more: averaged shrinking (else nearest).
bool AverageShrink();
// The largest percent used (the game's cap).
int Cap();
// The wheel's target is not reached yet (the camera's zoomMoving).
bool Moving();

// Zoom off for the rest of the run: the view laid out at 100 right away (before the city is drawn the game's way).
// Smooth scrolling's fallBack calls it (after a fault the game draws straight into the back buffer).
void Off(const char *why);

// The camera's zoom callbacks (core/scroll/camera.h): moving, one step (the scale towards the wheel's target, laid
// out, the view moved so the city point under the cursor stays there), the camera's speed scaled while zoomed.
bool CameraMoving();
void CameraStep();
bool CameraScale(double *fx, double *fy);

// The picture's zoom callbacks (core/scroll/picture.h): the zoomed city drawn into our buffer and scaled onto the
// screen (true: drawn, or skipped this frame; false: not zoomed, the usual drawing), the drag selection's limits as
// buffer pixels while drag_select_draw runs, and its map while it draws.
bool RenderZoomed(picture::RenderCall &c);
void DragDrawZoom(core::diag::Restore &r);
bool DragLinesMap(picture::ZoomMap *m);

// From the game layer's detour of the game's build cost text (drawn after the city, clipped to the city clip): while
// zoomed the city clip is our buffer's (smaller than the screen below 100%: the text was cut off), so for the call
// the clip's right and bottom are the city as shown on the screen. Not zoomed: the game's own. Guarded: a fault
// switches the zoom off and the text is drawn the game's way.
void ShownClipCall(void (*original)());

// From the game layer's detour of the game's save loading, after the game loaded a saved game or a mission: the zoom
// goes back to the starting zoom ([video] city_zoom), laid out at once (a wheel's zoom is not kept
// into the next game).
void OnGameLoaded();

// From the game layer's line_draw detour (x1, y1, x2, y2, colour; the drag selection's frame): while
// drag_select_draw draws zoomed, the frame's buffer pixels drawn on the screen; otherwise the game's own.
void LineDraw(int x1, int y1, int x2, int y2, int colour, void (*original)(int, int, int, int, int));

// For the offline tests: the percent and the buffer size in effect (as ViewFor would set them), our state back to rest
// (on again, nothing installed), and the scaling made to fail.
void SetForTest(int percent, int bufW, int bufH);
void ResetForTest();
void FailScalingForTest(bool fail);

} // namespace core::scroll::zoom
