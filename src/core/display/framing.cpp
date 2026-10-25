#include "core/display/framing.h"

#include <algorithm>
#include <cmath>

namespace core::display {

namespace {

// Python's round() for the halves that occur here (banker's rounding would differ only at exact .5, where the
// mockup tool and this code must agree: both use round-half-even).
long RoundHalfEven(double v)
{
    return std::lrint(v); // default rounding mode: to nearest, ties to even
}

struct Axis {
    int sizePx, units;
    bool snap;
};

Axis AxisLayout(int screen, int picture, double s)
{
    const double border = kTile * s;
    const double natural = (picture + 2 * kTile) * s;
    Axis a{};
    a.snap = (screen - natural) / 2 < kSnapBorders * border;
    a.sizePx = a.snap ? screen : static_cast<int>(RoundHalfEven(natural));
    a.units = static_cast<int>(RoundHalfEven(a.sizePx / s));
    if ((a.units - 2 * kTile - picture) % 2) {
        ++a.units; // even padding: the picture exactly in the middle
    }
    return a;
}

} // namespace

bool ComputeFrameLayout(int screenW, int screenH, int pictureW, int pictureH, FrameLayout *out, bool native)
{
    if (screenW <= 0 || screenH <= 0 || pictureW <= 0 || pictureH <= 0) {
        return false;
    }
    FrameLayout l{};
    l.s = std::min(static_cast<double>(screenW) / (pictureW + 2 * kTile),
                   static_cast<double>(screenH) / (pictureH + 2 * kTile));
    if (native && l.s >= 1.0) {
        l.s = 1.0;
        l.native = true;
    }
    Axis x = AxisLayout(screenW, pictureW, l.s), y = AxisLayout(screenH, pictureH, l.s);
    l.frameW = x.sizePx;
    l.frameH = y.sizePx;
    l.frameU = x.units;
    l.frameV = y.units;
    l.snapX = x.snap;
    l.snapY = y.snap;
    l.frameX = (screenW - l.frameW) / 2;
    l.frameY = (screenH - l.frameH) / 2;
    l.padU = (l.frameU - 2 * kTile - pictureW) / 2;
    l.padV = (l.frameV - 2 * kTile - pictureH) / 2;
    const double kx = static_cast<double>(l.frameW) / l.frameU, ky = static_cast<double>(l.frameH) / l.frameV;
    l.picX = l.frameX + (kTile + l.padU) * kx;
    l.picY = l.frameY + (kTile + l.padV) * ky;
    l.picW = pictureW * kx;
    l.picH = pictureH * ky;
    l.outerU = static_cast<int>(RoundHalfEven(screenW / l.s)) + 1;
    l.outerV = static_cast<int>(RoundHalfEven(screenH / l.s)) + 1;
    *out = l;
    return true;
}

int FrameTilePositions(int size, int *out, int max)
{
    int count = std::max(2, (size + kTile - 1) / kTile);
    int n = 0;
    for (int k = 0; k < count && n < max; ++k) {
        out[n++] = k == 0 ? 0 : k == count - 1 ? size - kTile : kTile * k;
    }
    return n;
}

} // namespace core::display
