// Offline test of the city picture's parts (docs/archive/city_export.md; not the game):
//   core/display/city_strips: every picture row comes from exactly one strip, inside the strip's drawing (above its
//     clip's bottom, below the rows the drawing leaves out), views within the clamp and even, for map sizes of the
//     game, small and large strips, the room above the top row;
//   core/capture/png_stream + big_picture: pictures written a row at a time read back equal (SDL_LoadPNG, RGB and
//     RGBA with the transparent colour, a scale of 2), the .part file gone, a failed write leaves nothing.
#include "core/capture/big_picture.h"
#include "core/capture/png_stream.h"
#include "core/display/city_strips.h"

#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-88s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------------------------
// Strips

void Strips(int mapCols, int mapRows, int margin, int keep, int overlap, int top)
{
    using namespace core::display;
    StripInput in{};
    in.left = (231 - mapCols) / 2;
    in.right = in.left + mapCols;
    in.top = ((457 - mapRows) / 2) & ~1;
    in.mapRows = mapRows;
    in.bottom = in.top + mapRows - (margin ? margin + 4 : 0);
    in.colPx = 80;
    in.halfH = 20;
    in.keepRows = keep;
    in.overlapRows = overlap;
    in.topRows = top;
    StripPlan p{};
    char what[160];
    std::snprintf(what, sizeof(what), "map %d x %d margin %d, keep %d overlap %d top %d: planned", mapCols, mapRows,
                  margin, keep, overlap, top);
    const bool planned = PlanStrips(in, &p);
    Check(planned, what);
    if (!planned) {
        return;
    }
    const int shown = in.bottom - in.top - 1;
    bool sizes = p.imageW == mapCols * 80 - 2 && p.imageH == p.topPx + shown * 20 && p.cols == mapCols - 1 &&
                 p.cols + 1 <= mapCols && p.viewRows + 2 <= mapRows && p.viewRows <= in.bottom - in.top &&
                 p.clipRight == mapCols * 80 - 2 && p.clipBottom <= p.bufH && p.topPx == (top & ~1) * 20;
    std::vector<int> owner(p.imageH, 0);
    bool inside = true, views = true;
    for (int k = 0; k < p.count; ++k) {
        const Strip s = StripAt(in, p, k);
        views = views && (s.viewY & 1) == 0 && s.viewY >= in.top && s.viewY + p.viewRows <= in.bottom;
        // the rows the drawing completes: from the buffer's top (the first strip) or the origin; a strip with
        // another below keeps at least two half rows of its view below what it gives (drawn again by the next), the
        // last one gives at most its view
        const int lowest = k + 1 < p.count ? p.topPx + (p.viewRows - 2) * 20 : p.topPx + p.viewRows * 20;
        inside = inside && s.fromY >= (k == 0 ? 0 : p.topPx) && s.fromY + s.rows <= lowest && s.rows > 0;
        for (int y = 0; y < s.rows; ++y) {
            if (s.toY + y >= 0 && s.toY + y < p.imageH) {
                ++owner[s.toY + y];
            }
            else {
                inside = false;
            }
        }
    }
    bool once = true;
    for (int c : owner) {
        once = once && c == 1;
    }
    std::snprintf(what, sizeof(what), "  %d strip(s) of %d (view %d x %d), picture %d x %d: sizes", p.count,
                  p.keepRows, p.cols, p.viewRows, p.imageW, p.imageH);
    Check(sizes, what);
    Check(once, "  every picture row from exactly one strip");
    Check(inside, "  copied rows inside what each strip's drawing completes");
    Check(views, "  views even and within the clamp");
}

// ---------------------------------------------------------------------------------------------
// PNG

std::uint16_t Pixel(int x, int y)
{
    return static_cast<std::uint16_t>((x * 7 + y * 13) ^ (x * y));
}

