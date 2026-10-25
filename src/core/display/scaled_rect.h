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

// Scales src by `scale` (>= 1) to (x, y) of dst, size round(src.w * scale) x round(src.h * scale), writing only inside
// clip (which is cut to dst). sharp: sharp bilinear; otherwise nearest (for whole-number scales: exact copies).
// False (nothing written) when the arguments make no sense.
bool ScaleRgb565(const Pixels16 &src, double scale, bool sharp, const Target16 &dst, int x, int y, const RECT &clip);

// The source's inverse of ScaleRgb565: the source pixel under destination pixel (dx, dy) of a picture drawn at
// (x, y) by `scale`. False when it is outside the picture.
bool ScaledToSource(int dx, int dy, int x, int y, double scale, int srcW, int srcH, int *sx, int *sy);

// out := src scaled to cover w x h (centred, cropped, bilinear), blurred by about `sigma` output pixels (three box
// passes each way), multiplied by brightness / 256 and reduced to RGB565 with 4 x 4 ordered dithering (dark, smooth
// gradients would otherwise show bands). Full resolution: meant to be computed once per picture and size.
void BlurredBackdrop(const Pixels16 &src, int w, int h, double sigma, int brightness, std::vector<WORD> *out);

} // namespace core::display
