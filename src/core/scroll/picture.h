// The picture of smooth (pixel) scrolling, for the city views of this game family: the city drawn with the camera's
// pixel remainder (core/scroll/camera.h), and the positions the game converts (clicks, the cursor tile, sounds, the
// drag selection) moved the same way. Emperor and Zeus draw and convert with the same code;
// the game layer gives its variables' addresses and a few callbacks in a
// PictureGame, and keeps the hooks (the detours call the functions below with the game's original as a thunk).
// Nothing here knows a game's addresses.
//
// Drawing: the games' tile blitters only handle a tile cut exactly in half at a clip edge; the city clip
// (viewport_init: left/top = origin, right/bottom = origin + whole tiles) is always on that grid, so they never see
// anything else. Moving the origin by an arbitrary number of pixels breaks that (Yongle's first smooth scrolling
// build wrote past the back buffer and crashed). So city_render draws exactly as the game expects -- origin and clip on
// the tile grid -- but into our own buffer with the clip one column and one row pair larger on the right and bottom
// (more where the extent needs it), and the view rectangle is then copied to the back buffer offset by the remainder
// (px, py). Everything city_render draws (tiles, figures, markers, the construction preview) moves together. The
// games' tile loops reach past the clip (Emperor columns -4 .. +6, rows -8 .. +21; Zeus columns -6 .. +8, rows -8 ..
// +21); their marker loops cover columns 0 .. width and rows 0 .. height - 1 only, so while drawing the viewport is
// also made one column wider and two half rows taller.
// Coordinate conversion outside that drawing (clicks, the cursor tile, sounds, floating text): the origin is moved by
// -(px, py) for the duration of the call and restored when it returns. Screen-to-grid calls made while city_render
// draws are converted the same way (their input is a screen position); grid_to_screen calls made while it draws are
// not (their output is a position in our buffer).
//
// The extent (SetCityExtent): where the city is seen to beyond the game's clip, which stays on the tile grid (the
// gap left of a sidebar against the screen's right edge, the rows down to the screen's bottom). With a city clip that
// already reaches past the screen's bottom (Zeus's layout, into the back buffer's spare rows) only the rows on the
// screen are shown.
//
// The city's zoom: the PictureGame's callbacks (zoomPercent, zoomBufferSize, renderZoomed, ...), which both games
// fill with the core's zoom (core/scroll/zoom.h); a game without a zoom would say so with callbacks that return
// "100%" / "no".
//
// Failure (our features never end the game): every call that changes game state for us runs under core/diag's guard
// with the changes in a Restore list (moved origin, larger viewport and clip, our buffer as draw_buffer). On an
// exception the list is put back, smooth scrolling is switched off for the rest of the run (camera::FallBack: the
// remainder goes to 0, then the game layer's fallBack, which calls Reset here) and the call is made again the game's
// own way; from then on every function passes the calls through, as with gameplay.smooth_scroll = off. Calls that
// change nothing (no remainder, no extent) go to the game outside the guard: a fault there is the game's own.
// Game thread only.
#pragma once

#include "core/diag/guard.h"
#include "core/hook/site.h"
#include "core/scroll/camera.h"

#include <cstddef>

namespace core::scroll::picture {

using IntVar = core::site::Var<int>;

// The game's variables the picture reads and writes (every one required).
struct PictureVars {
    PictureVars(IntVar originX, IntVar originY, IntVar widthTiles, IntVar heightRows, IntVar clipLeft, IntVar clipTop,
                IntVar clipRight, IntVar clipBottom, core::site::Var<BYTE *> drawBuffer, IntVar mouseX, IntVar mouseY,
                IntVar screenPitch, IntVar ddWidth, IntVar ddHeight, core::site::Var<unsigned char> mouseMode,
                IntVar cursorTile, core::site::Var<signed char> tileHeights)
        : originX(originX), originY(originY), widthTiles(widthTiles), heightRows(heightRows), clipLeft(clipLeft),
          clipTop(clipTop), clipRight(clipRight), clipBottom(clipBottom), drawBuffer(drawBuffer), mouseX(mouseX),
          mouseY(mouseY), screenPitch(screenPitch), ddWidth(ddWidth), ddHeight(ddHeight), mouseMode(mouseMode),
          cursorTile(cursorTile), tileHeights(tileHeights)
    {
    }

