// Drag scrolling with the middle mouse button (both games). Game-independent:
// the game layer gives its camera, its picture and its city screens in a DragGame.
//
// Over the city as shown (picture::OnCity), on the game layer's city screens (Pericles: also its editors' city), a
// middle press waits: moved more than kThreshold screen pixels while held, it becomes a drag -- the cursor is hidden
// and put back where it was pressed every frame, and the camera moves by what the mouse moved, the other way (the map
// follows the mouse: "grab the map"); in game pixels, through the output's scale and the city's zoom, so the map
// moves on the screen as far as the mouse did. With smooth scrolling off the view moves in whole tiles only. Released
// without moving: a click, which goes to the zoom (back to 100%, where the zoom has it). While dragging the game sees
// no mouse message (the left and right buttons swallowed with their releases; the moves too, or the building preview
// would shake as the cursor is put back), and the edges and the arrow keys do not scroll.
//
// Also the edge scrolling's width (gameplay.edge_scroll_width), given to the camera every frame.
//
// A drag ends (the cursor shown again, where it was pressed) on the release, when the window loses the focus, when the
// game leaves a city screen, when our panel or options window opens (the game layer's pause calls End), when the
// setting is switched off, and on an error (guarded; the drag is switched off and logged, the middle click still goes
// to the zoom). Game thread only.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/scroll/camera.h"

namespace core::scroll::drag {

// Moved more than this many screen pixels on either axis while held: a drag (Augustus's SCROLL_DRAG_MIN_DELTA).
constexpr int kThreshold = 4;

// What the drag needs from the game (a constant of the game layer). No callback may be nullptr.
struct DragGame {
    DragGame(const char *log, bool (*cityScreen)(), bool (*onCity)(int x, int y), bool (*dragSelecting)(),
             bool (*drawingWorks)(), bool (*otherMouse)(UINT message, WPARAM wparam, int x, int y))
        : log(log), cityScreen(cityScreen), onCity(onCity), dragSelecting(dragSelecting), drawingWorks(drawingWorks),
          otherMouse(otherMouse)
    {
    }

    const char *log;        // the prefix of the log lines, e.g. "zeus"
    bool (*cityScreen)();   // a screen where the city can be dragged (the game's city screens, the editors' too)
    bool (*onCity)(int x, int y); // the point (the game's pixels) is on the city as shown (picture::OnCity)
    bool (*dragSelecting)();      // a military drag selection is going on (its frame is the left button's)
    bool (*drawingWorks)(); // smooth scrolling is installed and working (its camera moves the view)
    bool (*otherMouse)(UINT message, WPARAM wparam, int x, int y); // the mouse filter after ours (the zoom's)
};

// The system's side (Win32 and the virtual display), so the offline test can give a fake one. No member may be
// nullptr.
struct DragSystem {
    bool (*cursorPos)(POINT *screen);   // the real cursor, in screen pixels
    void (*setCursorPos)(POINT screen); // puts it there
    void (*hideCursor)(bool hidden);    // hidden: no cursor shown, the game's own SetCursor not applied
    void (*capture)(bool on);           // the game window keeps the mouse (SetCapture / ReleaseCapture)
    bool (*middleHeld)();               // the middle button is down now
    bool (*focused)();                  // the game window is the foreground one
    double (*screenPerGamePixel)();     // the output's scale (screen pixels per game pixel), 1 when unknown
    bool (*enabled)();                  // gameplay.drag_scroll
    int (*edgeWidth)();                 // gameplay.edge_scroll_width
    void (*setMouseFilter)(bool (*filter)(UINT message, WPARAM wparam, int x, int y)); // core::display's one filter
};

// The real system (drag_system.cpp: Win32 and core::display).
const DragSystem &RealSystem();

// From the game layer, after its zoom is installed: the mouse filter becomes ours (passing everything we do not take
// to g.otherMouse) and the camera asks us for the drag every frame.
void Install(const DragGame &g, const DragSystem &s);

// The mouse filter (core::display::SetMouseFilter): true when the message is ours (kept from the game).
bool MouseFilter(UINT message, WPARAM wparam, int x, int y);

// Ends a press or a drag (the cursor shown and put back); why goes to the log. Nothing when there is none.
void End(const char *why);

// True while dragging (moved past the threshold).
bool Dragging();

// For the offline test: the state back to the start (no drag, the feature on again is not possible: a new process).
void ResetForTest();

} // namespace core::scroll::drag
