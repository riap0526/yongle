#include "core/display/city_zoom.h"

#include <cmath>

namespace core::display {

namespace {

int CeilDiv(long long a, long long b)
{
    return static_cast<int>((a + b - 1) / b);
}

} // namespace

int ZoomedSize(int show, int percent)
{
    return CeilDiv(static_cast<long long>(show) * percent, 100) + 1;
}

void ZoomView(const ZoomInput &in, int percent, int *cols, int *rows)
{
    *cols = CeilDiv(ZoomedSize(in.showW, percent) + 2, in.colPx);
    *rows = CeilDiv(ZoomedSize(in.showH, percent), in.halfH) + 1;
}

int ZoomToBuffer(int screen, int origin, double f)
{
    const int i = screen - origin;
    if (f > 1.0) {
        // Shrinking: screen pixel i shows the block [floor(i f), floor((i + 1) f)) (ShrinkRgb565's blocks); its middle
        // pixel, the one nearest shrinking samples. (A tiny epsilon: i f is a whole number when i * bufW is a
        // multiple of showW, and must not come out just below it.)
        const int b0 = static_cast<int>(std::floor(i * f + 1e-9));
        int b1 = static_cast<int>(std::floor((i + 1) * f + 1e-9));
        b1 = b1 > b0 ? b1 : b0 + 1;
        return origin + static_cast<int>(std::floor((b0 + b1 - 1) / 2.0)); // also left of the origin
    }
    // Enlarging (or 1:1): the buffer pixel under the screen pixel's centre.
    return origin + static_cast<int>(std::floor((i + 0.5) * f));
}

int ZoomToScreen(int buffer, int origin, double f)
{
    return origin + static_cast<int>(std::lround((buffer - origin) / f));
}

int ZoomNotch(int base, int notches, int low, int high)
{
    int p = base;
    for (int n = notches; n > 0; --n) {
        p = (p / kZoomNotch + 1) * kZoomNotch; // the next grid value above
    }
    for (int n = notches; n < 0; ++n) {
        p = ((p + kZoomNotch - 1) / kZoomNotch - 1) * kZoomNotch; // the next grid value below
    }
    if (high < low) {
        high = low;
    }
    return p < low ? low : p > high ? high : p;
}

double ZoomAnchorShift(int cursor, int origin, double fBefore, double fAfter)
{
    return (cursor - origin) * (fBefore - fAfter);
}

bool ViewFitsMap(int cols, int rows, int mapCols, int mapRows, int clampRows)
{
    return cols + 1 <= mapCols && rows + 2 <= mapRows && (clampRows <= 0 || rows <= clampRows);
}

bool ZoomFits(const ZoomInput &in, int percent)
{
    int cols = 0, rows = 0;
    ZoomView(in, percent, &cols, &rows);
    return ViewFitsMap(cols, rows, in.mapCols, in.mapRows, in.clampRows);
}

ZoomLayout ZoomFor(const ZoomInput &in)
{
    ZoomLayout z{};
    const int cap = in.maxPercent > 0 && in.maxPercent < kZoomMax ? (in.maxPercent < kZoomMin ? kZoomMin : in.maxPercent)
                                                                   : kZoomMax;
    int want = in.requested < kZoomMin ? kZoomMin : in.requested > cap ? cap : in.requested;
    if (in.showW <= 0 || in.showH <= 0 || in.colPx <= 0 || in.halfH <= 0) {
        want = 100;
    }
    if (in.mapCols <= 0 || in.mapRows <= 0 || in.showW <= 0 || in.showH <= 0 || in.colPx <= 0 || in.halfH <= 0) {
        z.unknownMap = in.mapCols <= 0 || in.mapRows <= 0;
        z.percent = 100;
        z.capped = want != 100;
    }
    else {
        int limit = 0;
        for (int p = cap; p >= kZoomMin; --p) {
            if (ZoomFits(in, p)) {
                limit = p;
                break;
            }
        }
        z.limit = limit;
        // Augustus: the scale at which the whole map plus 4 tiles on each side fits, the larger of the two axes, at
        // least 100.
        const int ax = CeilDiv(static_cast<long long>(in.mapCols + 8) * in.colPx * 100, in.showW);
        const int ay = CeilDiv(static_cast<long long>(in.mapRows + 8) * in.halfH * 100, in.showH);
        z.augustus = ax > ay ? ax : ay;
        z.augustus = z.augustus < 100 ? 100 : z.augustus;
        z.percent = limit == 0 ? kZoomMin : (want > limit ? limit : want);
        const int asked = in.requested < kZoomMin ? kZoomMin : in.requested > kZoomMax ? kZoomMax : in.requested;
        z.capped = asked > z.percent; // by the map or by the cap
    }
    z.bufW = ZoomedSize(in.showW, z.percent);
    z.bufH = ZoomedSize(in.showH, z.percent);
    if (in.colPx > 0 && in.halfH > 0) {
        ZoomView(in, z.percent, &z.cols, &z.rows);
    }
    return z;
}

} // namespace core::display