    IntVar originX, originY;         // the city viewport's origin (viewport_init)
    IntVar widthTiles, heightRows;   // the viewport's columns and half rows
    IntVar clipLeft, clipTop;        // the city clip (viewport_init)
    IntVar clipRight, clipBottom;    // exclusive
    core::site::Var<BYTE *> drawBuffer; // the back buffer the game draws into
    IntVar mouseX, mouseY;           // the game's mouse (mouse_to_grid reads these)
    IntVar screenPitch;              // bytes per row of the back buffer
    IntVar ddWidth, ddHeight;        // the screen's size
    core::site::Var<unsigned char> mouseMode; // the drag selection's mode while a military drag lasts
    IntVar cursorTile;               // the cursor's tile index
    core::site::Var<signed char> tileHeights; // the ground's height of each tile (by tile index)
};

// The city's zoom as the conversions see it (core/scroll/zoom.h).
struct ZoomMap {
    int left, top;             // where the city starts (screen and buffer)
    int showRight, showBottom; // the city on the screen (exclusive)
    double fx, fy;             // buffer pixels per screen pixel
};

// One city_render call: the arguments, and the changes made for it (put back after the draw or when it is abandoned).
struct RenderCall {
    int vx, vy, orientation;
    bool plain; // nothing to move: the game draws as it always does (outside the guard)
    core::diag::Restore r;
};

// What the picture needs from the game (a constant of the game layer). No callback may be nullptr: a game without
// the thing gives a function that says so, with the reason in a comment.
struct PictureGame {
    PictureGame(const char *log, const camera::CameraGame *camera, const PictureVars &vars,
                unsigned char mouseModeDragSelect, int heightPx, int mapTiles, DWORD dragStartCorner,
                DWORD dragDrawCorner, void (*drawCity)(int, int, int), void (*drawCityAgain)(int, int, int),
                int (*zoomPercent)(), void (*zoomBufferSize)(int *w, int *h), bool (*renderZoomed)(RenderCall &c),
                void (*dragDrawZoom)(core::diag::Restore &r), bool (*dragLinesMap)(ZoomMap *m))
        : log(log), camera(camera), vars(vars), mouseModeDragSelect(mouseModeDragSelect), heightPx(heightPx),
          mapTiles(mapTiles), dragStartCorner(dragStartCorner), dragDrawCorner(dragDrawCorner), drawCity(drawCity),
          drawCityAgain(drawCityAgain), zoomPercent(zoomPercent), zoomBufferSize(zoomBufferSize),
          renderZoomed(renderZoomed), dragDrawZoom(dragDrawZoom), dragLinesMap(dragLinesMap)
    {
    }

