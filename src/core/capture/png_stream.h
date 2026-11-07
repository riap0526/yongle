// A PNG file written a row at a time (city picture, docs/archive/city_export.md section 4.4): only the
// compressor's state and one chunk of output are in memory, however large the picture. The file is written as
// <path>.part and renamed to <path> by Close, so a picture that fails part way leaves no PNG behind. 8 bits per
// channel, RGB or RGBA, the Sub filter on every row. Any thread (one writer per file). Nothing here knows a game.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core::capture {

class PngWriter {
public:
    PngWriter() = default;
    PngWriter(const PngWriter &) = delete;
    PngWriter &operator=(const PngWriter &) = delete;
    ~PngWriter();

    // Creates <path>.part and writes the header. level: miniz 0..10. False (Error() says why) on failure.
    bool Open(const wchar_t *path, int w, int h, bool alpha, unsigned level);
    // One row of w pixels, 3 (RGB) or 4 (RGBA) bytes each, top to bottom; exactly h rows before Close.
    bool Row(const std::uint8_t *pixels);
    // Finishes the file and renames it to path (replacing a file of that name). False on failure (nothing kept).
    bool Close();
    // Gives up: the .part file is deleted.
    void Abort();

    const char *Error() const
    {
        return error_;
    }
    std::uint64_t Bytes() const
    {
        return bytes_;
    }

private:
    bool Put(const void *data, std::size_t size);
    bool FlushChunk();
    bool Fail(const char *what);
    static int Sink(const void *data, int size, void *self);

    void *file_ = nullptr; // HANDLE
    void *deflate_ = nullptr;
    wchar_t path_[1024] = {};
    wchar_t part_[1040] = {};
    int w_ = 0, h_ = 0, channels_ = 3, rows_ = 0;
    std::uint8_t *line_ = nullptr; // filter byte + the filtered row
    std::uint8_t *chunk_ = nullptr; // IDAT being filled (after its 8 byte head)
    std::size_t chunkUsed_ = 0;
    std::uint64_t bytes_ = 0;
    char error_[96] = "";
};

// The same rows from RGB565 (the game's pixels): out gets w pixels of RGB (alpha false) or RGBA. With alpha, pixels
// equal to key are fully transparent (black), all others opaque. scale >= 1 repeats each pixel scale times (nearest);
// out then holds w x scale pixels.
void Rgb565ToRgb(const std::uint16_t *row, int w, int scale, bool alpha, std::uint16_t key, std::uint8_t *out);

} // namespace core::capture
