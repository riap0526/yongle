// Offline test of core/display/moved_rect (not the game): showing a rectangle of a 16-bit surface elsewhere. Every
// surface ends right before a PAGE_NOACCESS page (and starts right after one), so a copy past either end faults and
// the test fails. Results are compared pixel by pixel with a plain model. Cases: screen sizes 1024x768, 1280x1024,
// 1920x1080, 2560x1080, 2560x1440, each with rows exactly as wide as the screen and with padded rows; rectangles at
// every edge and corner, overlapping, equal; and rectangles one pixel past an edge, empty, negative or of different
// sizes, which must be refused without touching anything.
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

// A buffer of `bytes` that ends exactly at a no-access page and starts right after another one.
struct Guarded {
    BYTE *base = nullptr, *px = nullptr;
    size_t total = 0;
    explicit Guarded(size_t bytes)
    {
        const size_t page = 4096, body = (bytes + page - 1) / page * page;
        total = body + 2 * page;
        base = static_cast<BYTE *>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old;
        VirtualProtect(base, page, PAGE_NOACCESS, &old);
        VirtualProtect(base + page + body, page, PAGE_NOACCESS, &old);
        px = base + page + body - bytes; // the last byte is the last one before the no-access page
    }
    ~Guarded()
    {
        VirtualFree(base, 0, MEM_RELEASE);
    }
};

WORD At(const BYTE *p, int pitch, int x, int y)
{
    WORD v;
    std::memcpy(&v, p + static_cast<size_t>(y) * pitch + x * 2, 2);
    return v;
}

void Put(BYTE *p, int pitch, int x, int y, WORD v)
{
    std::memcpy(p + static_cast<size_t>(y) * pitch + x * 2, &v, 2);
}

bool In(const RECT &r, int x, int y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// One case: the screen holds a pattern; Prepare; the "game" draws its window at `from` and a mark outside; Compose;
// then every pixel is compared with the model.
void RunCase(int w, int h, int pad, RECT from, RECT to)
{
    const int pitch = w * 2 + pad;
    const size_t bytes = static_cast<size_t>(pitch) * (h - 1) + static_cast<size_t>(w) * 2; // no slack at the end
    Guarded back(bytes), copy(bytes);
    Surface16 s{back.px, pitch, w, h, bytes};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Put(back.px, pitch, x, y, static_cast<WORD>(x * 7 + y * 13));
        }
    }
    std::vector<WORD> before(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            before[static_cast<size_t>(y) * w + x] = At(back.px, pitch, x, y);
        }
    }
    char what[200];
    std::snprintf(what, sizeof(what), "%dx%d pad %d: (%ld,%ld)-(%ld,%ld) to (%ld,%ld): prepare", w, h, pad, from.left,
                  from.top, from.right, from.bottom, to.left, to.top);
    Check(core::display::MovedRectPrepare(copy.px, s, from, to), what);
    // The copy's `from` holds what is on screen at `to`.
    bool behind = true;
    for (int y = from.top; y < from.bottom && behind; ++y) {
        for (int x = from.left; x < from.right; ++x) {
            if (At(copy.px, pitch, x, y) != before[static_cast<size_t>(y + to.top - from.top) * w + x + to.left - from.left]) {
                behind = false;
                break;
            }
        }
    }
    Check(behind, "prepare: the window's place in the copy shows what is behind its new place");
    // The game draws its window (every pixel of `from`, some left as they were: "transparent") and one mark outside.
    for (int y = from.top; y < from.bottom; ++y) {
        for (int x = from.left; x < from.right; ++x) {
            if ((x + y) % 5 != 0) {
                Put(copy.px, pitch, x, y, static_cast<WORD>(0x8000 | ((x - from.left) * 3 + (y - from.top))));
            }
        }
    }
    int markX = -1, markY = -1;
    for (int y = 0; y < h && markX < 0; y += 7) {
        for (int x = 0; x < w; x += 11) {
            if (!In(from, x, y)) {
                markX = x;
                markY = y;
                Put(copy.px, pitch, x, y, 0x7777);
                break;
            }
        }
    }
    std::vector<WORD> drawn(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            drawn[static_cast<size_t>(y) * w + x] = At(copy.px, pitch, x, y);
        }
    }
    Check(core::display::MovedRectCompose(s, copy.px, from, to), "compose");
    // Model: at `to` the window (from the copy's `from`); elsewhere outside `from` what was drawn; inside `from` but
    // not `to`: the screen as it was.
    int wrong = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            WORD want;
            if (In(to, x, y)) {
                want = drawn[static_cast<size_t>(y - to.top + from.top) * w + x - to.left + from.left];
            }
            else if (!In(from, x, y)) {
                want = drawn[static_cast<size_t>(y) * w + x];
            }
            else {
                want = before[static_cast<size_t>(y) * w + x];
            }
            wrong += At(back.px, pitch, x, y) != want;
        }
    }
    std::snprintf(what, sizeof(what), "%dx%d pad %d: (%ld,%ld)-(%ld,%ld) to (%ld,%ld): %d pixels differ from the model", w,
                  h, pad, from.left, from.top, from.right, from.bottom, to.left, to.top, wrong);
    Check(wrong == 0, what);
    if (markX >= 0 && !In(to, markX, markY)) {
        Check(At(back.px, pitch, markX, markY) == 0x7777, "a mark drawn outside the window stays where it was drawn");
    }
}

