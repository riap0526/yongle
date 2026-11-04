// A picture of RGB565 pixels shown larger somewhere else (R2c: the China map, games/emperor/china_map.cpp), and the
// blurred, darkened copy of it used as the background around it (as Augustus does for its empire map). Nothing here
// knows a game.
//
// Scaling is "sharp bilinear" (each source pixel a flat square, blended only across its edges, the blend one output
// pixel wide), or nearest neighbour for whole-number scales. Every write stays inside the destination's clip.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace core::display {

struct Pixels16 {
    const WORD *px; // first row
    int stride;     // pixels per row
    int w, h;
};

struct Target16 {
    WORD *px; // first row
    int stride; // pixels per row
    int w, h;
};

// How the output rows of a scaling are worked through (R5: core/platform/parallel). A ForRows calls fn(ctx, begin,
// end) for pieces that together cover [0, count) once each -- in any order, possibly on several threads at the same
// time -- and returns when all are done. nullptr: fn(ctx, 0, count) on the calling thread. Every output row is
// written by exactly one piece and depends only on the source, so the result is the same either way.
using RowsFn = void (*)(void *ctx, int begin, int end);
using ForRows = void (*)(int count, RowsFn fn, void *ctx);

// Scales src by `scale` (>= 1) to (x, y) of dst, size round(src.w * scale) x round(src.h * scale), writing only inside
// clip (which is cut to dst). sharp: sharp bilinear; otherwise nearest (for whole-number scales: exact copies).
// False (nothing written) when the arguments make no sense.
bool ScaleRgb565(const Pixels16 &src, double scale, bool sharp, const Target16 &dst, int x, int y, const RECT &clip,
                 ForRows forRows = nullptr);

// R5: src shrunk to outW x outH (outW <= src.w, outH <= src.h) at dst's first row and column. average: each output
// pixel the area-weighted average of exactly the source area it covers (src.w / outW x src.h / outH pixels; a pixel
// cut by its edge counts by the part covered, in 32nds), so details do not blink as the picture moves by a pixel;
// otherwise nearest (the pixel in the middle of each whole-pixel block). False (nothing written) when the arguments
// make no sense.
bool ShrinkRgb565(const Pixels16 &src, int outW, int outH, bool average, const Target16 &dst,
                  ForRows forRows = nullptr);

// The source's inverse of ScaleRgb565: the source pixel under destination pixel (dx, dy) of a picture drawn at
// (x, y) by `scale`. False when it is outside the picture.
bool ScaledToSource(int dx, int dy, int x, int y, double scale, int srcW, int srcH, int *sx, int *sy);

// out := src scaled to cover w x h (centred, cropped, bilinear), blurred by about `sigma` output pixels (three box
// passes each way; not blurred when sigma <= 0), multiplied by brightness / 256 and reduced to RGB565 with 4 x 4 ordered dithering (dark, smooth
// gradients would otherwise show bands). Full resolution: meant to be computed once per picture and size.
void BlurredBackdrop(const Pixels16 &src, int w, int h, double sigma, int brightness, std::vector<WORD> *out);

// O3: sharp bilinear in two passes for a renderer without it (Direct3D 9): the srcW x srcH picture drawn scaled by
// (scaleX, scaleY) is first enlarged k times with nearest into a render target, then scaled to its place with linear,
// so each output pixel blends at most two neighbouring source pixels. Returns k (>= 2, the smallest whole number at
// least as large as the scale, lowered while k srcW or k srcH exceeds maxTexture); 1 when no second pass is needed
// (the same whole-number scale both ways, or not enlarged: draw directly, nearest or linear); -1 when it would be
// needed but even k = 2 does not fit maxTexture (draw with nearest).
int TwoPassFactor(double scaleX, double scaleY, int srcW, int srcH, int maxTexture);

} // namespace core::display
