#include "core/display/scaled_rect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace core::display {
namespace {

// 565 <-> 0x07E0F81F spread (green in the high half): two weighted pixels add without overlapping (weights 0..32).
inline DWORD Spread(WORD p)
{
    return (p | (static_cast<DWORD>(p) << 16)) & 0x07E0F81Fu;
}

inline WORD Pack(DWORD v)
{
    v &= 0x07E0F81Fu;
    return static_cast<WORD>(v | (v >> 16));
}

inline WORD Lerp(WORD a, WORD b, int w) // w: 0..32, the share of b
{
    if (w <= 0) {
        return a;
    }
    if (w >= 32) {
        return b;
    }
    return Pack((Spread(a) * (32 - w) + Spread(b) * w) >> 5);
}

// For each destination pixel along one axis: the two source pixels and the share (0..32) of the second.
struct Tap {
    int i0, i1, w;
};

void Taps(int count, int origin, double scale, bool sharp, int srcSize, std::vector<Tap> *out)
{
    out->resize(count);
    for (int d = 0; d < count; ++d) {
        // The destination pixel's centre in source pixels.
        const double v = (d - origin + 0.5) / scale - 0.5;
        int i0 = static_cast<int>(std::floor(v));
        double f = v - i0;
        if (sharp) {
            f = (f - 0.5) * scale + 0.5; // the blend only across the pixel edge, one output pixel wide
            f = f < 0 ? 0 : f > 1 ? 1 : f;
        }
        else {
            f = f < 0.5 ? 0 : 1;
        }
        int i1 = i0 + 1;
        i0 = std::clamp(i0, 0, srcSize - 1);
        i1 = std::clamp(i1, 0, srcSize - 1);
        (*out)[d] = Tap{i0, i1, static_cast<int>(f * 32 + 0.5)};
    }
}

} // namespace

bool ScaleRgb565(const Pixels16 &src, double scale, bool sharp, const Target16 &dst, int x, int y, const RECT &clip)
{
    if (!src.px || !dst.px || src.w <= 0 || src.h <= 0 || src.stride < src.w || dst.stride < dst.w || dst.w <= 0 ||
        dst.h <= 0 || !(scale >= 1.0) || scale > 64.0) {
        return false;
    }
    const int outW = static_cast<int>(std::lround(src.w * scale)), outH = static_cast<int>(std::lround(src.h * scale));
    const int left = std::max({static_cast<int>(clip.left), 0, x});
    const int top = std::max({static_cast<int>(clip.top), 0, y});
    const int right = std::min({static_cast<int>(clip.right), dst.w, x + outW});
    const int bottom = std::min({static_cast<int>(clip.bottom), dst.h, y + outH});
    if (left >= right || top >= bottom) {
        return true; // nothing visible
    }
    std::vector<Tap> cols, rows;
    Taps(right - left, x - left, scale, sharp, src.w, &cols);
    Taps(bottom - top, y - top, scale, sharp, src.h, &rows);
    const int n = right - left;
    std::vector<WORD> a(n), b(n);
    for (int r = 0; r < bottom - top; ++r) {
        const Tap &t = rows[r];
        const WORD *s0 = src.px + static_cast<size_t>(t.i0) * src.stride;
        const WORD *s1 = src.px + static_cast<size_t>(t.i1) * src.stride;
        WORD *d = dst.px + static_cast<size_t>(top + r) * dst.stride + left;
        for (int c = 0; c < n; ++c) {
            const Tap &u = cols[c];
            a[c] = Lerp(s0[u.i0], s0[u.i1], u.w);
        }
        if (t.w == 0 || t.i0 == t.i1) {
            std::copy(a.begin(), a.end(), d);
            continue;
        }
        for (int c = 0; c < n; ++c) {
            const Tap &u = cols[c];
            b[c] = Lerp(s1[u.i0], s1[u.i1], u.w);
        }
        if (t.w >= 32) {
            std::copy(b.begin(), b.end(), d);
            continue;
        }
        for (int c = 0; c < n; ++c) {
            d[c] = Lerp(a[c], b[c], t.w);
        }
    }
    return true;
}