// A refused case: nothing in either buffer changes.
void RunRefused(int w, int h, RECT from, RECT to, const char *why)
{
    const int pitch = w * 2;
    const size_t bytes = static_cast<size_t>(pitch) * h;
    Guarded back(bytes), copy(bytes);
    std::memset(back.px, 0x11, bytes);
    std::memset(copy.px, 0x22, bytes);
    Surface16 s{back.px, pitch, w, h, bytes};
    bool refused = !core::display::MovedRectValid(s, from, to) && !core::display::MovedRectPrepare(copy.px, s, from, to) &&
                   !core::display::MovedRectCompose(s, copy.px, from, to);
    bool untouched = true;
    for (size_t i = 0; i < bytes && untouched; ++i) {
        untouched = back.px[i] == 0x11 && copy.px[i] == 0x22;
    }
    char what[160];
    std::snprintf(what, sizeof(what), "%dx%d: refused and untouched: %s", w, h, why);
    Check(refused && untouched, what);
}

RECT R(int x, int y, int w, int h)
{
    return RECT{x, y, x + w, y + h};
}

} // namespace

int main()
{
    struct Size {
        int w, h;
    };
    const Size sizes[] = {{1024, 768}, {1280, 1024}, {1920, 1080}, {2560, 1080}, {2560, 1440}};
    for (const Size &z : sizes) {
        for (int pad : {0, 64}) {
            const int ww = 480, wh = 512; // the Warehouse window (0x1e x 0x20 blocks)
            const RECT orig = R(0, 0x2a, ww, wh);
            const RECT targets[] = {
                R(0, 0x2a, ww, wh),                         // not moved
                R((z.w - ww) / 2, (z.h - wh) / 2, ww, wh),  // centred
                R(z.w - ww, z.h - wh, ww, wh),              // bottom right corner
                R(z.w - ww, 0, ww, wh),                     // top right
                R(0, z.h - wh, ww, wh),                     // bottom left
                R(0, 0, ww, wh),                            // top left (overlaps the window's own place)
                R(17, 0x2a + 5, ww, wh),                    // overlapping, a few pixels off
            };
            for (const RECT &t : targets) {
                RunCase(z.w, z.h, pad, orig, t);
            }
            RunCase(z.w, z.h, pad, R(z.w - 1, z.h - 1, 1, 1), R(0, 0, 1, 1)); // one pixel, corner to corner
            RunCase(z.w, z.h, pad, R(0, 0, z.w, z.h), R(0, 0, z.w, z.h));     // the whole screen
        }
        RunRefused(z.w, z.h, R(0, 0x2a, 480, 512), R(z.w - 479, 0, 480, 512), "one pixel past the right edge");
        RunRefused(z.w, z.h, R(0, 0x2a, 480, 512), R(0, z.h - 511, 480, 512), "one pixel past the bottom");
        RunRefused(z.w, z.h, R(0, 0x2a, 480, 512), R(-1, 0, 480, 512), "left of the screen");
        RunRefused(z.w, z.h, R(0, 0x2a, 480, 512), R(0, -1, 480, 512), "above the screen");
        RunRefused(z.w, z.h, R(0, 0x2a, 0, 512), R(0, 0, 0, 512), "empty");
        RunRefused(z.w, z.h, R(0, 0x2a, 480, 512), R(0, 0, 481, 512), "different sizes");
    }
    // A surface that says it is larger than its memory, or rows narrower than the screen: refused.
    {
        Guarded back(1024 * 2 * 768), copy(1024 * 2 * 768);
        Surface16 small{back.px, 1024 * 2, 1024, 768, 1024 * 2 * 767};
        Surface16 narrow{back.px, 1000, 1024, 768, 1024 * 2 * 768};
        Check(!core::display::MovedRectValid(small, R(0, 0, 10, 10), R(5, 5, 10, 10)), "memory smaller than the rows");
        Check(!core::display::MovedRectValid(narrow, R(0, 0, 10, 10), R(5, 5, 10, 10)), "pitch narrower than the width");
    }
    std::printf("%d checks\nRESULT: %s (%d failure(s))\n", g_cases, g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
