// Which tile of a set goes where, for the game's dialog art built of 16 px tiles (Yongle's options
// window painter and core/display/framed_screens drew the same layouts each in its own copy). Both games'
// dialog art has the same layouts (Emperor's panel_draw_tiles 0x5bed30 / inset_panel_draw_tiles 0x5bf080; Zeus's
// panel group 0x83 / inset 0x84 have the same counts and order).
//
// Each walker calls put(tile, x, y) for every tile in drawing order (later tiles cover earlier ones where they
// overlap): tile is the index within the set, x / y where its top left goes. Nothing here knows a game or draws.
//
// Threads: any (no state).
#pragma once

#include "core/display/framing.h"

#include <algorithm>
#include <vector>

namespace core::display::tiles {

constexpr int kSize = 16;

// A nine-part box: corners, edges cycling over edgeCycle tiles, middle rows cycling over rowsCycle rows of perRow
// tiles (the inside of a row is at inside + row offset + column cycle).
struct Nine {
    int perRow, edgeCycle, rowsCycle;
    int tl, tr, bl, br, top, bottom, left, right, inside;
};
constexpr Nine kPanel{12, 10, 10, 0, 0xb, 0x84, 0x8f, 1, 0x85, 0xc, 0x17, 0xd}; // the dialog panel (parchment)
constexpr Nine kInset{7, 5, 5, 0, 6, 0x2a, 0x30, 1, 0x2b, 7, 0xd, 8};             // the sunken box

inline std::vector<int> Positions(int size)
{
    std::vector<int> out(static_cast<size_t>(std::max(2, (size + kSize - 1) / kSize)));
    out.resize(static_cast<size_t>(FrameTilePositions(size, out.data(), static_cast<int>(out.size()))));
    return out;
}

// The box w x h at (x0, y0), whole tiles with the last row and column flush with the far edges. inside = false: the
// border only (framed_screens draws its own pattern inside).
template <class Put>
void WalkNine(const Nine &n, int x0, int y0, int w, int h, bool inside, Put put)
{
    const std::vector<int> xs = Positions(w), ys = Positions(h);
    int mid = 0;
    for (size_t r = 0; r < ys.size(); ++r) {
        int k = 0;
        for (size_t c = 0; c < xs.size(); ++c) {
            const bool lc = c == xs.size() - 1, lr = r == ys.size() - 1;
            int i = -1;
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
            else if (inside) {
                i = n.inside + mid + k;
            }
            if (i >= 0) {
                put(i, x0 + xs[c], y0 + ys[r]);
            }
            if (c > 0 && !lc) {
                k = (k + 1) % n.edgeCycle;
            }
        }
        if (r > 0 && r < ys.size() - 1) {
            mid = (mid + n.perRow) % (n.perRow * n.rowsCycle);
        }
    }
}

// Left, middle..., right (tiles 0, 1, 2) along w: a button bar, a strip. At least two tiles; w / 16 of them.
template <class Put>
void WalkPieces(int x, int y, int w, Put put)
{
    const int n = std::max(2, w / kSize);
    for (int k = 0; k < n; ++k) {
        put(k == 0 ? 0 : k == n - 1 ? 2 : 1, x + k * kSize, y);
    }
}

// The frame around the button under the mouse (Emperor's button_frame_draw 0x5bf360): eight tiles, 0 top left, then
// clockwise (1 top, 2 top right, 3 right, 4 bottom right, 5 bottom, 6 bottom left, 7 left).
template <class Put>
void WalkFrame(int x, int y, int w, int h, Put put)
{
    for (int xx = x + kSize; xx < x + w - kSize; xx += kSize) {
        put(1, xx, y);
        put(5, xx, y + h - kSize);
    }
    for (int yy = y + kSize; yy < y + h - kSize; yy += kSize) {
        put(7, x, yy);
        put(3, x + w - kSize, yy);
    }
    put(0, x, y);
    put(2, x + w - kSize, y);
    put(4, x + w - kSize, y + h - kSize);
    put(6, x, y + h - kSize);
}

// A thin line (Emperor's hline_draw 0x5bf250): left, middle..., right (tiles 0, 1, 2); at least two tiles wide.
template <class Put>
void WalkLine(int x, int y, int w, Put put)
{
    w = std::max(2 * kSize, w);
    put(0, x, y);
    for (int xx = x + kSize; xx < x + w - kSize; xx += kSize) {
        put(1, xx, y);
    }
    put(2, x + w - kSize, y);
}

} // namespace core::display::tiles
