// Offline test of core/display/scaled_rect (R2c, the China map): whole-number scales are exact copies, scale 1 is
// the picture itself, sharp bilinear keeps each pixel's own colour inside it and blends only at the edges, nothing
// is written outside the clip, the mouse mapping is the inverse of the drawing, and the backdrop of a flat picture
// is that colour darkened.
#include "core/display/scaled_rect.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

namespace d = core::display;

constexpr WORD kRed = 0xF800, kGreen = 0x07E0, kBlue = 0x001F, kWhite = 0xFFFF, kSentinel = 0x1234;

struct Canvas {
    int w, h;
    std::vector<WORD> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_, kSentinel) {}
    d::Target16 target() { return d::Target16{px.data(), w, w, h}; }
    WORD at(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
};

// The backdrop as it was before its vertical passes were made row-wise (2026-10-25): the new one must give the same
// pixels.
void ReferenceBlur(const d::Pixels16 &src, int w, int h, double sigma, int brightness, std::vector<WORD> *out)
{
    out->assign(static_cast<size_t>(std::max(w, 0)) * std::max(h, 0), 0);
    if (!src.px || src.w <= 0 || src.h <= 0 || w <= 0 || h <= 0) {
        return;
    }
    // Cover: the source region that fills w x h, centred; for each output pixel its source position (16.16).
    const double cover = std::max(static_cast<double>(w) / src.w, static_cast<double>(h) / src.h);
    const double rx = (src.w - w / cover) / 2, ry = (src.h - h / cover) / 2;
    std::vector<int> sx0(w), sx1(w), fx(w), sy0(h), sy1(h), fy(h);
    for (int x = 0; x < w; ++x) {
        const double u = std::clamp(rx + (x + 0.5) / cover - 0.5, 0.0, src.w - 1.0);
        sx0[x] = static_cast<int>(u);
        sx1[x] = std::min(sx0[x] + 1, src.w - 1);
        fx[x] = static_cast<int>((u - sx0[x]) * 256);
    }
    for (int y = 0; y < h; ++y) {
        const double v = std::clamp(ry + (y + 0.5) / cover - 0.5, 0.0, src.h - 1.0);
        sy0[y] = static_cast<int>(v);
        sy1[y] = std::min(sy0[y] + 1, src.h - 1);
        fy[y] = static_cast<int>((v - sy0[y]) * 256);
    }
    // Three box passes each way approximate a Gaussian: sigma^2 = 3 ((2r + 1)^2 - 1) / 12.
    const int r = std::max(1, static_cast<int>(std::lround((std::sqrt(4.0 * sigma * sigma + 1.0) - 1.0) / 2.0)));
    static const int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    std::vector<std::uint16_t> ch(static_cast<size_t>(w) * h); // one channel, 8 fractional bits
    std::vector<std::uint16_t> line(std::max(w, h));
    auto boxLine = [&](std::uint16_t *p, int n, int stride) {
        for (int pass = 0; pass < 3; ++pass) {
            long sum = 0;
            for (int k = -r; k <= r; ++k) {
                sum += p[static_cast<size_t>(std::clamp(k, 0, n - 1)) * stride];
            }
            for (int i = 0; i < n; ++i) {
                line[i] = static_cast<std::uint16_t>(sum / (2 * r + 1));
                sum += p[static_cast<size_t>(std::min(i + r + 1, n - 1)) * stride] -
                       p[static_cast<size_t>(std::max(i - r, 0)) * stride];
            }
            for (int i = 0; i < n; ++i) {
                p[static_cast<size_t>(i) * stride] = line[i];
            }
        }
    };
    struct Channel {
        int shift, max, outShift;
    };
    const Channel channels[3] = {{11, 31, 11}, {5, 63, 5}, {0, 31, 0}};
    for (const Channel &c : channels) {
        for (int y = 0; y < h; ++y) { // the covering picture, bilinear
            const WORD *r0 = src.px + static_cast<size_t>(sy0[y]) * src.stride;
            const WORD *r1 = src.px + static_cast<size_t>(sy1[y]) * src.stride;
            std::uint16_t *d = &ch[static_cast<size_t>(y) * w];
            for (int x = 0; x < w; ++x) {
                auto v = [&](const WORD *row, int i) { return (row[i] >> c.shift) & c.max; };
                const int top = v(r0, sx0[x]) * (256 - fx[x]) + v(r0, sx1[x]) * fx[x];
                const int bot = v(r1, sx0[x]) * (256 - fx[x]) + v(r1, sx1[x]) * fx[x];
                d[x] = static_cast<std::uint16_t>((top * (256 - fy[y]) + bot * fy[y]) >> 8); // 8 fractional bits
            }
        }
        for (int y = 0; y < h; ++y) {
            boxLine(&ch[static_cast<size_t>(y) * w], w, 1);
        }
        for (int x = 0; x < w; ++x) {
            boxLine(&ch[x], h, w);
        }
        for (int y = 0; y < h; ++y) { // darkened, back to c.max levels with ordered dithering
            const std::uint16_t *p = &ch[static_cast<size_t>(y) * w];
            WORD *d = out->data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                const int v = (p[x] * brightness) >> 8;                 // 8 fractional bits
                const int q = (v + kBayer[y & 3][x & 3] * 16 + 8) >> 8;  // threshold (0.5 .. 15.5) / 16
                d[x] = static_cast<WORD>(d[x] | (std::min(q, c.max) << c.outShift));
            }
        }
    }
}

} // namespace

