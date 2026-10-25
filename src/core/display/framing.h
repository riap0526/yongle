// Geometry of a framed full-screen picture (docs/resolution_ui/fullscreen_art.md §7.5, R2a): the picture plus a
// border of one tile row on all four sides, at one scale, as large as the screen allows and centred; background
// everywhere else. If the background left outside the border on an axis would be thinner than twice the border per
// side, the border goes to the screen edge on that axis instead and the extra space becomes padding inside the
// border (equal on both sides, an even number of units, so the picture sits exactly in the middle).
//
// Units are the picture's own pixels (the border is kTile units thick). The same function serves the background's
// composition (in units), the output (screen pixels) and the mouse mapping, so the three always agree. It is the
// layout() of tools/mockups/fsmock3.py, which produced the approved v4 mockups.
//
// Two layers are drawn from it (each resampled once):
//   background: the outer fill (outerW x outerH units, drawn at (0,0) scaled by s and cropped by the screen) and the
//               frame (frameU x frameV units: border + padding, drawn into frame*), both composed by the game layer;
//   picture:    the picture's own pixels, drawn straight into picture* (screen pixels, fractional).
// Nothing here knows a game.
#pragma once

namespace core::display {

constexpr int kTile = 16;           // border thickness in units
constexpr double kSnapBorders = 2.0; // snap to the screen edge below this many border widths of outside fill

struct FrameLayout {
    double s;                  // scale: screen pixels per unit (before the frame's own rounding)
    int frameX, frameY;        // the frame (border + padding + picture) in screen pixels
    int frameW, frameH;
    int frameU, frameV;        // the frame's size in units (whole tiles at its edges; frameW / frameU ~ s)
    int padU, padV;            // padding between border and picture, per side, in units
    bool snapX, snapY;         // the border is at the screen edge on this axis
    double picX, picY;         // the picture in screen pixels (one resample from its own pixels)
    double picW, picH;
    int outerU, outerV;        // the outer fill's size in units, drawn at (0,0) scaled by s
    bool native;               // laid out at the picture's own size (s = 1)
};

// screenW x screenH: the output in pixels; pictureW x pictureH: the picture in units. False when either is empty.
// native (R2d.1): scale 1 -- the picture's own size, centred, the same border and snapping rule -- when the picture
// and its border fit on the screen; otherwise (and without it) as large as the screen allows.
bool ComputeFrameLayout(int screenW, int screenH, int pictureW, int pictureH, FrameLayout *out, bool native = false);

// Tile positions along a side of `size` units: the first at 0, the last flush with the end, the middle ones every
// kTile (as the game's dialog panels place them). Writes at most max positions; returns the count.
int FrameTilePositions(int size, int *out, int max);

} // namespace core::display
