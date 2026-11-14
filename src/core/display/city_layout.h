// Parts of the city screen's layout at any resolution that both games share (moved from
// games/emperor/resolution.cpp when Zeus needed them). Nothing here knows a
// game: the game layer gives its clip, its tile height, its images and its drawing functions.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace core::display {

// The spare rows below the screen the city may draw into (core/ddraw/surface_memory: kSpareRows, the canary): the
// clip's overshoot of the screen (clipBottom > screenH: the game's own clip, or a viewport with the next half row
// past the bottom), or, when the city is shown down to shownBottom below the clip, the next half row past it (a
// city drawn by our smooth scrolling). Never more than two half rows (anything else is not a clip of this mode).
// The city's clip is exclusive at the bottom in both games' image clipping, so the last row written is clipBottom - 1.
int SpareRowsAllowance(int clipBottom, int shownBottom, int halfH, int screenH);

// SpareRowsAllowance applied to the canary (core::ddraw::SetCanaryAllowance) and logged when it changes:
// "<prefix>the city may draw N row(s) below the screen ...".
void ApplySpareRows(const char *prefix, int clipBottom, int shownBottom, int halfH, int screenH);

// Tiles an image of width imageW along a row from x = right - imageW down to the first x <= left - imageW
// (right-aligned: the games' 800 branches align the top bar to the sidebar), calling draw(x, ctx) for each.
void TileRowLeft(int imageW, int left, int right, void (*draw)(int x, void *ctx), void *ctx);

// ---------------------------------------------------------------------------------------------
// The sidebar's filler (used by Zeus; drawn into the game's 16-bit back buffer). Every function stays inside the surface
// (core/display/moved_rect's Surface16) and does nothing for an empty or inconsistent one.

struct Surface16; // core/display/moved_rect.h

// An inset panel of any pixel size from the 16 px tiles of a 7 x 7 set (the games' inner panels: row 0 the top
// edge, rows 1..5 five middle rows, row 6 the bottom edge; in each row column 0 the left edge, 1..5 five middles,
// 6 the right edge; tile k = row x 7 + column). The panel's last column and row are put at right - 16 and
// bottom - 16 (overlapping the one before), so any size >= 32 x 32 fills [left, right) x [top, bottom) exactly; the
// interior comes first and the edges and corners after it. draw(k, x, y, ctx) is called for each tile in that order.
// False (nothing drawn) when the panel is smaller than 32 x 32.
bool InsetPanel(const RECT &area, void (*draw)(int k, int x, int y, void *ctx), void *ctx);

// Fills `area` with an RGB565 picture (w x h) tiled with every other tile mirrored (left-right in odd columns of
// tiles, top-bottom in odd rows), so that neighbouring tiles meet on the same pixels: no hard seams.
void MirrorTile(const Surface16 &s, const WORD *px, int w, int h, const RECT &area);

// Copies the rectangle `from` of the surface to (toX, toY) (overlapping copies are fine).
void CopyRect(const Surface16 &s, const RECT &from, int toX, int toY);

// Repeats the rows [band.top, band.bottom) of columns [band.left, band.right) of the surface down `area`'s rows
// (area's columns are band's), from area.top on.
void RepeatRows(const Surface16 &s, const RECT &band, int top, int bottom);

} // namespace core::display