bool RoundTrip(int w, int h, int scale, bool transparent, const char *name)
{
    core::capture::BigPicture *pic = core::capture::BigPicture::Create(w, h);
    if (!pic) {
        return false;
    }
    const std::uint16_t key = 0xF81F;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            pic->Row(y)[x] = (x + y) % 17 == 0 ? key : Pixel(x, y);
        }
    }
    wchar_t path[MAX_PATH];
    std::swprintf(path, MAX_PATH, L"%hs", name);
    core::capture::BigOptions o;
    o.scale = scale;
    o.transparent = transparent;
    o.key = key;
    char error[128] = "";
    bool ok = core::capture::WriteBig(*pic, path, o, error, sizeof(error), false);
    if (!ok) {
        std::printf("  write failed: %s\n", error);
    }
    wchar_t part[MAX_PATH + 8];
    std::swprintf(part, MAX_PATH + 8, L"%ls.part", path);
    ok = ok && GetFileAttributesW(part) == INVALID_FILE_ATTRIBUTES;
    SDL_Surface *s = ok ? SDL_LoadPNG(name) : nullptr;
    SDL_Surface *rgba = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888) : nullptr; // bytes R, G, B, A
    ok = ok && rgba && rgba->w == w * scale && rgba->h == h * scale;
    for (int y = 0; ok && y < h * scale; ++y) {
        const auto *row = static_cast<const std::uint8_t *>(rgba->pixels) + static_cast<size_t>(y) * rgba->pitch;
        for (int x = 0; ok && x < w * scale; ++x) {
            const std::uint16_t c = pic->Row(y / scale)[x / scale];
            std::uint8_t want[4];
            core::capture::Rgb565ToRgb(&c, 1, 1, true, transparent ? key : 0x0001, want); // 0x0001: not this colour
            if (!transparent) {
                want[3] = 255;
                core::capture::Rgb565ToRgb(&c, 1, 1, false, 0, want);
            }
            ok = std::memcmp(row + x * 4, want, transparent ? 4 : 3) == 0 && (transparent || row[x * 4 + 3] == 255);
        }
    }
    if (rgba) {
        SDL_DestroySurface(rgba);
    }
    if (s) {
        SDL_DestroySurface(s);
    }
    DeleteFileW(path);
    delete pic;
    return ok;
}

} // namespace

int main()
{
    // the game's maps (resolution_ui_r5.md section 8.2) with and without the bottom margin
    Strips(54, 112, 3, 64, 4, 8);
    Strips(68, 140, 0, 64, 4, 8);
    Strips(84, 168, 5, 64, 4, 8);
    Strips(112, 224, 2, 64, 4, 8);
    Strips(112, 224, 0, 1000, 4, 8); // as few strips as the map allows
    Strips(112, 224, 6, 8, 4, 8);    // the seam check: many strips
    Strips(84, 168, 5, 4, 2, 0);     // the smallest strips, no room above
    // as the game layer draws now: no room above the view's top row (city_export.md section 11.3)
    Strips(84, 168, 2, 64, 4, 0);
    Strips(112, 224, 2, 64, 4, 0);
    Strips(112, 224, 0, 1000, 4, 0);
    Strips(112, 224, 6, 8, 4, 0);
    {
        core::display::StripInput in{10, 11, 0, 100, 100, 80, 20, 64, 4, 8};
        core::display::StripPlan p{};
        Check(!core::display::PlanStrips(in, &p), "a map one column wide: nothing to draw");
    }

    Check(RoundTrip(300, 700, 1, false, "test_city_picture_rgb.png"), "PNG: 300 x 700 RGB (blocks of 256 rows) read back");
    Check(RoundTrip(123, 77, 2, true, "test_city_picture_rgba.png"), "PNG: 123 x 77 RGBA, scale 2, key transparent");
    Check(RoundTrip(1, 1, 4, false, "test_city_picture_one.png"), "PNG: 1 x 1, scale 4");
    {
        core::capture::PngWriter png;
        bool opened = png.Open(L"test_city_picture_short.png", 4, 4, false, 1);
        std::uint8_t row[12] = {};
        png.Row(row);
        const bool closed = png.Close(); // two rows short
        Check(opened && !closed && GetFileAttributesW(L"test_city_picture_short.png") == INVALID_FILE_ATTRIBUTES &&
                  GetFileAttributesW(L"test_city_picture_short.png.part") == INVALID_FILE_ATTRIBUTES,
              "PNG: rows missing at Close: no file left");
    }
    {
        core::capture::PngWriter png;
        Check(!png.Open(L"no_such_folder\\x.png", 4, 4, false, 1) && png.Error()[0], "PNG: a folder that is not "
                                                                                     "there: refused with a reason");
    }
    std::printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
