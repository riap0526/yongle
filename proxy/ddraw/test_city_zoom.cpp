// Offline test of core/display/city_zoom and ShrinkRgb565 (R5, docs/resolution_ui/r5_city_zoom.md section 8.2):
//   - for map ranges seen in the game (54x112 .. 112x224) and several screens: the limit is the largest percent whose
//     view (plus the drawing's extra column and two half rows) is within the map's range, one more does not fit, and
//     every percent up to it fits; a request above it is capped to it; an unknown map gives 100; a map too small for
//     even 50% gives limit 0;
//   - for every percent 50..400: the buffer region shown plus any remainder stays inside the drawn clip;
//   - enlarging (ScaleRgb565 at 100/percent) covers the whole city on the screen;
//   - averaged and nearest shrinking: flat colours stay, a checkerboard averages to grey, nearest picks one pixel,
//     nonsense is refused;
//   - view_clamp's bottom margin (section 9.1): the limit's view is never taller than the clamp holds; the caller's
//     cap (150);
//   - the rows split over our workers (core/platform/parallel, section 9.2) give the same pixels as one thread, and
//     with no workers everything runs on the caller.
#include "core/display/city_zoom.h"
#include "core/display/scaled_rect.h"
#include "core/platform/parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace cd = core::display;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("%-80s FAILED\n", what);
    }
}

void CheckShown(bool ok, const char *what)
{
    Check(ok, what);
    if (ok) {
        std::printf("%-80s ok\n", what);
    }
}

struct Screen {
    const char *name;
    int showW, showH; // the city on the screen: sidebar_x x (height - 40)
};

struct Map {
    int cols, rows;
};

} // namespace

