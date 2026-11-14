#include "core/display/city_layout.h"

#include "core/ddraw/surface_memory.h"
#include "core/display/moved_rect.h"
#include "core/log.h"

#include <cstring>

namespace core::display {

int SpareRowsAllowance(int clipBottom, int shownBottom, int halfH, int screenH)
{
    int reach = clipBottom;
    if (shownBottom > clipBottom && halfH > 0) {
        reach = clipBottom + (shownBottom - clipBottom + halfH - 1) / halfH * halfH;
    }
    int allowance = reach > screenH ? reach - screenH : 0;
    if (halfH > 0 && allowance > 2 * halfH) {
        allowance = 2 * halfH; // not a clip of this mode
    }
    return allowance;
}

void ApplySpareRows(const char *prefix, int clipBottom, int shownBottom, int halfH, int screenH)
{
    static int last = -1;
    const int allowance = SpareRowsAllowance(clipBottom, shownBottom, halfH, screenH);
    core::ddraw::SetCanaryAllowance(allowance);
    if (allowance != last) {
        last = allowance;
        core_log("%sthe city may draw %d row(s) below the screen (spare rows; city clip bottom %d, shown to %d)", prefix,
                 allowance, clipBottom, shownBottom ? shownBottom : clipBottom);
    }
}

void TileRowLeft(int imageW, int left, int right, void (*draw)(int x, void *ctx), void *ctx)
{
    if (imageW <= 0) {
        return;
    }
    for (int x = right - imageW; x > left - imageW; x -= imageW) {
        draw(x, ctx);
    }
}

namespace {

constexpr int kTile = 16;

int TileAt(int r, int c, int lastR, int lastC)
{
    if (r == 0) {
        return c == 0 ? 0 : c == lastC ? 6 : 1 + (c - 1) % 5;
    }
    if (r == lastR) {
        return c == 0 ? 42 : c == lastC ? 48 : 43 + (c - 1) % 5;
    }
    const int band = 7 * (1 + (r - 1) % 5);
    return c == 0 ? band : c == lastC ? band + 6 : band + 1 + (c - 1) % 5;
}

// Inside the surface, which is consistent.
bool Fits(const Surface16 &s, const RECT &r)
{
    return s.px && s.w > 0 && s.h > 0 && s.pitch >= s.w * 2 &&
           s.bytes >= static_cast<size_t>(s.pitch) * static_cast<size_t>(s.h - 1) + static_cast<size_t>(s.w) * 2 &&
           r.left >= 0 && r.top >= 0 && r.right <= s.w && r.bottom <= s.h && r.left < r.right && r.top < r.bottom;
}

WORD *Row(const Surface16 &s, int y)
{
    return reinterpret_cast<WORD *>(s.px + static_cast<size_t>(y) * static_cast<size_t>(s.pitch));
}

} // namespace

bool InsetPanel(const RECT &area, void (*draw)(int k, int x, int y, void *ctx), void *ctx)
{
    const int w = area.right - area.left, h = area.bottom - area.top;
    if (w < 2 * kTile || h < 2 * kTile) {
        return false;
    }
    const int cols = (w - 1) / kTile + 1, rows = (h - 1) / kTile + 1; // the last ones at right - 16, bottom - 16
    const int lastC = cols - 1, lastR = rows - 1;
    auto x = [&](int c) { return c == lastC ? static_cast<int>(area.right) - kTile : static_cast<int>(area.left) + c * kTile; };
    auto y = [&](int r) { return r == lastR ? static_cast<int>(area.bottom) - kTile : static_cast<int>(area.top) + r * kTile; };
    for (int r = 1; r < lastR; ++r) { // the interior
        for (int c = 1; c < lastC; ++c) {
            draw(TileAt(r, c, lastR, lastC), x(c), y(r), ctx);
        }
    }
    for (int r = 1; r < lastR; ++r) { // the left and right edges
        draw(TileAt(r, 0, lastR, lastC), x(0), y(r), ctx);
        draw(TileAt(r, lastC, lastR, lastC), x(lastC), y(r), ctx);
    }
    for (int c = 0; c < cols; ++c) { // the top and bottom edges with the corners
        draw(TileAt(0, c, lastR, lastC), x(c), y(0), ctx);
        draw(TileAt(lastR, c, lastR, lastC), x(c), y(lastR), ctx);
    }
    return true;
}

void MirrorTile(const Surface16 &s, const WORD *px, int w, int h, const RECT &area)
{
    if (!px || w <= 0 || h <= 0 || !Fits(s, area)) {
        return;
    }
    for (int y = area.top; y < area.bottom; ++y) {
        const int ty = (y - area.top) / h, iy = (y - area.top) % h;
        const WORD *src = px + static_cast<size_t>(ty % 2 ? h - 1 - iy : iy) * static_cast<size_t>(w);
        WORD *dst = Row(s, y);
        for (int x = area.left; x < area.right; ++x) {
            const int tx = (x - area.left) / w, ix = (x - area.left) % w;
            dst[x] = src[tx % 2 ? w - 1 - ix : ix];
        }
    }
}

void CopyRect(const Surface16 &s, const RECT &from, int toX, int toY)
{
    const RECT to{toX, toY, toX + (from.right - from.left), toY + (from.bottom - from.top)};
    if (!Fits(s, from) || !Fits(s, to)) {
        return;
    }
    const size_t bytes = static_cast<size_t>(from.right - from.left) * 2;
    const int rows = from.bottom - from.top;
    const bool down = toY > from.top; // copy the last row first, so that an overlap is not read after it is written
    for (int i = 0; i < rows; ++i) {
        const int r = down ? rows - 1 - i : i;
        std::memmove(Row(s, toY + r) + toX, Row(s, from.top + r) + from.left, bytes);
    }
}

void RepeatRows(const Surface16 &s, const RECT &band, int top, int bottom)
{
    const RECT area{band.left, top, band.right, bottom};
    const int period = band.bottom - band.top;
    if (period <= 0 || !Fits(s, band) || !Fits(s, area)) {
        return;
    }
    const size_t bytes = static_cast<size_t>(band.right - band.left) * 2;
    for (int y = top; y < bottom; ++y) {
        const int from = band.top + (y - top) % period;
        if (from != y) {
            std::memmove(Row(s, y) + band.left, Row(s, from) + band.left, bytes);
        }
    }
}

} // namespace core::display
