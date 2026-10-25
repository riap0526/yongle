#include "core/display/logical_size.h"

#include <cstdio>

namespace core::display {

namespace {

constexpr int kMinScale = 100, kMaxScale = 250, kStep = 5;
constexpr int kAutoHeight = 1080;

int Div(int native, int percent)
{
    return static_cast<int>(static_cast<long long>(native) * 100 / percent);
}

} // namespace

LogicalSize ChooseLogicalSize(int native_w, int native_h, core_resolution_kind kind, int fixed_w, int fixed_h,
                              int scale_percent, int min_w, int min_h)
{
    LogicalSize r;
    if (kind == CORE_RESOLUTION_FIXED || native_w <= 0 || native_h <= 0) {
        r.w = fixed_w;
        r.h = fixed_h;
        if (r.w < min_w || r.h < min_h) {
            r.w = r.w < min_w ? min_w : r.w;
            r.h = r.h < min_h ? min_h : r.h;
            r.limited = true;
        }
        return r;
    }
    int p = scale_percent;
    if (p == 0) { // auto (R4): 1080 rows on a taller screen, at any ratio (2560x1440 -> 1920x1080, 133 %)
        const int w = static_cast<int>(static_cast<long long>(native_w) * kAutoHeight / native_h) & ~1;
        if (native_h > kAutoHeight && w >= min_w && kAutoHeight >= min_h) {
            r.w = w;
            r.h = kAutoHeight;
            r.scale_percent = static_cast<int>((static_cast<long long>(native_h) * 100 + kAutoHeight / 2) / kAutoHeight);
            return r;
        }
        p = kMinScale; // the screen's own size (the minimum and its limits below as for any scale)
    }
    // The largest scale that keeps the minimum size.
    int maxP = kMinScale;
    while (maxP + kStep <= kMaxScale && Div(native_w, maxP + kStep) >= min_w && Div(native_h, maxP + kStep) >= min_h) {
        maxP += kStep;
    }
    if (p > maxP) {
        p = maxP;
        r.limited = true;
    }
    r.scale_percent = p;
    r.w = Div(native_w, p) & ~1; // even widths keep every row 4-byte aligned in a 16-bit buffer
    r.h = Div(native_h, p);
    if (r.w < min_w || r.h < min_h) { // a screen smaller than the minimum
        r.w = r.w < min_w ? min_w : r.w;
        r.h = r.h < min_h ? min_h : r.h;
        r.scale_percent = 100;
        r.limited = true;
    }
    return r;
}

void DescribeSetting(core_resolution_kind kind, int fixed_w, int fixed_h, int scale_percent, char *out, int size)
{
    if (kind == CORE_RESOLUTION_FIXED) {
        std::snprintf(out, size, "%dx%d", fixed_w, fixed_h);
    }
    else if (scale_percent == 0) {
        std::snprintf(out, size, "scaled auto");
    }
    else {
        std::snprintf(out, size, "scaled %d%%", scale_percent);
    }
}

namespace {
int g_minW = 0, g_minH = 0;
} // namespace

void SetGameMinimum(int min_w, int min_h)
{
    g_minW = min_w;
    g_minH = min_h;
}

void GameMinimum(int *min_w, int *min_h)
{
    *min_w = g_minW;
    *min_h = g_minH;
}

} // namespace core::display
