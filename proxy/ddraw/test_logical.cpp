// Offline test of core/display/logical_size (not the game): the display scale rules of
// docs/resolution_ui/display_scale.md §6.1 with Emperor's 1024x768 minimum.
#include "core/display/logical_size.h"

#include <cstdio>

namespace {

int g_failures = 0;

void Expect(const char *what, int nw, int nh, core_resolution_kind kind, int fw, int fh, int scale, int w, int h,
            int pct, bool limited)
{
    core::display::LogicalSize r = core::display::ChooseLogicalSize(nw, nh, kind, fw, fh, scale, 1024, 768);
    bool ok = r.w == w && r.h == h && r.scale_percent == pct && r.limited == limited;
    std::printf("%-44s %dx%d %d%%%s  %s\n", what, r.w, r.h, r.scale_percent, r.limited ? " limited" : "",
                ok ? "ok" : "FAILED");
    if (!ok) {
        std::printf("    expected %dx%d %d%%%s\n", w, h, pct, limited ? " limited" : "");
        ++g_failures;
    }
}

} // namespace

int main()
{
    const auto F = CORE_RESOLUTION_FIXED, S = CORE_RESOLUTION_SCALED;
    Expect("fixed 1024x768 on 2560x1440", 2560, 1440, F, 1024, 768, 0, 1024, 768, 100, false);
    Expect("fixed 1280x1024 on 2560x1440", 2560, 1440, F, 1280, 1024, 0, 1280, 1024, 100, false);
    Expect("fixed 2560x1080 on 2560x1440", 2560, 1440, F, 2560, 1080, 0, 2560, 1080, 100, false);
    Expect("fixed 800x600 raised to the minimum", 2560, 1440, F, 800, 600, 0, 1024, 768, 100, true);
    Expect("auto on 1920x1080", 1920, 1080, S, 0, 0, 0, 1920, 1080, 100, false);
    Expect("auto on 2560x1440 (exactly 1080 high)", 2560, 1440, S, 0, 0, 0, 1920, 1080, 133, false);
    Expect("auto on 3440x1440 (ultrawide)", 3440, 1440, S, 0, 0, 0, 2580, 1080, 133, false);
    Expect("auto on 2560x1600", 2560, 1600, S, 0, 0, 0, 1728, 1080, 148, false);
    Expect("auto on 1080x1920 (portrait: too narrow)", 1080, 1920, S, 0, 0, 0, 1080, 1920, 100, false);
    Expect("auto on 3840x2160", 3840, 2160, S, 0, 0, 0, 1920, 1080, 200, false);
    Expect("100% on 3840x2160 (R4 test C, offline)", 3840, 2160, S, 0, 0, 100, 3840, 2160, 100, false);
    Expect("125% on 3840x2160", 3840, 2160, S, 0, 0, 125, 3072, 1728, 125, false);
    Expect("250% on 3840x2160 (the largest scale fits)", 3840, 2160, S, 0, 0, 250, 1536, 864, 250, false);
    Expect("auto on 1920x1200", 1920, 1200, S, 0, 0, 0, 1728, 1080, 111, false);
    Expect("auto on 2560x1080 (ultrawide)", 2560, 1080, S, 0, 0, 0, 2560, 1080, 100, false);
    Expect("150% on 2560x1440", 2560, 1440, S, 0, 0, 150, 1706, 960, 150, false);
    Expect("250% on 2560x1440 lowered to 185%", 2560, 1440, S, 0, 0, 250, 1382, 778, 185, true);
    Expect("100% on 1280x720 (screen below the minimum)", 1280, 720, S, 0, 0, 100, 1280, 768, 100, true);
    Expect("auto on 1024x768", 1024, 768, S, 0, 0, 0, 1024, 768, 100, false);
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
