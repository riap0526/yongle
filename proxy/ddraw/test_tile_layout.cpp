// Offline test of core/display/tile_layout (no DLL, no game): the shared tile walkers give exactly the tiles, places
// and order of the copies they replaced -- Yongle's options window painter (DrawNine, Pieces, Frame,
// Line in games/emperor/options_window.cpp) and core/display/framed_screens (Frame, Inset) -- kept below as they
// were, over many sizes.
#include "core/display/tile_layout.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace tl = core::display::tiles;

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

struct Put {
    int i, x, y;
    bool operator==(const Put &o) const { return i == o.i && x == o.x && y == o.y; }
};
using Puts = std::vector<Put>;

constexpr int kTile = 16;

// --- The copies as they were (HEAD 5ad80f3) ---

std::vector<int> OldPositions(int size) // options_window.cpp Positions
{
    int count = std::max(2, (size + kTile - 1) / kTile);
    std::vector<int> out{0};
    for (int k = 1; k < count - 1; ++k) {
        out.push_back(kTile * k);
    }
    out.push_back(size - kTile);
    return out;
}

void OldDrawNine(int base, const tl::Nine &n, int x0, int y0, int w, int h, Puts &o) // options_window.cpp DrawNine
{
    std::vector<int> xs = OldPositions(w), ys = OldPositions(h);
    int mid = 0;
    for (size_t r = 0; r < ys.size(); ++r) {
        int k = 0;
        for (size_t c = 0; c < xs.size(); ++c) {
            bool lc = c == xs.size() - 1, lr = r == ys.size() - 1;
            int i;
            if (r == 0) {
                i = c == 0 ? n.tl : lc ? n.tr : n.top + k;
            }
            else if (lr) {
                i = c == 0 ? n.bl : lc ? n.br : n.bottom + k;
            }
            else if (c == 0) {
                i = n.left + mid;
            }
            else if (lc) {
                i = n.right + mid;
            }
            else {
                i = n.inside + mid + k;
            }
            o.push_back({base + i, x0 + xs[c], y0 + ys[r]});
            if (c > 0 && !lc) {
                k = (k + 1) % n.edgeCycle;
            }
        }
        if (r > 0 && r < ys.size() - 1) {
            mid = (mid + n.perRow) % (n.perRow * n.rowsCycle);
        }
    }
}

void OldPieces(int base, int x, int y, int w, Puts &o)
{
    int n = std::max(2, w / kTile);
    for (int k = 0; k < n; ++k) {
        o.push_back({base + (k == 0 ? 0 : k == n - 1 ? 2 : 1), x + k * kTile, y});
    }
}

void OldFrame(int b, int x, int y, int w, int h, Puts &o)
{
    for (int xx = x + kTile; xx < x + w - kTile; xx += kTile) {
        o.push_back({b + 1, xx, y});
        o.push_back({b + 5, xx, y + h - kTile});
    }
    for (int yy = y + kTile; yy < y + h - kTile; yy += kTile) {
        o.push_back({b + 7, x, yy});
        o.push_back({b + 3, x + w - kTile, yy});
    }
    o.push_back({b + 0, x, y});
    o.push_back({b + 2, x + w - kTile, y});
    o.push_back({b + 4, x + w - kTile, y + h - kTile});
    o.push_back({b + 6, x, y + h - kTile});
}

void OldLine(int b, int x, int y, int w, Puts &o)
{
    w = std::max(2 * kTile, w);
    o.push_back({b, x, y});
    for (int xx = x + kTile; xx < x + w - kTile; xx += kTile) {
        o.push_back({b + 1, xx, y});
    }
    o.push_back({b + 2, x + w - kTile, y});
}

int OldFrameTilePositions(int size, int *out, int max) // framing.cpp
{
    int count = std::max(2, (size + kTile - 1) / kTile);
    int n = 0;
    for (int k = 0; k < count && n < max; ++k) {
        out[n++] = k == 0 ? 0 : k == count - 1 ? size - kTile : kTile * k;
    }
    return n;
}

void OldFramedFrame(int w, int h, Puts &o) // framed_screens.cpp Frame (the border only)
{
    int xs[512], ys[512];
    const int nx = OldFrameTilePositions(w, xs, 512), ny = OldFrameTilePositions(h, ys, 512);
    int midRow = 0;
    for (int r = 0; r < ny; ++r) {
        int k = 0;
        for (int c = 0; c < nx; ++c) {
            const bool lastC = c == nx - 1, lastR = r == ny - 1;
            int i = -1;
            if (r == 0) {
                i = c == 0 ? 0 : lastC ? 0xb : 1 + k;
            }
            else if (lastR) {
                i = c == 0 ? 0x84 : lastC ? 0x8f : 0x85 + k;
            }
            else if (c == 0) {
                i = 0xc + midRow;
            }
            else if (lastC) {
                i = 0x17 + midRow;
            }
            if (i >= 0) {
                o.push_back({i, xs[c], ys[r]});
            }
            if (c > 0 && !lastC) {
                k = (k + 1) % 10;
            }
        }
        if (r > 0 && r < ny - 1) {
            midRow = (midRow + 12) % 120;
        }
    }
}

