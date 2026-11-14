// Offline test of core/display/city_layout (not the game): the spare rows' allowance, tiling a row, and the sidebar
// filler's pixel functions (an inset panel's tile placement at any size, mirrored tiling, moving a block, repeating a
// band of rows). The surface ends right before a PAGE_NOACCESS page, so a write past it faults and the test fails.
// Results are compared with plain models.
#include "core/display/city_layout.h"
#include "core/display/moved_rect.h"

#include <cstdio>
#include <cstring>
#include <vector>

using core::display::Surface16;

namespace {

int g_failures = 0, g_cases = 0;

void Check(bool ok, const char *what)
{
    ++g_cases;
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++g_failures;
    }
}

struct Screen {
    BYTE *px = nullptr;
    Surface16 s{};
    Screen(int w, int h)
    {
        const size_t bytes = static_cast<size_t>(w) * h * 2, page = 4096, body = (bytes + page - 1) / page * page;
        BYTE *base = static_cast<BYTE *>(VirtualAlloc(nullptr, body + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old;
        VirtualProtect(base + body, page, PAGE_NOACCESS, &old);
        px = base + body - bytes;
        s = Surface16{px, w * 2, w, h, bytes};
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                At(x, y) = static_cast<WORD>(y * 7 + x);
            }
        }
    }
    WORD &At(int x, int y) { return reinterpret_cast<WORD *>(px)[y * s.w + x]; }
};

struct Placed {
    int k, x, y;
};
std::vector<Placed> g_placed;

void Place(int k, int x, int y, void *)
{
    g_placed.push_back({k, x, y});
}

void Allowance()
{
    using core::display::SpareRowsAllowance;
    Check(SpareRowsAllowance(1020, 0, 15, 1013) == 7, "Zeus 1013: the clip 7 rows past the screen");
    Check(SpareRowsAllowance(765, 0, 15, 768) == 0, "the clip inside the screen: none");
    Check(SpareRowsAllowance(765, 768, 15, 768) == 12, "shown past the clip: the next half row (smooth scrolling)");
    Check(SpareRowsAllowance(2000, 0, 15, 1080) == 30, "never more than two half rows");
    int xs[8], n = 0;
    struct Ctx {
        int *xs, *n;
    } c{xs, &n};
    core::display::TileRowLeft(
        838, 0, 1678,
        [](int x, void *p) {
            auto *c = static_cast<Ctx *>(p);
            c->xs[(*c->n)++] = x;
        },
        &c);
    Check(n == 3 && xs[0] == 840 && xs[1] == 2 && xs[2] == -836, "a row tiled from 1678 to the left: 840, 2, -836");
}

// Every pixel of the panel covered, each tile inside it, the corners and edges where inner_panel_draw has them.
void Inset(int w, int h)
{
    g_placed.clear();
    const RECT a{100, 50, 100 + w, 50 + h};
    const bool drawn = core::display::InsetPanel(a, &Place, nullptr);
    if (w < 32 || h < 32) {
        Check(!drawn && g_placed.empty(), "smaller than 32 x 32: nothing");
        return;
    }
    std::vector<int> cover(static_cast<size_t>(w) * h, -1); // the last tile drawn at each pixel
    bool inside = true;
    for (const Placed &p : g_placed) {
        inside = inside && p.x >= a.left && p.y >= a.top && p.x + 16 <= a.right && p.y + 16 <= a.bottom;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const int px = p.x - a.left + x, py = p.y - a.top + y;
                if (px >= 0 && py >= 0 && px < w && py < h) {
                    cover[static_cast<size_t>(py) * w + px] = p.k;
                }
            }
        }
    }
    bool full = true;
    for (int v : cover) {
        full = full && v >= 0;
    }
    char what[96];
    std::snprintf(what, sizeof(what), "%dx%d: every pixel covered, every tile inside", w, h);
    Check(drawn && full && inside, what);
    auto k = [&](int x, int y) { return cover[static_cast<size_t>(y) * w + x]; };
    std::snprintf(what, sizeof(what), "%dx%d: corners 0, 6, 42, 48 and the edges on top", w, h);
    Check(k(0, 0) == 0 && k(w - 1, 0) == 6 && k(0, h - 1) == 42 && k(w - 1, h - 1) == 48 && k(w / 2, 0) / 7 == 0 &&
              k(w / 2, h - 1) / 7 == 6 && k(0, h / 2) % 7 == 0 && k(w - 1, h / 2) % 7 == 6,
          what);
    if (w >= 48 && h >= 48) {
        std::snprintf(what, sizeof(what), "%dx%d: the interior from the middle tiles", w, h);
        Check(k(20, 20) == 8 && k(w / 2, h / 2) % 7 >= 1 && k(w / 2, h / 2) % 7 <= 5, what);
    }
}

