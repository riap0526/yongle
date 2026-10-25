// Offline test of core/display/framing (not the game): the geometry of a framed full-screen picture
// (docs/resolution_ui/fullscreen_art.md §7.5). The approved v4 mockups (tools/mockups/fsmock3.py) give the
// expected numbers; the invariants (picture centred in the frame, frame centred on the screen, the outside fill
// either at least two border widths or none, even padding) are checked over many screen sizes.
#include "core/display/framing.h"

#include <cmath>
#include <cstdio>

namespace {

using core::display::ComputeFrameLayout;
using core::display::FrameLayout;
using core::display::kTile;

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-72s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

void Expect(const char *what, int W, int H, int pw, int ph, double s, int fx, int fy, int fw, int fh, bool sx, bool sy)
{
    FrameLayout l{};
    bool ok = ComputeFrameLayout(W, H, pw, ph, &l) && std::fabs(l.s - s) < 0.001 && l.frameX == fx &&
              l.frameY == fy && l.frameW == fw && l.frameH == fh && l.snapX == sx && l.snapY == sy;
    std::printf("%-40s s %.3f frame %dx%d at %d,%d (%d x %d units, pad %d/%d) snap %d/%d  %s\n", what, l.s, l.frameW,
                l.frameH, l.frameX, l.frameY, l.frameU, l.frameV, l.padU, l.padV, l.snapX, l.snapY,
                ok ? "ok" : "FAILED");
    if (!ok) {
        std::printf("    expected s %.3f frame %dx%d at %d,%d snap %d/%d\n", s, fw, fh, fx, fy, sx, sy);
        ++g_failures;
    }
}

bool Invariants(int W, int H, int pw, int ph, char *why, size_t size)
{
    FrameLayout l{};
    if (!ComputeFrameLayout(W, H, pw, ph, &l)) {
        std::snprintf(why, size, "no layout");
        return false;
    }
    const double b = kTile * l.s;
    const double kx = static_cast<double>(l.frameW) / l.frameU, ky = static_cast<double>(l.frameH) / l.frameV;
    double leftGap = l.picX - l.frameX, rightGap = l.frameX + l.frameW - (l.picX + l.picW);
    double topGap = l.picY - l.frameY, bottomGap = l.frameY + l.frameH - (l.picY + l.picH);
    int outX = l.frameX, outX2 = W - l.frameX - l.frameW, outY = l.frameY, outY2 = H - l.frameY - l.frameH;
    struct {
        bool ok;
        const char *what;
    } checks[] = {
        {std::fabs(leftGap - rightGap) < 0.01 && std::fabs(topGap - bottomGap) < 0.01, "picture centred in the frame"},
        {std::abs(outX - outX2) <= 1 && std::abs(outY - outY2) <= 1, "frame centred on the screen"},
        {l.frameX >= 0 && l.frameY >= 0 && l.frameW <= W && l.frameH <= H, "frame on the screen"},
        {(l.frameU - 2 * kTile - pw) % 2 == 0 && (l.frameV - 2 * kTile - ph) % 2 == 0 && l.padU >= 0 && l.padV >= 0,
         "even padding, not negative"},
        {l.snapX ? (l.frameX == 0 && l.frameW == W) : (outX >= 2 * b - 1), "x: at the edge, or outside >= 2 borders"},
        {l.snapY ? (l.frameY == 0 && l.frameH == H) : (outY >= 2 * b - 1), "y: at the edge, or outside >= 2 borders"},
        {std::fabs(kx - l.s) / l.s < 0.01 && std::fabs(ky - l.s) / l.s < 0.01, "frame scale within 1% of s"},
        {l.outerU * l.s >= W && l.outerV * l.s >= H, "outer fill covers the screen"},
    };
    for (const auto &c : checks) {
        if (!c.ok) {
            std::snprintf(why, size, "%s (%dx%d, picture %dx%d)", c.what, W, H, pw, ph);
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    std::printf("-- the v4 mockups (fullscreen_art.md §7.5)\n");
    Expect("main menu 1024x768 on 2560x1440", 2560, 1440, 1024, 768, 1.800, 329, 0, 1901, 1440, false, true);
    Expect("load screen 798x412 on 2560x1440", 2560, 1440, 798, 412, 3.084, 0, 0, 2560, 1440, true, true);
    Expect("load screen 798x412 on 2560x1080", 2560, 1080, 798, 412, 2.432, 270, 0, 2019, 1080, false, true);

    std::printf("-- native (R2d.1): the picture at its own size, centred; as large as fits when it does not fit\n");
    {
        FrameLayout n{};
        Check(ComputeFrameLayout(2560, 1440, 1024, 768, &n, true) && n.native && n.s == 1.0 && n.frameW == 1056 &&
                  n.frameH == 800 && n.frameX == 752 && n.frameY == 320 && !n.snapX && !n.snapY && n.picX == 768 &&
                  n.picY == 336 && n.outerU == 2561 && n.outerV == 1441,
              "menu 1024x768 native on 2560x1440: frame 1056x800 at 752,320, picture at 768,336");
        Check(ComputeFrameLayout(2560, 1080, 798, 468, &n, true) && n.native && n.frameW == 830 && n.frameH == 500 &&
                  n.frameX == 865 && n.frameY == 290,
              "load block 798x468 native on 2560x1080: frame 830x500 at 865,290");
        Check(ComputeFrameLayout(1024, 768, 1024, 768, &n, true) && !n.native && n.s < 1.0,
              "menu native on a 1024x768 screen: does not fit, scaled to fit");
        FrameLayout f{};
        Check(ComputeFrameLayout(2560, 1440, 1024, 768, &f, false) && !f.native && std::fabs(f.s - 1.8) < 0.001,
              "native off: as before");
    }

    std::printf("-- the v4 padding: load screen on 2560x1440 has 37 px above and below the picture\n");
    FrameLayout l{};
    ComputeFrameLayout(2560, 1440, 798, 412, &l);
    double top = l.picY - l.frameY - kTile * static_cast<double>(l.frameH) / l.frameV;
    Check(std::fabs(top - 37) < 1, "padding top ~37 px");

    std::printf("-- tile positions (last tile flush with the far edge)\n");
    int pos[128];
    int n = core::display::FrameTilePositions(830, pos, 128);
    Check(n == 52 && pos[0] == 0 && pos[1] == 16 && pos[50] == 800 && pos[51] == 814, "830 units: 0,16,..,800,814");
    n = core::display::FrameTilePositions(1056, pos, 128);
    Check(n == 66 && pos[65] == 1040 && pos[64] == 1024, "1056 units: whole tiles");

    std::printf("-- invariants over screen sizes 1024..3840 x 768..2160 (1024x768 and 798x412 pictures)\n");
    long cases = 0, bad = 0;
    char why[200] = "";
    for (int W = 1024; W <= 3840; W += 37) {
        for (int H = 768; H <= 2160; H += 29) {
            for (int p = 0; p < 2; ++p) {
                ++cases;
                char w2[200];
                if (!Invariants(W, H, p ? 798 : 1024, p ? 412 : 768, w2, sizeof(w2))) {
                    if (!bad++) {
                        std::snprintf(why, sizeof(why), "%s", w2);
                    }
                }
            }
        }
    }
    std::printf("%ld cases, %ld failed%s%s\n", cases, bad, bad ? ": first: " : "", why);
    if (bad) {
        ++g_failures;
    }
    const int common[][2] = {{1920, 1080}, {2560, 1440}, {2560, 1080}, {1280, 1024}, {1920, 1200}, {3840, 2160}};
    for (const auto &c : common) {
        for (int p = 0; p < 2; ++p) {
            char what[96];
            bool ok = Invariants(c[0], c[1], p ? 798 : 1024, p ? 412 : 768, why, sizeof(why));
            std::snprintf(what, sizeof(what), "invariants %dx%d, picture %s", c[0], c[1], p ? "798x412" : "1024x768");
            Check(ok, what);
        }
    }
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