void OldFramedInset(int x0, int y0, int w, int h, Puts &o) // framed_screens.cpp Inset
{
    int xs[512], ys[64];
    const int nx = OldFrameTilePositions(w, xs, 512), ny = OldFrameTilePositions(h, ys, 64);
    int mid = 0;
    for (int r = 0; r < ny; ++r) {
        int k = 0;
        for (int c = 0; c < nx; ++c) {
            const bool lastC = c == nx - 1, lastR = r == ny - 1;
            int i;
            if (r == 0) {
                i = c == 0 ? 0 : lastC ? 6 : 1 + k;
            }
            else if (lastR) {
                i = c == 0 ? 0x2a : lastC ? 0x30 : 0x2b + k;
            }
            else {
                i = c == 0 ? 7 + mid : lastC ? 0xd + mid : 8 + mid + k;
            }
            o.push_back({i, x0 + xs[c], y0 + ys[r]});
            if (c > 0 && !lastC) {
                k = (k + 1) % 5;
            }
        }
        if (r > 0 && r < ny - 1) {
            mid = (mid + 7) % 35;
        }
    }
}

// The old painter's panel constants (options_window.cpp kPanelTiles / kInsetTiles).
constexpr tl::Nine kOldPanel{12, 10, 10, 0, 0xb, 0x84, 0x8f, 1, 0x85, 0xc, 0x17, 0xd};
constexpr tl::Nine kOldInset{7, 5, 5, 0, 6, 0x2a, 0x30, 1, 0x2b, 7, 0xd, 8};

bool SameNine(const tl::Nine &a, const tl::Nine &b)
{
    return a.perRow == b.perRow && a.edgeCycle == b.edgeCycle && a.rowsCycle == b.rowsCycle && a.tl == b.tl &&
           a.tr == b.tr && a.bl == b.bl && a.br == b.br && a.top == b.top && a.bottom == b.bottom &&
           a.left == b.left && a.right == b.right && a.inside == b.inside;
}

} // namespace

int main()
{
    Check(SameNine(tl::kPanel, kOldPanel) && SameNine(tl::kInset, kOldInset), "the panel and inset constants as before");

    // Sizes: below one tile, exact multiples, odd sizes, large (the options window is 640 x 480; framed pictures up
    // to the screen in units).
    std::vector<int> sizes;
    for (int s = 1; s <= 200; ++s) sizes.push_back(s);
    for (int s = 200; s <= 1300; s += 7) sizes.push_back(s);

    bool nine = true, framed = true, inset = true, pieces = true, frame = true, line = true;
    int cases = 0;
    for (int w : sizes) {
        for (int h : {1, 15, 16, 17, 25, 31, 32, 33, 48, 100, 208, 480, 481, 1000}) {
            ++cases;
            for (const tl::Nine *n : {&tl::kPanel, &tl::kInset}) {
                Puts a, b;
                OldDrawNine(0x100, *n, 7, 9, w, h, a);
                tl::WalkNine(*n, 7, 9, w, h, true, [&](int i, int x, int y) { b.push_back({0x100 + i, x, y}); });
                nine = nine && a == b;
            }
            {
                Puts a, b;
                OldFramedFrame(w, h, a);
                tl::WalkNine(tl::kPanel, 0, 0, w, h, false, [&](int i, int x, int y) { b.push_back({i, x, y}); });
                framed = framed && a == b;
            }
            if (h <= 64 * kTile) { // the old inset kept at most 64 rows
                Puts a, b;
                OldFramedInset(3, 5, w, h, a);
                tl::WalkNine(tl::kInset, 3, 5, w, h, true, [&](int i, int x, int y) { b.push_back({i, x, y}); });
                inset = inset && a == b;
            }
            {
                Puts a, b;
                OldFrame(0x40, 11, 13, w, h, a);
                tl::WalkFrame(11, 13, w, h, [&](int i, int x, int y) { b.push_back({0x40 + i, x, y}); });
                frame = frame && a == b;
            }
        }
        {
            Puts a, b;
            OldPieces(0x20, 5, 6, w, a);
            tl::WalkPieces(5, 6, w, [&](int i, int x, int y) { b.push_back({0x20 + i, x, y}); });
            pieces = pieces && a == b;
        }
        {
            Puts a, b;
            OldLine(0x30, 5, 6, w, a);
            tl::WalkLine(5, 6, w, [&](int i, int x, int y) { b.push_back({0x30 + i, x, y}); });
            line = line && a == b;
        }
    }
    std::printf("(%d sizes w x h)\n", cases);
    Check(nine, "nine-part box: panel and inset as the options window painter");
    Check(framed, "panel border only: as framed_screens Frame");
    Check(inset, "inset: as framed_screens Inset");
    Check(frame, "hover frame: as the options window painter");
    Check(pieces, "left / middle / right: as the options window painter");
    Check(line, "thin line: as the options window painter");

    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