int main()
{
    const Screen screens[] = {{"2560x1440", 2334, 1400}, {"1920x1080", 1694, 1040}, {"3840x2160", 3614, 2120},
                              {"2560x1600", 2334, 1560}, {"1280x720", 1054, 680},   {"1422x800", 1196, 760}};
    const Map maps[] = {{54, 112}, {68, 140}, {84, 168}, {112, 224}, {160, 320}};
    const int colPx = 80, halfH = 20;
    char what[200];

    // --- limits and caps
    for (const Screen &sc : screens) {
        for (const Map &m : maps) {
            cd::ZoomInput in{400, sc.showW, sc.showH, colPx, halfH, m.cols, m.rows};
            const cd::ZoomLayout z = cd::ZoomFor(in);
            std::snprintf(what, sizeof(what), "%s, map %d x %d: limit %d%%, view %d x %d, buffer %d x %d px",
                          sc.name, m.cols, m.rows, z.limit, z.cols, z.rows, z.bufW, z.bufH);
            CheckShown(z.limit >= cd::kZoomMin && z.percent == z.limit, what);
            Check(cd::ZoomFits(in, z.limit), "  the limit fits");
            Check(z.cols + 1 <= m.cols && z.rows + 2 <= m.rows, "  its view plus the drawing's margin is in the map");
            if (z.limit < cd::kZoomMax) {
                Check(!cd::ZoomFits(in, z.limit + 1), "  one percent more does not fit");
                Check(z.capped, "  a request of 400 is capped");
            }
            for (int p = cd::kZoomMin; p <= z.limit; ++p) {
                Check(cd::ZoomFits(in, p), "  every percent up to the limit fits");
                in.requested = p;
                const cd::ZoomLayout q = cd::ZoomFor(in);
                Check(q.percent == p && !q.capped, "  a request within the limit is kept");
                int cols = 0, rows = 0;
                cd::ZoomView(in, p, &cols, &rows);
                Check(cols + 1 <= m.cols && rows + 2 <= m.rows, "  no percent allowed gives a view past the map");
            }
            in.requested = z.limit + 37;
            const cd::ZoomLayout over = cd::ZoomFor(in);
            Check(over.percent == (z.limit + 37 > cd::kZoomMax ? z.limit : z.limit) || over.percent == z.limit,
                  "  a request over the limit gives the limit");
        }
    }

    // --- the numbers of r5_city_zoom.md section 8.2 (2560x1440: city 29 x 71 at 100%)
    {
        cd::ZoomInput in{400, 2334, 1400, colPx, halfH, 54, 112};
        const int small = cd::ZoomFor(in).limit;
        in.mapCols = 112;
        in.mapRows = 224;
        const int large = cd::ZoomFor(in).limit;
        std::snprintf(what, sizeof(what), "2560x1440: small map limit %d%% (about 150-158), large map %d%% (300-315)",
                      small, large);
        CheckShown(small >= 145 && small <= 158 && large >= 295 && large <= 315, what);
    }

    // --- unknown or too small maps, nonsense
    {
        cd::ZoomInput in{200, 2334, 1400, colPx, halfH, 0, 0};
        const cd::ZoomLayout z = cd::ZoomFor(in);
        CheckShown(z.percent == 100 && z.unknownMap, "no map range yet: 100%");
        in.mapCols = 10;
        in.mapRows = 20;
        const cd::ZoomLayout t = cd::ZoomFor(in);
        CheckShown(t.limit == 0 && !cd::ZoomFits(in, cd::kZoomMin), "a map smaller than 50% of the view: limit 0");
        in.mapCols = 112;
        in.mapRows = 224;
        in.requested = 10;
        CheckShown(cd::ZoomFor(in).percent == cd::kZoomMin, "a request below 50 is 50");
        in.requested = 100;
        CheckShown(cd::ZoomFor(in).percent == 100, "100 stays 100");
    }

    // --- the drawing's arithmetic for every percent (games/emperor/scroll RenderZoomed)
    {
        bool ok = true;
        for (const Screen &sc : screens) {
            for (int p = cd::kZoomMin; p <= cd::kZoomMax; ++p) {
                const cd::ZoomInput in{p, sc.showW, sc.showH, colPx, halfH, 1000, 1000};
                const cd::ZoomLayout z = cd::ZoomFor(in);
                const int right2 = (z.cols + 1) * colPx - 2, bottom2 = (z.rows + 1) * halfH; // relative to the origin
                const int pxMax = colPx - 1, pyMax = 2 * halfH - 1;
                if (pxMax + z.bufW > right2 || pyMax + z.bufH > bottom2) {
                    ok = false;
                    std::printf("  %s %d%%: buffer part %d x %d + remainder does not fit the clip %d x %d\n", sc.name,
                                p, z.bufW, z.bufH, right2, bottom2);
                }
                if (p < 100 && std::lround(z.bufW * 100.0 / p) < sc.showW) {
                    ok = false;
                    std::printf("  %s %d%%: enlarged %ld px wide < %d\n", sc.name, p, std::lround(z.bufW * 100.0 / p),
                                sc.showW);
                }
                if (p < 100 && std::lround(z.bufH * 100.0 / p) < sc.showH) {
                    ok = false;
                }
            }
        }
        CheckShown(ok, "every percent: the part shown plus any remainder is inside the clip; enlarging covers it");
    }

    // --- shrinking
    {
        const int sw = 300, sh = 200;
        std::vector<WORD> src(sw * sh, 0x1234), dst(150 * 100, 0);
        const cd::Pixels16 s{src.data(), sw, sw, sh};
        const cd::Target16 d{dst.data(), 150, 150, 100};
        bool flat = cd::ShrinkRgb565(s, 150, 100, true, d);
        for (WORD w : dst) {
            flat = flat && w == 0x1234;
        }
        CheckShown(flat, "average, 2:1: a flat colour stays the same");
        for (int y = 0; y < sh; ++y) {
            for (int x = 0; x < sw; ++x) {
                src[y * sw + x] = ((x + y) & 1) ? 0xFFFF : 0x0000;
            }
        }
        cd::ShrinkRgb565(s, 150, 100, true, d);
        const WORD grey = dst[50 * 150 + 70];
        const int r = grey >> 11, g = (grey >> 5) & 63, b = grey & 31;
        std::snprintf(what, sizeof(what), "average, 2:1: a black and white checkerboard is grey (%d, %d, %d)", r, g,
                      b);
        CheckShown(r >= 15 && r <= 16 && g >= 31 && g <= 32 && b >= 15 && b <= 16, what);
        cd::ShrinkRgb565(s, 150, 100, false, d);
        CheckShown(dst[50 * 150 + 70] == 0x0000 || dst[50 * 150 + 70] == 0xFFFF,
                   "nearest, 2:1: a checkerboard gives black or white pixels");
        std::vector<WORD> odd(97 * 61, 0x5555);
        const cd::Target16 d2{odd.data(), 97, 97, 61};
        CheckShown(cd::ShrinkRgb565(s, 97, 61, true, d2) && cd::ShrinkRgb565(s, 97, 61, false, d2),
                   "average and nearest at 300 x 200 -> 97 x 61 (uneven blocks)");
        std::vector<WORD> big(400 * 300, 0);
        const cd::Target16 d3{big.data(), 400, 400, 300};
        CheckShown(!cd::ShrinkRgb565(s, 400, 300, true, d3), "larger than the source: refused");
        std::vector<WORD> tiny(10 * 10, 0x7777);
        const cd::Target16 d4{tiny.data(), 10, 10, 10};
        bool big32 = cd::ShrinkRgb565(s, 10, 10, true, d4); // 30 x 20 blocks: more than 32 pixels, nearest
        CheckShown(big32, "a block over 32 pixels falls back to nearest");
    }

    // --- view_clamp's bottom margin (r5_city_zoom.md section 9.1): the view never taller than the clamp holds
    {
        bool ok = true;
        for (const Screen &sc : screens) {
            for (const Map &m : maps) {
                for (int margin : {0, 1, 3, 6, 12}) {
                    const int clampRows = m.rows - (margin ? margin + 4 : 0);
                    cd::ZoomInput in{400, sc.showW, sc.showH, colPx, halfH, m.cols, m.rows, clampRows};
                    const cd::ZoomLayout z = cd::ZoomFor(in);
                    if (z.limit == 0) {
                        continue;
                    }
                    if (z.rows > clampRows || z.rows + 2 > m.rows || z.cols + 1 > m.cols) {
                        ok = false;
                        std::printf("  %s map %d x %d margin %d: %d%% gives %d half rows (clamp %d)\n", sc.name,
                                    m.cols, m.rows, margin, z.limit, z.rows, clampRows);
                    }
                    if (z.limit < cd::kZoomMax && cd::ZoomFits(in, z.limit + 1)) {
                        ok = false;
                    }
                }
            }
        }
        CheckShown(ok, "with a bottom margin: the limit's view fits the clamp, one percent more does not");
        // The first test's case: 112 x 224 at 2560x1440 asked 315 gave 222 half rows; with a margin it must be less.
        cd::ZoomInput in{315, 2334, 1400, colPx, halfH, 112, 224, 224 - (3 + 4)};
        const cd::ZoomLayout z = cd::ZoomFor(in);
        std::snprintf(what, sizeof(what), "112 x 224, margin 3: limit %d%%, %d half rows <= %d", z.limit, z.rows,
                      224 - 7);
        CheckShown(z.rows <= 224 - 7 && z.limit < 315, what);
    }

    // --- the caller's cap (games/emperor/zoom: 150)
    {
        cd::ZoomInput in{315, 2334, 1400, colPx, halfH, 112, 224, 0, 150};
        const cd::ZoomLayout z = cd::ZoomFor(in);
        CheckShown(z.percent == 150 && z.limit == 150 && z.capped, "cap 150: a request of 315 gives 150, capped");
        in.requested = 125;
        CheckShown(cd::ZoomFor(in).percent == 125, "cap 150: 125 stays 125");
        in.mapCols = 54;
        in.mapRows = 112;
        in.requested = 150;
        const cd::ZoomLayout s = cd::ZoomFor(in);
        std::snprintf(what, sizeof(what), "cap 150 on the small map: the map's own limit %d%% when lower", s.limit);
        CheckShown(s.limit <= 150 && s.percent == s.limit, what);
    }

    // --- the same pixels with the rows split over our workers (core/platform/parallel)
    {
        struct Case {
            int sw, sh, ow, oh;
        };
        const Case cases[] = {{2919, 1751, 2334, 1400}, {3502, 2101, 2334, 1400}, {301, 203, 97, 61}, {64, 40, 63, 9}};
        unsigned seed = 12345;
        bool same = true;
        for (const Case &k : cases) {
            std::vector<WORD> src(static_cast<size_t>(k.sw) * k.sh);
            for (WORD &w : src) {
                seed = seed * 1103515245u + 12345u;
                w = static_cast<WORD>(seed >> 16);
            }
            const cd::Pixels16 s{src.data(), k.sw, k.sw, k.sh};
            for (bool average : {true, false}) {
                std::vector<WORD> a(static_cast<size_t>(k.ow) * k.oh, 1), b(a.size(), 2);
                cd::ShrinkRgb565(s, k.ow, k.oh, average, cd::Target16{a.data(), k.ow, k.ow, k.oh});
                cd::ShrinkRgb565(s, k.ow, k.oh, average, cd::Target16{b.data(), k.ow, k.ow, k.oh},
                                 &core::platform::parallel::ForRows);
                same = same && a == b;
            }
            // Enlarging the shrunk size back up (50%-like): sharp bilinear, into a larger picture with a clip.
            const cd::Pixels16 small{src.data(), k.sw, k.ow, k.oh};
            const int tw = k.ow * 2 + 7, th = k.oh * 2 + 5;
            std::vector<WORD> a(static_cast<size_t>(tw) * th, 3), b(a.size(), 3);
            const RECT clip{3, 2, tw - 1, th - 2};
            cd::ScaleRgb565(small, 2.0, true, cd::Target16{a.data(), tw, tw, th}, 3, 2, clip);
            cd::ScaleRgb565(small, 2.0, true, cd::Target16{b.data(), tw, tw, th}, 3, 2, clip,
                            &core::platform::parallel::ForRows);
            same = same && a == b;
        }
        std::snprintf(what, sizeof(what), "split over %d worker(s): shrinking and enlarging give the same pixels",
                      core::platform::parallel::Workers());
        CheckShown(same, what);
        // Timing (printed only; the real numbers come from the game's log): 125% at 2560x1440.
        {
            std::vector<WORD> big(2919 * 1751, 0x1234), out(2334 * 1400);
            const cd::Pixels16 s{big.data(), 2919, 2919, 1751};
            const cd::Target16 d{out.data(), 2334, 2334, 1400};
            LARGE_INTEGER f, t0, t1, t2;
            QueryPerformanceFrequency(&f);
            double serial = 1e9, split = 1e9;
            for (int i = 0; i < 20; ++i) {
                QueryPerformanceCounter(&t0);
                cd::ShrinkRgb565(s, 2334, 1400, true, d);
                QueryPerformanceCounter(&t1);
                cd::ShrinkRgb565(s, 2334, 1400, true, d, &core::platform::parallel::ForRows);
                QueryPerformanceCounter(&t2);
                serial = std::min(serial, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
                split = std::min(split, (t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart);
            }
            std::printf("  timing, 2919 x 1751 -> 2334 x 1400 average: one thread %.2f ms, split %.2f ms (best of 20)\n",
                        serial, split);
        }
        core::platform::parallel::LimitWorkers(0);
        std::vector<WORD> src(300 * 200, 0x4321), a(150 * 100, 0);
        cd::ShrinkRgb565(cd::Pixels16{src.data(), 300, 300, 200}, 150, 100, true, cd::Target16{a.data(), 150, 150, 100},
                         &core::platform::parallel::ForRows);
        bool flat = core::platform::parallel::Workers() == 0;
        for (WORD w : a) {
            flat = flat && w == 0x4321;
        }
        CheckShown(flat, "no workers: everything on the caller");
        core::platform::parallel::LimitWorkers(core::platform::parallel::kMaxWorkers);
    }

    std::printf("RESULT: %s (%d checks, %d failure(s))\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