bool ShrinkRgb565(const Pixels16 &src, int outW, int outH, bool average, const Target16 &dst)
{
    if (!src.px || !dst.px || src.w <= 0 || src.h <= 0 || src.stride < src.w || outW <= 0 || outH <= 0 ||
        outW > src.w || outH > src.h || outW > dst.w || outH > dst.h || dst.stride < dst.w) {
        return false;
    }
    // Source pixel boundaries of each output column and row: [b[i], b[i + 1]).
    std::vector<int> bx(outW + 1), by(outH + 1);
    for (int i = 0; i <= outW; ++i) {
        bx[i] = static_cast<int>(static_cast<long long>(i) * src.w / outW);
    }
    for (int j = 0; j <= outH; ++j) {
        by[j] = static_cast<int>(static_cast<long long>(j) * src.h / outH);
    }
    const int maxW = (src.w + outW - 1) / outW, maxH = (src.h + outH - 1) / outH;
    if (!average || maxW * maxH > 32) {
        // Nearest: the source pixel at the centre of each block.
        std::vector<int> cx(outW);
        for (int i = 0; i < outW; ++i) {
            cx[i] = (bx[i] + bx[i + 1] - 1) / 2;
        }
        for (int j = 0; j < outH; ++j) {
            const WORD *s = src.px + static_cast<size_t>((by[j] + by[j + 1] - 1) / 2) * src.stride;
            WORD *d = dst.px + static_cast<size_t>(j) * dst.stride;
            for (int i = 0; i < outW; ++i) {
                d[i] = s[cx[i]];
            }
        }
        return true;
    }
    // Average: every source pixel of the block counted once (spread sums: at most 32 pixels, checked above), then
    // divided by the block's size with a 16.16 reciprocal.
    std::vector<DWORD> column(src.w);
    std::vector<DWORD> inverse(33);
    for (int n = 1; n <= 32; ++n) {
        inverse[n] = (65536u + n / 2) / n;
    }
    for (int j = 0; j < outH; ++j) {
        const int y0 = by[j], y1 = by[j + 1] > y0 ? by[j + 1] : y0 + 1;
        const WORD *row = src.px + static_cast<size_t>(y0) * src.stride;
        for (int x = 0; x < src.w; ++x) {
            column[x] = Spread(row[x]);
        }
        for (int y = y0 + 1; y < y1; ++y) {
            row = src.px + static_cast<size_t>(y) * src.stride;
            for (int x = 0; x < src.w; ++x) {
                column[x] += Spread(row[x]);
            }
        }
        WORD *d = dst.px + static_cast<size_t>(j) * dst.stride;
        for (int i = 0; i < outW; ++i) {
            const int x0 = bx[i], x1 = bx[i + 1] > x0 ? bx[i + 1] : x0 + 1;
            DWORD sum = 0;
            for (int x = x0; x < x1; ++x) {
                sum += column[x];
            }
            const DWORD inv = inverse[(x1 - x0) * (y1 - y0)];
            const DWORD b = ((sum & 0x3FFu) * inv + 0x8000u) >> 16;
            const DWORD r = (((sum >> 11) & 0x3FFu) * inv + 0x8000u) >> 16;
            const DWORD g = ((sum >> 21) * inv + 0x8000u) >> 16;
            d[i] = static_cast<WORD>((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
        }
    }
    return true;
}

bool ScaledToSource(int dx, int dy, int x, int y, double scale, int srcW, int srcH, int *sx, int *sy)
{
    if (!(scale > 0)) {
        return false;
    }
    const int u = static_cast<int>(std::floor((dx - x + 0.5) / scale));
    const int v = static_cast<int>(std::floor((dy - y + 0.5) / scale));
    if (dx < x || dy < y || u < 0 || v < 0 || u >= srcW || v >= srcH) {
        return false;
    }
    *sx = u;
    *sy = v;
    return true;
}

void BlurredBackdrop(const Pixels16 &src, int w, int h, double sigma, int brightness, std::vector<WORD> *out)
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
    std::vector<std::uint16_t> tmp(ch.size());
    std::vector<long> colSum(w);
    // sum / (2r + 1) as a multiplication: exact while sum < 2^16 (2r + 1) and 2r + 1 < 2^16 (the error stays below
    // (2r + 1) / 2^16 of one step), which the 16-bit channel values guarantee.
    const std::uint64_t inv = (std::uint64_t{1} << 32) / static_cast<std::uint64_t>(2 * r + 1) + 1;
    auto divide = [inv](long sum) {
        return static_cast<std::uint16_t>((static_cast<std::uint64_t>(sum) * inv) >> 32);
    };
    auto boxLine = [&](std::uint16_t *p, int n, int stride) {
        for (int pass = 0; pass < 3; ++pass) {
            long sum = 0;
            for (int k = -r; k <= r; ++k) {
                sum += p[static_cast<size_t>(std::clamp(k, 0, n - 1)) * stride];
            }
            for (int i = 0; i < n; ++i) {
                line[i] = divide(sum);
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
        for (int y = 0; y < h && sigma > 0; ++y) {
            boxLine(&ch[static_cast<size_t>(y) * w], w, 1);
        }
        // The vertical passes row by row (the same sums as boxLine down each column, but reading whole rows: the
        // column-wise walk took most of the 350 ms at 2560x1440, R2d.1 follow-up).
        for (int pass = 0; pass < 3 && sigma > 0; ++pass) {
            for (int x = 0; x < w; ++x) {
                long s = 0;
                for (int k = -r; k <= r; ++k) {
                    s += ch[static_cast<size_t>(std::clamp(k, 0, h - 1)) * w + x];
                }
                colSum[x] = s;
            }
            for (int y = 0; y < h; ++y) {
                const std::uint16_t *add = &ch[static_cast<size_t>(std::min(y + r + 1, h - 1)) * w];
                const std::uint16_t *sub = &ch[static_cast<size_t>(std::max(y - r, 0)) * w];
                std::uint16_t *o = &tmp[static_cast<size_t>(y) * w];
                for (int x = 0; x < w; ++x) {
                    o[x] = divide(colSum[x]);
                    colSum[x] += add[x] - sub[x];
                }
            }
            ch.swap(tmp);
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

int TwoPassFactor(double scaleX, double scaleY, int srcW, int srcH, int maxTexture)
{
    constexpr double kEps = 1e-3;
    const double s = std::max(scaleX, scaleY);
    const double whole = std::round(scaleX);
    if (s <= 1 + kEps || (whole >= 1 && std::fabs(scaleX - whole) < kEps && std::fabs(scaleY - whole) < kEps)) {
        return 1;
    }
    int k = static_cast<int>(std::ceil(s - kEps));
    while (k >= 2 && (static_cast<long long>(k) * srcW > maxTexture || static_cast<long long>(k) * srcH > maxTexture)) {
        --k;
    }
    return k >= 2 ? k : -1;
}

} // namespace core::display
