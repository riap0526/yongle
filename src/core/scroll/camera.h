// The camera of smooth (pixel) scrolling, for the city views of this game family (Emperor and Zeus scroll with the
// same code: the same units, the same step, the same speed formula). Game-independent: the game layer gives its
// variables' addresses and a few callbacks in a CameraGame; nothing here knows a game's addresses.
//
// The game's camera is view_x (columns) / view_y (half rows, kept even); the camera adds a pixel remainder
// (0 <= x < tile_width + 2, 0 <= y < 2 x tile_half_height). At the game's city calls of scroll_get_direction the
// direction is worked out every frame with the game's own rules (the cursor on the screen's edge, the arrow keys),
// view_scrolling is set as the game sets it (so the simulation pauses while scrolling, as in the original) and the
// camera moves by the elapsed time with Augustus's easing (core/scroll/smooth_scroll.h); the game's own step is then
// told "no direction" (8). The cruise speed is Augustus's for each scroll speed setting (camera.cpp StepMs): fixed, the
// same on every computer, in columns a second the same in every game (the original game's own timing
// made 100% far too fast to use once the camera is not held back by frames).
//
// Showing the remainder (drawing the city off the tile grid, converting clicks) is core/scroll/picture's, which reads
// the remainder here; the game layer hooks the functions it needs.
//
// Failure (our features never end the game): a frame of the camera runs under core/diag's guard with the camera,
// view_scrolling and the key flags in its Restore list. On an exception they are put back, the feature (the game
// layer's) is switched off, the remainder goes to 0 and the game's own scrolling runs from then on.
// Game thread only.
#pragma once

#include <cstddef>

#include "core/diag/guard.h"
#include "core/hook/site.h"

namespace core::scroll::camera {

using IntVar = core::site::Var<int>;

// The game's variables the camera reads and writes (every one required).
struct CameraVars {
    CameraVars(IntVar viewX, IntVar viewY, IntVar tileWidth, IntVar tileHalfHeight, IntVar cursorX, IntVar cursorY,
               IntVar desktopWidth, IntVar desktopHeight, IntVar keyLeft, IntVar keyRight, IntVar keyUp,
               IntVar keyDown, IntVar viewScrolling, IntVar scrollSpeed, IntVar frameCount)
        : viewX(viewX), viewY(viewY), tileWidth(tileWidth), tileHalfHeight(tileHalfHeight), cursorX(cursorX),
          cursorY(cursorY), desktopWidth(desktopWidth), desktopHeight(desktopHeight), keyLeft(keyLeft),
          keyRight(keyRight), keyUp(keyUp), keyDown(keyDown), viewScrolling(viewScrolling), scrollSpeed(scrollSpeed),
          frameCount(frameCount)
    {
    }

    IntVar viewX, viewY;            // the camera: columns, half rows (even)
    IntVar tileWidth;               // one column is tileWidth + 2 px
    IntVar tileHalfHeight;          // one half row
    IntVar cursorX, cursorY;        // the raw cursor on the screen (edge scrolling)
    IntVar desktopWidth, desktopHeight; // the screen's size the edges are tested against
    IntVar keyLeft, keyRight, keyUp, keyDown; // set by the game's window procedure on an arrow key
    IntVar viewScrolling;           // the game's "scrolling now" (the simulation waits while it is set)
    IntVar scrollSpeed;             // the game's scroll speed setting, percent (10..100; 0 does not scroll)
    IntVar frameCount;              // +1 on every game frame (the camera runs once per frame)
};

// What the camera needs from the game (a constant of the game layer). No callback may be nullptr: a game without
// the thing gives a function that says so, with the reason in a comment.
struct CameraGame {
    CameraGame(const char *log, core::diag::Feature *feature, const CameraVars &vars, void (*clampView)(),
               void (*soundStep)(), bool (*keyHeld)(unsigned vk), void (*fallBack)(), bool (*zoomMoving)(),
               void (*zoomFrame)(), bool (*zoomScale)(double *fx, double *fy))
        : log(log), feature(feature), vars(vars), clampView(clampView), soundStep(soundStep), keyHeld(keyHeld),
          fallBack(fallBack), zoomMoving(zoomMoving), zoomFrame(zoomFrame), zoomScale(zoomScale)
    {
    }

