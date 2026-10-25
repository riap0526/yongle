#include "core/display/moved_rect.h"

#include <cstring>

namespace core::display {

namespace {

bool Inside(const RECT &r, int w, int h)
{
    return r.left >= 0 && r.top >= 0 && r.left < r.right && r.top < r.bottom && r.right <= w && r.bottom <= h;
}

BYTE *Row(BYTE *base, int pitch, int y)
{
    return base + static_cast<size_t>(y) * static_cast<size_t>(pitch);
}

// The rows y0..y1 (exclusive) of columns x0..x1 (exclusive) from src to dst at (dx, dy); all checked by the caller.
void Copy(BYTE *dst, const BYTE *src, int pitch, const RECT &s, int dx, int dy)
{
    const size_t bytes = static_cast<size_t>(s.right - s.left) * 2;
    for (int y = s.top; y < s.bottom; ++y) {
        std::memcpy(Row(dst, pitch, y + dy) + (s.left + dx) * 2, Row(const_cast<BYTE *>(src), pitch, y) + s.left * 2,
                    bytes);
    }
}

} // namespace

bool MovedRectValid(const Surface16 &s, const RECT &from, const RECT &to)
{
    if (!s.px || s.w <= 0 || s.h <= 0 || s.pitch < s.w * 2 ||
        s.bytes < static_cast<size_t>(s.pitch) * (s.h - 1) + static_cast<size_t>(s.w) * 2) {
        return false;
    }
    return Inside(from, s.w, s.h) && Inside(to, s.w, s.h) && from.right - from.left == to.right - to.left &&
           from.bottom - from.top == to.bottom - to.top;
}

bool MovedRectPrepare(BYTE *copy, const Surface16 &back, const RECT &from, const RECT &to)
{
    if (!copy || !MovedRectValid(back, from, to)) {
        return false;
    }
    for (int y = 0; y < back.h; ++y) {
        std::memcpy(Row(copy, back.pitch, y), Row(back.px, back.pitch, y), static_cast<size_t>(back.w) * 2);
    }
    Copy(copy, back.px, back.pitch, to, from.left - to.left, from.top - to.top);
    return true;
}

bool MovedRectCompose(const Surface16 &back, const BYTE *copy, const RECT &from, const RECT &to)
{
    if (!copy || !MovedRectValid(back, from, to)) {
        return false;
    }
    for (int y = 0; y < back.h; ++y) {
        BYTE *dst = Row(back.px, back.pitch, y);
        const BYTE *src = Row(const_cast<BYTE *>(copy), back.pitch, y);
        if (y < from.top || y >= from.bottom) {
            std::memcpy(dst, src, static_cast<size_t>(back.w) * 2);
        }
        else {
            std::memcpy(dst, src, static_cast<size_t>(from.left) * 2);
            std::memcpy(dst + from.right * 2, src + from.right * 2, static_cast<size_t>(back.w - from.right) * 2);
        }
    }
    Copy(back.px, copy, back.pitch, from, to.left - from.left, to.top - from.top);
    return true;
}

} // namespace core::display
