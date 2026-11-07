#include "core/capture/png_stream.h"

#include "core/capture/png_encode.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace core::capture {

namespace {

constexpr std::size_t kChunk = 256 * 1024; // IDAT data per chunk

void Be32(std::uint8_t *p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

} // namespace

PngWriter::~PngWriter()
{
    Abort();
}

bool PngWriter::Fail(const char *what)
{
    if (!error_[0]) {
        std::snprintf(error_, sizeof(error_), "%s (error %lu)", what, GetLastError());
    }
    Abort();
    return false;
}

void PngWriter::Abort()
{
    core_deflate_end(deflate_);
    deflate_ = nullptr;
    if (file_) {
        CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
        DeleteFileW(part_);
    }
    std::free(line_);
    std::free(chunk_);
    line_ = nullptr;
    chunk_ = nullptr;
}

bool PngWriter::Put(const void *data, std::size_t size)
{
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(file_), data, static_cast<DWORD>(size), &written, nullptr) ||
        written != size) {
        return false;
    }
    bytes_ += size;
    return true;
}

// The chunk's data is in chunk_ + 8; its head (length, "IDAT") and its CRC around it.
bool PngWriter::FlushChunk()
{
    if (chunkUsed_ == 0) {
        return true;
    }
    Be32(chunk_, static_cast<std::uint32_t>(chunkUsed_));
    std::memcpy(chunk_ + 4, "IDAT", 4);
    Be32(chunk_ + 8 + chunkUsed_, static_cast<std::uint32_t>(core_crc32(0, chunk_ + 4, chunkUsed_ + 4)));
    const bool ok = Put(chunk_, chunkUsed_ + 12);
    chunkUsed_ = 0;
    return ok;
}

int PngWriter::Sink(const void *data, int size, void *self)
{
    auto *w = static_cast<PngWriter *>(self);
    const auto *p = static_cast<const std::uint8_t *>(data);
    while (size > 0) {
        std::size_t room = kChunk - w->chunkUsed_;
        std::size_t n = static_cast<std::size_t>(size) < room ? static_cast<std::size_t>(size) : room;
        std::memcpy(w->chunk_ + 8 + w->chunkUsed_, p, n);
        w->chunkUsed_ += n;
        p += n;
        size -= static_cast<int>(n);
        if (w->chunkUsed_ == kChunk && !w->FlushChunk()) {
            return 0;
        }
    }
    return 1;
}

bool PngWriter::Open(const wchar_t *path, int w, int h, bool alpha, unsigned level)
{
    Abort();
    error_[0] = 0;
    bytes_ = 0;
    rows_ = 0;
    chunkUsed_ = 0;
    if (w <= 0 || h <= 0 || w > 0x7fffffff / 4 - 1) {
        return Fail("bad size");
    }
    w_ = w;
    h_ = h;
    channels_ = alpha ? 4 : 3;
    std::swprintf(path_, sizeof(path_) / sizeof(path_[0]), L"%ls", path);
    std::swprintf(part_, sizeof(part_) / sizeof(part_[0]), L"%ls.part", path);
    line_ = static_cast<std::uint8_t *>(std::malloc(static_cast<std::size_t>(w) * channels_ + 1));
    chunk_ = static_cast<std::uint8_t *>(std::malloc(kChunk + 12));
    if (!line_ || !chunk_) {
        return Fail("no memory");
    }
    HANDLE f = CreateFileW(part_, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return Fail("cannot create the file");
    }
    file_ = f;
    std::uint8_t head[33] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    Be32(head + 16, static_cast<std::uint32_t>(w));
    Be32(head + 20, static_cast<std::uint32_t>(h));
    head[24] = 8;                   // bits per channel
    head[25] = alpha ? 6 : 2;       // RGBA / RGB
    head[26] = head[27] = head[28] = 0; // deflate, adaptive filters, no interlace
    Be32(head + 29, static_cast<std::uint32_t>(core_crc32(0, head + 12, 17)));
    if (!Put(head, sizeof(head))) {
        return Fail("cannot write the file");
    }
    deflate_ = core_deflate_begin(level, &Sink, this);
    if (!deflate_) {
        return Fail("no memory for the compressor");
    }
    return true;
}

bool PngWriter::Row(const std::uint8_t *pixels)
{
    if (!deflate_ || rows_ >= h_) {
        return Fail("row out of place");
    }
    // Sub: each byte minus the same channel of the pixel to its left
    const std::size_t n = static_cast<std::size_t>(w_) * channels_;
    line_[0] = 1;
    std::uint8_t *out = line_ + 1;
    std::memcpy(out, pixels, channels_);
    for (std::size_t i = channels_; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(pixels[i] - pixels[i - channels_]);
    }
    if (!core_deflate_add(deflate_, line_, n + 1, 0)) {
        return Fail("compressing failed");
    }
    ++rows_;
    return true;
}

bool PngWriter::Close()
{
    if (!deflate_ || rows_ != h_) {
        return Fail("not all rows were given");
    }
    if (!core_deflate_add(deflate_, nullptr, 0, 1) || !FlushChunk()) {
        return Fail("compressing failed");
    }
    core_deflate_end(deflate_);
    deflate_ = nullptr;
    static const std::uint8_t end[12] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82};
    if (!Put(end, sizeof(end))) {
        return Fail("cannot write the file");
    }
    HANDLE f = static_cast<HANDLE>(file_);
    file_ = nullptr;
    const bool closed = CloseHandle(f) != 0;
    if (!closed || !MoveFileExW(part_, path_, MOVEFILE_REPLACE_EXISTING)) {
        std::snprintf(error_, sizeof(error_), "cannot finish the file (error %lu)", GetLastError());
        DeleteFileW(part_);
        Abort();
        return false;
    }
    Abort(); // frees the buffers (nothing open any more)
    return true;
}

void Rgb565ToRgb(const std::uint16_t *row, int w, int scale, bool alpha, std::uint16_t key, std::uint8_t *out)
{
    if (scale < 1) {
        scale = 1;
    }
    for (int x = 0; x < w; ++x) {
        const std::uint16_t c = row[x];
        std::uint8_t px[4];
        const unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31; // the bits repeated, as SDL expands them
        px[0] = static_cast<std::uint8_t>(r << 3 | r >> 2);
        px[1] = static_cast<std::uint8_t>(g << 2 | g >> 4);
        px[2] = static_cast<std::uint8_t>(b << 3 | b >> 2);
        px[3] = 255;
        if (alpha && c == key) {
            px[0] = px[1] = px[2] = px[3] = 0;
        }
        for (int k = 0; k < scale; ++k) {
            std::memcpy(out, px, alpha ? 4 : 3);
            out += alpha ? 4 : 3;
        }
    }
}

} // namespace core::capture