    const char *log;              // the prefix of the log lines that say which game, e.g. "zeus"
    core::diag::Feature *feature; // the game layer's (its picture half uses the same one); off after a fault
    CameraVars vars;
    void (*clampView)();          // the game's view_clamp (__cdecl, no arguments): keeps view_x / view_y in the map
    void (*soundStep)();          // the game's sound_view_decay, called once per tile's width scrolled
    bool (*keyHeld)(unsigned vk); // the key (or a key bound to that arrow) is held now
    void (*fallBack)();           // after a fault: the game layer resets its own state (the remainder is 0 already)
    bool (*zoomMoving)();         // the city's zoom is changing (zoomFrame then runs every frame, smooth or not)
    void (*zoomFrame)();          // one step of the zoom, before the scrolling of the frame
    bool (*zoomScale)(double *fx, double *fy); // zoomed: the game's pixels per screen pixel (the speed is scaled)
};

// From the game layer's Attach, once its hooks are installed: the setting (gameplay.smooth_scroll) and the log line.
void Start(const CameraGame &g, bool smooth);

// The setting now (the game layer reads it, e.g. its zoom step).
bool Smooth();

// The setting changed while running (game thread; the game is paused by our panel then). Off: the view snaps to the
// whole tile it is on. False when it cannot be switched on: the feature was switched off after an error.
bool SetSmooth(const CameraGame &g, bool smooth);

// From the game's scroll_get_direction detour, at the game's city call sites only (the game layer checks the return
// address): true when the camera has moved for this frame (the detour returns 8, "no direction", so the game's own
// step does nothing); false: the detour calls the game's own. Once per game frame; guarded.
bool CityDirection(const CameraGame &g);

// Drag scrolling (core/scroll/drag, which installs these): take is asked once per camera frame, with or without smooth
// scrolling, for the movement of a drag (game pixels; whole tiles when smooth is off) and returns true while dragging
// (the edges and the arrow keys do not scroll then, and the game's own step is told "no direction"); loop runs from
// OnLoop. Both nullptr: no drag scrolling (the offline tests of the camera alone).
using DragTake = bool (*)(const CameraGame &g, bool smooth, int *dx, int *dy);
using DragLoop = void (*)();
void SetDrag(DragTake take, DragLoop loop);

// Edge scrolling: the cursor this many pixels (the game's, as the game reads the cursor) or less from the screen's
// edge scrolls (gameplay.edge_scroll_width; with smooth scrolling off through GameDirection). 0: the edges do not
// scroll at all (the arrow keys and a drag still do).
void SetEdgeWidth(int px);

// The game's own direction at a city call site (CityDirection said false: smooth scrolling off, or switched off after
// an error): runs call (the game's scroll_get_direction) with the game's cursor seen through the edge width -- within
// it counts as on the edge, and with width 0 the cursor is kept a pixel off the edges -- then puts the cursor back, so
// the game's whole-tile scrolling follows gameplay.edge_scroll_width too. Width 1 changes nothing (the game's own).
int GameDirection(const CameraGame &g, int (*call)());

// From the game's view_scroll_step detour (both modes): counts the game's own steps for the rate log.
void CountStep(const CameraGame &g, int dir);

// Once per game frame (or main-loop turn): the milliseconds since the previous one (the time spent scrolling).
void OnLoop(const CameraGame &g, double ms);

// After a pause (our panel was open): the speed starts again from rest.
void Resume();

// The scroll statistics since the last call, for the rate log ("" when the view did not scroll): "; scrolling ... ms
// at scroll speed ...%: x ... px/s, y ... px/s, ... steps, ... tile changes (expected ... px/s per axis)".
void TakeStats(const CameraGame &g, char *out, std::size_t size);

// The feature was switched off by a guard (the camera's or the game layer's picture half): remainder 0, the speed
// from rest, logged once, then g.fallBack().
void FallBack(const CameraGame &g);

// Someone else moved the camera (minimap click, "go to", rotation, loading a game): start again on a whole tile.
void SyncView(const CameraGame &g);

// Moves the camera by (dx, dy) pixels: whole columns and row pairs into view_x / view_y, clamped by the game (an edge
// stops that axis on a whole tile).
void Move(const CameraGame &g, int dx, int dy);

// The zoom's step: the view put back where it was before the step's layout (the
// layout's own clamp may have moved it already), then moved by (dx, dy) and clamped once, as Move does -- also with no
// movement. One clamp, so the east and south edges stay pinned as the west and north ones do.
void MoveFrom(const CameraGame &g, int vx, int vy, int dx, int dy);

// The view as it is now is the camera's (not "someone else", SyncView), e.g. after the game layer's zoom step.
void NoteView(const CameraGame &g);

// The east and south edges of the map. The game's clamp works in whole
// columns and row pairs: at its east (south) limit the original's city, a whole number of them, ends on the original's
// east (south) edge -- the map's jag lies beyond it. Ours is not a whole number of them (the gap left of the sidebar at
// 100%, the zoomed city's size), so the camera's limit there is a line instead: the game's limit + px
// (core/scroll/picture works it out as the original's edge less where our city ends; negative: stop before the game's
// limit, positive: go past it with a remainder), as Akhenaten's camera limit is a fixed line less the shown size. West
// and north need nothing: our city starts where the original's does.
// SetEdgeOffsets: from the picture before each frame's drawing and from the zoom's step after its layout (layout: a
// key of the view's size -- the game's limits depend on it). Move keeps the camera at most on the lines (a zoom step
// that shrinks the city at the east edge keeps it pinned there, as the west edge is). KeepEdges: from the picture
// before the game draws (also with smooth scrolling off): SyncView first; a view past a line goes back to it, and a
// view at the game's limit after someone else moved it there (a minimap click, a game loaded, the game's own
// whole-tile step) is put on the line. The view and the remainders it changes go into r (put back if the guarded call
// faults; kept otherwise). True when it moved the view. SnapEdges: the zoom's whole-tile step (smooth scrolling off):
// a view at the game's limit put on the line.
// halfRows: the view's half rows, which the game's clamp reads (camera.cpp Edges asks the clamp once more with one
// less, to find the half row its even south limit leaves out); nullptr: not asked.
void SetEdgeOffsets(int east, int south, int layout, volatile int *halfRows = nullptr);
bool KeepEdges(const CameraGame &g, core::diag::Restore &r);
void SnapEdges(const CameraGame &g);

// The pixel remainder.
void Remainder(int *px, int *py);
// Remainder 0 (whole tiles).
void ClearRemainder();
// Remainder 0 into r (put back when r is undone).
void ZeroRemainder(core::diag::Restore &r);

// For the offline tests: a remainder as if the camera had moved there (the view as it is now is the camera's).
void SetRemainderForTest(const CameraGame &g, int px, int py);

// The game's call sites: the return address is one of calls[0..n) (as the game's own addresses).
bool IsCall(const void *ret, const DWORD *calls, int n);
// At Attach: each of calls[0..n) follows a "call target" (e8 rel32). False (logged with prefix) otherwise.
bool CallsTarget(const char *log, const DWORD *calls, int n, const core::site::Site &target);

} // namespace core::scroll::camera
