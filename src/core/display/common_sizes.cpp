#include "core/display/common_sizes.h"

namespace core::display {

namespace {

struct Known {
    int w, h;
};

// From small to large within each aspect (docs/archive/resolution_ui_r2b1.md, R2b.1-9).
constexpr Known kSizes[] = {
    // 16:9
    {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160},
    // 16:10
    {1280, 800}, {1440, 900}, {1680, 1050}, {1920, 1200}, {2560, 1600}, {2880, 1800}, {3840, 2400},
    // 21:9
    {2560, 1080}, {3440, 1440}, {3840, 1600},
    // 4:3, 5:4
    {1024, 768}, {1280, 960}, {1280, 1024}, {1600, 1200},
    // 3:2
    {2160, 1440}, {2256, 1504}, {2736, 1824}, {3000, 2000},
};

struct Aspect {
    int w, h;
    const char *name;
};
constexpr Aspect kAspects[] = {{16, 9, "16:9"}, {16, 10, "16:10"}, {64, 27, "21:9"},
                               {4, 3, "4:3"},   {5, 4, "5:4"},    {3, 2, "3:2"}};

// Within 2% of each other.
bool Near(double a, double b)
{
    double d = a > b ? a - b : b - a;
    return d <= 0.02 * b;
}

bool SameAspect(int w1, int h1, int w2, int h2)
{
    return h1 > 0 && h2 > 0 && Near(static_cast<double>(w1) / h1, static_cast<double>(w2) / h2);
}

bool Less(const CommonSize &a, const CommonSize &b)
{
    return a.w != b.w ? a.w < b.w : a.h < b.h;
}

} // namespace

const char *AspectName(int w, int h)
{
    if (h <= 0) {
        return nullptr;
    }
    const char *best = nullptr;
    double bestDiff = 1e9, r = static_cast<double>(w) / h;
    for (const Aspect &a : kAspects) {
        double ar = static_cast<double>(a.w) / a.h, d = r > ar ? r - ar : ar - r;
        if (Near(r, ar) && d < bestDiff) {
            best = a.name;
            bestDiff = d;
        }
    }
    return best;
}

int CommonSizes(int nativeW, int nativeH, int minW, int minH, bool allAspects, CommonSize *out, int max)
{
    int n = 0;
    auto add = [&](int w, int h, bool native) {
        if (n >= max || w < minW || h < minH || w > nativeW || h > nativeH) {
            return;
        }
        if (!allAspects && !SameAspect(w, h, nativeW, nativeH)) {
            return;
        }
        for (int k = 0; k < n; ++k) {
            if (out[k].w == w && out[k].h == h) {
                out[k].native = out[k].native || native;
                return;
            }
        }
        out[n++] = CommonSize{w, h, AspectName(w, h), native, static_cast<long long>(w) * h >= 3200LL * 1800};
    };
    add(nativeW, nativeH, true);
    for (const Known &k : kSizes) {
        add(k.w, k.h, false);
    }
    for (int i = 1; i < n; ++i) { // insertion sort: a few dozen entries
        CommonSize v = out[i];
        int j = i - 1;
        while (j >= 0 && Less(v, out[j])) {
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = v;
    }
    return n;
}

} // namespace core::display