    const char *log;                  // the prefix of the log lines that say which game, e.g. "zeus"
    const camera::CameraGame *camera; // the camera (its feature is the picture's too; the remainder, view_x)
    PictureVars vars;
    unsigned char mouseModeDragSelect; // mouseMode's value during a military drag selection
    int heightPx;                     // pixels per level of the ground's height (the drag corner)
    int mapTiles;                     // entries of tileHeights
    DWORD dragStartCorner;            // return addresses of grid_to_screen's calls for the drag frame's corner in
    DWORD dragDrawCorner;             // drag_select_start and drag_select_draw (the game's own addresses)
    void (*drawCity)(int, int, int);  // the game's city_render, as the game layer draws the city
    void (*drawCityAgain)(int, int, int); // the same, once more after a drawing was abandoned (a fault in it)
    int (*zoomPercent)();             // the city's zoom now (100: none)
    void (*zoomBufferSize)(int *w, int *h); // the zoomed view's size in the game's pixels
    bool (*renderZoomed)(RenderCall &c); // true: the zoomed city was drawn (or skipped) into c; false: the usual drawing
    void (*dragDrawZoom)(core::diag::Restore &r); // drag_select_draw runs (zoomed: its limits as buffer pixels into r)
    bool (*dragLinesMap)(ZoomMap *m); // true while drag_select_draw draws zoomed, with its map
};

// The city as shown on the screen (right and bottom exclusive).
struct Shown {
    int left, top, right, bottom;
};

// ---------------------------------------------------------------------------------------------
// From the game layer's detours (each takes the game's original function as a thunk)

// city_render: drawn with the remainder and the extent (see the top of this file), then the city drawn callback
// (SetCityDrawn) and the after-city callback (SetAfterCity).
void CityRender(const PictureGame &g, int vx, int vy, int orientation);

// screen_to_grid(x, y, unused): a point outside the city as shown is no tile (0, without calling the game).
int ScreenToGrid(const PictureGame &g, int x, int y, int unused, int (*original)(int x, int y, int unused));
// mouse_to_grid: during a drag selection the mouse is kept to the city as shown. unused: Emperor's caller pushes an
// argument the function does not read (passed on as it came); Zeus's has none (0, its thunk drops it).
int MouseToGrid(const PictureGame &g, int unused, int (*original)(int unused));
// screen_to_grid_unbounded(x, y, unused): the drag selection's.
int ScreenToGridRaw(const PictureGame &g, int x, int y, int unused, int (*original)(int x, int y, int unused));
// grid_to_screen: from = the detour's return address as the game's own (the drag frame's corners).
void GridToScreen(const PictureGame &g, DWORD from, int gx, int gy, int *sx, int *sy,
                  void (*original)(int gx, int gy, int *sx, int *sy));
// city_view_width_px (the "Game paused" banner): the city as shown, with an extent.
int CityWidth(const PictureGame &g, int (*original)());
// drag_select_start / drag_select_draw / drag_select_finish (drag: the game's drag structure, the functions' this).
void DragStart(const PictureGame &g, int *drag, void (*original)(int *drag));
void DragAdjusted(const PictureGame &g, int *drag, void (*original)(int *drag), bool draw);

// The camera's east and south lines (camera.h SetEdgeOffsets) worked out for the layout as it is now: before each
// drawing, and from the zoom's step after its layout and before it moves the camera.
void UpdateEdges(const PictureGame &g);

// ---------------------------------------------------------------------------------------------
// From the game layer

// Where the city should be seen to, beyond the game's clip: right = the sidebar's left edge (or the screen's right
// edge without a sidebar), bottom = the screen's bottom; 0 = the clip (the game's own layouts). Set by the layout
// after each viewport and sidebar position.
void SetCityExtent(int right, int bottom);
void Extent(int *right, int *bottom);

// After the feature was switched off by a guard: our state back to rest (the game layer's camera fallBack calls it).
void Reset();

// The city as shown right after it was drawn: the clip's top left to the extent (or the clip without one), never past
// the screen. False when that is not inside the back buffer (why: the reason).
bool DrawnCity(const PictureGame &g, int *left, int *top, int *right, int *bottom, const char **why);

// Called right after the game drew the city (before the interface), with the back buffer and DrawnCity's rectangle.
// Game thread; the callback runs its own guard. One callback (Emperor's menus' background; Zeus's reads DrawnCity).
using CityDrawn = void (*)(const BYTE *back, int pitch, int left, int top, int right, int bottom);
void SetCityDrawn(CityDrawn fn);

// Called at the end of every city_render (after the city was drawn and shown, before the interface), with the
// drawing's direction argument. One callback (Emperor's city picture).
using AfterCity = void (*)(int orientation);
void SetAfterCity(AfterCity fn);

// A drawing into a buffer of right x bottom pixels at 100% without the pixel remainder (Emperor's city picture,
// Zeus's figure portraits): the conversions made while it draws are not moved. The changes go into r (the caller
// undoes it after the drawing, also when it is abandoned).
void PictureDrawing(core::diag::Restore &r, int right, int bottom);
// The game's city_render as the picture draws it (timed as "city drawn by the game").
void DrawCity(const PictureGame &g, int vx, int vy, int orientation);

// The screen position is on the city as shown (not the sidebar, not the top bar).
bool OnCity(const PictureGame &g, int x, int y);

// For a problem report (game thread): saves our city buffer as last drawn (the city with the pixel offset still in
// it) as report<n>_<screen>_city_..., or logs that it has not been used yet.
void SaveCityBuffer(int report);

// The log line of an Attach that installed the picture (prefix: the game's).
void LogInstalled(const PictureGame &g);

// ---------------------------------------------------------------------------------------------
// For the zoom (core/scroll/zoom: RenderZoomed, CameraStep, LineDraw)

// The zoom now (false at 100%, or without an extent): outside the drawing from the screen's sizes, while city_render
// draws the drawing's own.
bool ZoomNow(const PictureGame &g, ZoomMap *m);
bool Zoomed(const PictureGame &g);
// A conversion would see a moved origin (or a zoomed mouse) now.
bool Shifts(const PictureGame &g);
// Our city buffer, at least size bytes (nullptr: no memory).
BYTE *Buffer(std::size_t size);
// +1 each time the usual (100%) drawing used the buffer: the zoom's layout is not in it any more.
int BufferUses();
// The buffer's layout as last drawn (for a report).
void NoteBuffer(int pitch, int rows);
// While the zoomed drawing draws: the city as shown on the screen and the drawing's zoom (fx 0: none).
void SetRenderShown(const Shown &s);
void SetRenderZoom(const ZoomMap &m);
// The flag "city_render draws into our buffer" (for r.Set).
bool *InRenderFlag();
// The drag selection works in buffer pixels.
bool BufferMode();
// The camera's pixel remainder.
int RemX();
int RemY();

} // namespace core::scroll::picture
