// Offline test of core/display/common_sizes (not the game; R2b.1-9): the common sizes listed for a screen are those
// that fit it (not larger than its native size, not smaller than the minimum), smallest first, the native size among
// them, by default only of the screen's aspect; all aspects on request; aspect names; heavy sizes marked.
#include "core/display/common_sizes.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-80s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

using core::display::CommonSize;

// "1366x768 1600x900 ..." for the log and the comparisons.
const char *List(const CommonSize *s, int n)
{
    static char out[1024];
    out[0] = 0;
    for (int i = 0; i < n; ++i) {
        size_t len = std::strlen(out);
        std::snprintf(out + len, sizeof(out) - len, "%s%dx%d%s", i ? " " : "", s[i].w, s[i].h, s[i].native ? "*" : "");
    }
    return out;
}

bool Sorted(const CommonSize *s, int n)
{
    for (int i = 1; i < n; ++i) {
        if (s[i].w < s[i - 1].w || (s[i].w == s[i - 1].w && s[i].h <= s[i - 1].h)) {
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    CommonSize s[64];
    int n = core::display::CommonSizes(2560, 1440, 1024, 768, false, s, 64);
    std::printf("2560x1440, 16:9 only: %s\n", List(s, n));
    Check(std::strcmp(List(s, n), "1366x768 1600x900 1920x1080 2560x1440*") == 0,
          "2560x1440 screen: the 16:9 sizes up to it, the native one marked");
    n = core::display::CommonSizes(1920, 1080, 1024, 768, false, s, 64);
    Check(std::strcmp(List(s, n), "1366x768 1600x900 1920x1080*") == 0, "1920x1080 screen: nothing larger than it");
    n = core::display::CommonSizes(3840, 2160, 1024, 768, false, s, 64);
    Check(n == 6 && s[5].w == 3840 && s[5].native && s[5].heavy && s[4].w == 3200 && s[4].heavy && !s[3].heavy,
          "4K screen: up to 3840x2160, 3200x1800 and 3840x2160 marked heavy");
    n = core::display::CommonSizes(2560, 1600, 1024, 768, false, s, 64);
    Check(std::strcmp(List(s, n), "1280x800 1440x900 1680x1050 1920x1200 2560x1600*") == 0, "16:10 screen: 16:10 sizes");
    n = core::display::CommonSizes(3440, 1440, 1024, 768, false, s, 64);
    Check(std::strcmp(List(s, n), "2560x1080 3440x1440*") == 0, "ultrawide 3440x1440: 21:9 sizes");
    n = core::display::CommonSizes(1366, 768, 1024, 768, false, s, 64);
    Check(std::strcmp(List(s, n), "1366x768*") == 0, "1366x768 laptop: only itself (720p is below the minimum)");
    n = core::display::CommonSizes(2560, 1440, 1024, 768, true, s, 64);
    std::printf("2560x1440, all aspects: %s\n", List(s, n));
    Check(Sorted(s, n) && n > 10, "all aspects: more sizes, smallest first");
    bool fits = true, has1024 = false, has1600x1200 = false;
    for (int i = 0; i < n; ++i) {
        fits = fits && s[i].w <= 2560 && s[i].h <= 1440 && s[i].w >= 1024 && s[i].h >= 768;
        has1024 = has1024 || (s[i].w == 1024 && s[i].h == 768);
        has1600x1200 = has1600x1200 || (s[i].w == 1600 && s[i].h == 1200);
    }
    Check(fits && has1024 && has1600x1200, "all aspects: every one fits the screen and the minimum (4:3 included)");
    n = core::display::CommonSizes(1700, 1000, 1024, 768, false, s, 64);
    Check(n >= 1 && s[n - 1].w == 1700 && s[n - 1].native && s[n - 1].aspect == nullptr,
          "an unusual screen (1700x1000): its own size listed, without an aspect name");
    n = core::display::CommonSizes(1000, 700, 1024, 768, false, s, 64);
    Check(n == 0, "a screen smaller than the minimum: nothing listed");
    n = core::display::CommonSizes(2560, 1440, 1024, 768, true, s, 3);
    Check(n == 3, "at most max entries");
    Check(std::strcmp(core::display::AspectName(1920, 1080), "16:9") == 0 &&
              std::strcmp(core::display::AspectName(1366, 768), "16:9") == 0 &&
              std::strcmp(core::display::AspectName(1680, 1050), "16:10") == 0 &&
              std::strcmp(core::display::AspectName(3440, 1440), "21:9") == 0 &&
              std::strcmp(core::display::AspectName(1280, 1024), "5:4") == 0 &&
              std::strcmp(core::display::AspectName(2256, 1504), "3:2") == 0 &&
              core::display::AspectName(1000, 1000) == nullptr,
          "aspect names (1366x768 counts as 16:9)");
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
