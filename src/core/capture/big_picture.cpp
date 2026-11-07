#include "core/capture/big_picture.h"

#include "core/capture/png_stream.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace core::capture {

BigPicture *BigPicture::Create(int w, int h)
{
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768) {
        core_log("picture: %d x %d is not a size we can make", w, h);
        return nullptr;
    }
    auto *p = new (std::nothrow) BigPicture;
    const int count = (h + kBlockRows - 1) / kBlockRows;
    auto **blocks = p ? static_cast<std::uint16_t **>(std::calloc(count, sizeof(std::uint16_t *))) : nullptr;
    if (!p || !blocks) {
        delete p;
        core_log("picture: no memory for %d x %d", w, h);
        return nullptr;
    }
    p->w_ = w;
    p->h_ = h;
    p->count_ = count;
    p->blocks_ = blocks;
    for (int i = 0; i < count; ++i) {
        const int rows = i + 1 < count ? kBlockRows : h - i * kBlockRows;
        const std::size_t bytes = static_cast<std::size_t>(w) * rows * 2;
        blocks[i] = static_cast<std::uint16_t *>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!blocks[i]) {
            core_log("picture: no memory for %d x %d (block %d of %d, %.1f MB in all)", w, h, i + 1, count,
                     static_cast<double>(w) * h * 2 / (1024.0 * 1024.0));
            delete p;
            return nullptr;
        }
    }
    return p;
}

BigPicture::~BigPicture()
{
    FreeBefore(h_);
    std::free(blocks_);
}

void BigPicture::FreeBefore(int y)
{
    for (int i = 0; i < count_; ++i) {
        const int end = (i + 1) * kBlockRows < h_ ? (i + 1) * kBlockRows : h_; // the block's rows end here
        if (end <= y && blocks_[i]) {
            VirtualFree(blocks_[i], 0, MEM_RELEASE);
            blocks_[i] = nullptr;
        }
    }
}

bool WriteBig(BigPicture &pic, const wchar_t *path, const BigOptions &o, char *error, std::size_t size,
              bool freeAsWritten)
{
    const int scale = o.scale < 1 ? 1 : o.scale > 8 ? 8 : o.scale;
    const int w = pic.Width() * scale, h = pic.Height() * scale;
    PngWriter png;
    auto *line = static_cast<std::uint8_t *>(std::malloc(static_cast<std::size_t>(w) * 4));
    bool ok = line && png.Open(path, w, h, o.transparent, o.level);
    for (int y = 0; ok && y < pic.Height(); ++y) {
        Rgb565ToRgb(pic.Row(y), pic.Width(), scale, o.transparent, o.key, line);
        for (int k = 0; ok && k < scale; ++k) {
            ok = png.Row(line);
        }
        if (freeAsWritten && (y + 1) % BigPicture::kBlockRows == 0) {
            pic.FreeBefore(y + 1);
        }
    }
    ok = ok && png.Close();
    if (!ok) {
        std::snprintf(error, size, "%s", !line ? "no memory" : png.Error()[0] ? png.Error() : "failed");
        png.Abort();
    }
    std::free(line);
    return ok;
}

} // namespace core::capture