void Mirror()
{
    Screen sc(64, 40);
    const WORD src[3 * 2] = {1, 2, 3, 4, 5, 6}; // 3 x 2
    core::display::MirrorTile(sc.s, src, 3, 2, RECT{10, 5, 19, 11});
    // Columns of tiles: 0 as is, 1 mirrored, 2 as is; rows of tiles: 0 as is, 1 flipped, 2 as is.
    const WORD row0[9] = {1, 2, 3, 3, 2, 1, 1, 2, 3}, row2[9] = {4, 5, 6, 6, 5, 4, 4, 5, 6};
    bool ok = true;
    for (int x = 0; x < 9; ++x) {
        ok = ok && sc.At(10 + x, 5) == row0[x] && sc.At(10 + x, 6) == row2[x]; // tile row 0: rows 0, 1
        ok = ok && sc.At(10 + x, 7) == row2[x] && sc.At(10 + x, 8) == row0[x]; // tile row 1 flipped: rows 1, 0
        ok = ok && sc.At(10 + x, 9) == row0[x] && sc.At(10 + x, 10) == row2[x];
    }
    Check(ok, "mirrored tiles: neighbours meet on the same pixels");
    Check(sc.At(9, 5) == 5 * 7 + 9 && sc.At(19, 5) == 5 * 7 + 19 && sc.At(10, 4) == 4 * 7 + 10 &&
              sc.At(10, 11) == 11 * 7 + 10,
          "nothing outside the area");
    Screen edge(16, 8);
    core::display::MirrorTile(edge.s, src, 3, 2, RECT{10, 4, 16, 8}); // to the surface's last pixel
    Check(edge.At(15, 7) == 1 + 0, "up to the surface's last pixel (no write past it)");
    core::display::MirrorTile(edge.s, src, 3, 2, RECT{10, 4, 17, 8});
    Check(edge.At(10, 3) == 3 * 7 + 10, "one column past the surface: refused");
}

void Moves()
{
    Screen sc(32, 64);
    core::display::CopyRect(sc.s, RECT{4, 10, 12, 30}, 4, 20); // overlapping, down
    bool ok = true;
    for (int y = 0; y < 20; ++y) {
        ok = ok && sc.At(4, 20 + y) == (10 + y) * 7 + 4 && sc.At(11, 20 + y) == (10 + y) * 7 + 11;
    }
    Check(ok && sc.At(3, 25) == 25 * 7 + 3 && sc.At(12, 25) == 25 * 7 + 12, "an overlapping block moved down");
    Screen up(32, 64);
    core::display::CopyRect(up.s, RECT{0, 30, 32, 64}, 0, 20); // overlapping, up, to the last row
    Check(up.At(0, 20) == 30 * 7 && up.At(31, 53) == 63 * 7 + 31, "an overlapping block moved up (from the last row)");
    Screen past(32, 64);
    core::display::CopyRect(past.s, RECT{0, 30, 32, 64}, 0, 31);
    Check(past.At(0, 31) == 31 * 7, "a move ending past the surface: refused");
    Screen rep(16, 64);
    core::display::RepeatRows(rep.s, RECT{0, 50, 7, 54}, 10, 20); // a 4-row band down rows 10..19
    ok = true;
    for (int y = 10; y < 20; ++y) {
        ok = ok && rep.At(0, y) == (50 + (y - 10) % 4) * 7 && rep.At(6, y) == (50 + (y - 10) % 4) * 7 + 6;
    }
    Check(ok && rep.At(7, 12) == 12 * 7 + 7 && rep.At(0, 9) == 9 * 7 && rep.At(0, 20) == 20 * 7,
          "a band of rows repeated, only its columns and rows");
}

} // namespace

int main()
{
    Allowance();
    const int sizes[][2] = {{179, 312}, {179, 298}, {179, 672}, {32, 32}, {33, 47}, {48, 48}, {31, 100}, {100, 31},
                            {16, 16}, {179, 32}, {200, 200}};
    for (const auto &s : sizes) {
        Inset(s[0], s[1]);
    }
    Mirror();
    Moves();
    std::printf("  %d cases, %d failed\n  RESULT: %s (%d failure(s))\n", g_cases, g_failures,
                g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