int main()
{
    // A 2 x 2 picture: red green / blue white.
    const WORD pic[4] = {kRed, kGreen, kBlue, kWhite};
    const d::Pixels16 src{pic, 2, 2, 2};

    Canvas a(10, 8);
    Check(d::ScaleRgb565(src, 2.0, false, a.target(), 3, 2, RECT{0, 0, 10, 8}), "scale 2, nearest");
    bool exact = true;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            exact = exact && a.at(3 + x, 2 + y) == pic[(y / 2) * 2 + x / 2];
        }
    }
    Check(exact, "whole-number scale: each pixel a 2 x 2 block of its own colour");
    Check(a.at(2, 2) == kSentinel && a.at(7, 2) == kSentinel && a.at(3, 1) == kSentinel && a.at(3, 6) == kSentinel,
          "nothing written around the picture");

    Canvas b(4, 4);
    d::ScaleRgb565(src, 1.0, true, b.target(), 1, 1, RECT{0, 0, 4, 4});
    Check(b.at(1, 1) == kRed && b.at(2, 1) == kGreen && b.at(1, 2) == kBlue && b.at(2, 2) == kWhite && b.at(0, 0) == kSentinel,
          "scale 1, sharp: the picture itself");

    // 4 x 1 picture scaled by 2.5 (sharp): pixel centres keep their colour, edges blend over one pixel at most.
    const WORD row[4] = {kRed, kRed, kBlue, kBlue};
    Canvas c(10, 3);
    d::ScaleRgb565(d::Pixels16{row, 4, 4, 1}, 2.5, true, c.target(), 0, 0, RECT{0, 0, 10, 3});
    int blended = 0;
    for (int x = 0; x < 10; ++x) {
        blended += c.at(x, 0) != kRed && c.at(x, 0) != kBlue;
    }
    Check(c.at(0, 0) == kRed && c.at(3, 0) == kRed && c.at(6, 0) == kBlue && c.at(9, 0) == kBlue && blended <= 1,
          "sharp bilinear: flat inside each pixel, at most one blended pixel at the edge");
    Check(c.at(0, 1) == kRed && c.at(9, 2) == kBlue, "rows of a one-row picture repeat it");

    Canvas e(10, 8);
    d::ScaleRgb565(src, 2.0, false, e.target(), 3, 2, RECT{4, 3, 6, 5});
    int inside = 0, outside = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 10; ++x) {
            const bool in = x >= 4 && x < 6 && y >= 3 && y < 5;
            (in ? inside : outside) += e.at(x, y) != kSentinel;
        }
    }
    Check(inside == 4 && outside == 0, "only inside the clip");
    Canvas f(6, 6);
    Check(d::ScaleRgb565(src, 3.0, true, f.target(), -2, -2, RECT{-5, -5, 50, 50}) && f.at(0, 0) == kRed &&
              f.at(1, 0) == kGreen && f.at(5, 5) == kSentinel,
          "a picture partly off the target: cut at its edges");
    Check(!d::ScaleRgb565(src, 0.5, true, f.target(), 0, 0, RECT{0, 0, 6, 6}), "scales below 1 refused");

    int sx = -1, sy = -1;
    Check(d::ScaledToSource(3, 2, 3, 2, 2.0, 2, 2, &sx, &sy) && sx == 0 && sy == 0 &&
              d::ScaledToSource(6, 5, 3, 2, 2.0, 2, 2, &sx, &sy) && sx == 1 && sy == 1,
          "mouse: the corners map back to the corner pixels");
    Check(!d::ScaledToSource(7, 2, 3, 2, 2.0, 2, 2, &sx, &sy) && !d::ScaledToSource(2, 3, 3, 2, 2.0, 2, 2, &sx, &sy),
          "mouse: outside the picture is not on it");
    bool inverse = true;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            inverse = inverse && d::ScaledToSource(3 + x, 2 + y, 3, 2, 2.0, 2, 2, &sx, &sy) &&
                      a.at(3 + x, 2 + y) == pic[sy * 2 + sx];
        }
    }
    Check(inverse, "mouse: every drawn pixel maps back to the pixel drawn there");
    Check(d::ScaledToSource(1061, 467, 404, 69, 1.799, 848, 746, &sx, &sy) && sx == 365 && sy == 221,
          "mouse: 2560x1440 layout (scale 1.799) maps into the 848x746 map");

    std::vector<WORD> flat(64 * 32, 0x8410); // grey (r 16, g 32, b 16)
    std::vector<WORD> out;
    d::BlurredBackdrop(d::Pixels16{flat.data(), 64, 64, 32}, 200, 120, 24, 128, &out);
    bool dark = out.size() == 200u * 120u;
    for (WORD p : out) {
        const int r = p >> 11, g = (p >> 5) & 63, bl = p & 31;
        dark = dark && std::abs(r - 8) <= 1 && std::abs(g - 16) <= 1 && std::abs(bl - 8) <= 1;
    }
    Check(dark, "backdrop of a flat picture: its colour at half brightness, the whole area");
    std::vector<WORD> half(64 * 32, 0);
    for (int y = 0; y < 32; ++y) {
        for (int x = 32; x < 64; ++x) {
            half[static_cast<size_t>(y) * 64 + x] = 0xFFFF;
        }
    }
    d::BlurredBackdrop(d::Pixels16{half.data(), 64, 64, 32}, 256, 128, 24, 256, &out);
    const int left = out[64 * 256 + 10] >> 11, mid = out[64 * 256 + 128] >> 11, right = out[64 * 256 + 245] >> 11;
    Check(left < 4 && right > 27 && mid > 8 && mid < 24, "backdrop: blurred across the edge, flat far from it");

    // Ordered dithering: a flat red of 16 at brightness 136/256 is 8.5 levels: half the pixels 8, half 9.
    std::vector<WORD> red(64 * 32, static_cast<WORD>(16 << 11));
    d::BlurredBackdrop(d::Pixels16{red.data(), 64, 64, 32}, 64, 32, 3.0, 136, &out);
    int eights = 0, nines = 0;
    for (WORD p : out) {
        eights += (p >> 11) == 8;
        nines += (p >> 11) == 9;
    }
    Check(eights == 1024 && nines == 1024, "dithering: between two levels, half each (no bands)");

    // The row-wise vertical passes give exactly the same pixels as the column-wise ones; time at 2560x1440.
    std::vector<WORD> noise(2334 * 1400);
    unsigned seed = 12345;
    for (WORD &p : noise) {
        seed = seed * 1103515245u + 12345u;
        p = static_cast<WORD>(seed >> 16);
    }
    bool same = true;
    for (double sigma : {2.0, 3.0, 24.0}) {
        std::vector<WORD> a, b;
        d::BlurredBackdrop(d::Pixels16{noise.data(), 2334, 300, 200}, 320, 180, sigma, 166, &a);
        ReferenceBlur(d::Pixels16{noise.data(), 2334, 300, 200}, 320, 180, sigma, 166, &b);
        same = same && a == b;
    }
    Check(same, "backdrop: row-wise vertical passes give the same pixels (sigma 2, 3, 24)");
    auto t0 = std::chrono::steady_clock::now();
    d::BlurredBackdrop(d::Pixels16{noise.data(), 2334, 2334, 1400}, 2560, 1440, 2.0, 166, &out);
    auto t1 = std::chrono::steady_clock::now();
    std::vector<WORD> old;
    ReferenceBlur(d::Pixels16{noise.data(), 2334, 2334, 1400}, 2560, 1440, 2.0, 166, &old);
    auto t2 = std::chrono::steady_clock::now();
    const double now = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double before = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("backdrop 2334x1400 -> 2560x1440, sigma 2: %.0f ms (column-wise before: %.0f ms)\n", now, before);
    Check(out == old, "backdrop: same pixels at 2560x1440");

    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
