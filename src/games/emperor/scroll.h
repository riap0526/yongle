// Pixel (smooth) scrolling of the city view. The game itself scrolls in whole tiles.
#pragma once

#include <cstddef>

#include "core/diag/guard.h"
#include "core/scroll/picture.h"
#include "games/emperor/site.h"

namespace emperor::scroll {

// Checks the sites and installs the hooks. Returns false (nothing installed) if a site does not match. The hooks are
// installed either way; with smooth off (gameplay.smooth_scroll) the direction hook hands the game its own tile
// steps and the pixel remainder stays 0, which makes every other hook pass the calls through unchanged.
bool Attach(bool smooth);

// Drag scrolling (core/scroll/drag): the middle button drags the city. After the zoom's Attach (the drag's mouse filter
// passes the rest on to the zoom's). Only with Attach's hooks installed.
void AttachDrag();

// The setting changed while running (game thread; the game is paused by our panel then). Off: the view snaps to the
// whole tile it is on. False when it cannot be switched on: the feature was switched off after an error.
bool SetSmooth(bool smooth);

// From the loop-step hook, once per main-loop iteration: elapsed milliseconds since the previous one.
void OnLoop(double frameMs);

// After a pause (our panel was open): the scroll speed starts again from rest instead of from the time before it.
void Resume();

// Scroll statistics since the last call, formatted for the rate log ("" when the view did not scroll).
void TakeStats(char *out, size_t size);

// For a problem report (core/diag/report.h; game thread): saves our city buffer as last drawn (the city with the
// pixel offset still in it) as report<n>_<screen>_city_..., or logs that it has not been used yet.
void SaveCityBuffer(int report);

// Where the city should be seen to, beyond the game's own clip,
// which stays on the tile grid (viewport_init; the city message panel 0x50bf10 is placed from it, and the game's
// tile drawing needs it on the grid): right = the sidebar's left edge, bottom = the screen's bottom; 0 = the clip
// (the game's own layouts). Set by the layout (games/emperor/resolution) after each viewport. While the city is
// drawn the clip is made larger on the grid and only the part up to (right, bottom) is shown: through our city buffer
// when there is a gap on the right or a pixel offset, else (bottom only) straight into the back buffer, whose spare
// rows (core/ddraw/surface_memory.h) take the rows below the screen. Only with the emulated DirectDraw.
void SetCityExtent(int right, int bottom);

// False once smooth scrolling was switched off after an error (core/diag guard).
bool Working();

// The screen position is on the city as shown (not the sidebar, not the top bar).
bool OnCity(int x, int y);

// The picture's view of Emperor, for the core's zoom (games/emperor/zoom: core/scroll/zoom.h ZoomGame).
const core::scroll::picture::PictureGame &Picture();

// Called right after the game drew the city (before the interface), with the back buffer and the city's
// rectangle on it (right and bottom exclusive). Game thread; the callback runs its own guard.
using CityDrawn = void (*)(const BYTE *back, int pitch, int left, int top, int right, int bottom);
void SetCityDrawn(CityDrawn fn);

// City picture (core/capture/city_picture, which sets the after-city callback and the picture's drawing state with
// core/scroll/picture itself): one strip drawn with the game's own city_render (timed as the picture's).
void DrawCityForPicture(int vx, int vy, int orientation);

// For the offline tests (src/games/emperor/test_restore.cpp): the detours by site, and the pixel remainder.
const Detour *Detours(int *count);
void SetRemainderForTest(int px, int py);
void Remainder(int *px, int *py);

} // namespace emperor::scroll
