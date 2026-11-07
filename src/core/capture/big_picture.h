// A large RGB565 picture (city picture, docs/archive/city_export.md section 4.3): its rows in blocks allocated
// one by one, so no single large piece of the address space is needed (the game is a 32-bit program without
// LARGE_ADDRESS_AWARE: 2 GB in all), and written as a PNG a row at a time (core/capture/png_stream.h). Nothing here
// knows a game.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core::capture {

class BigPicture {
public:
    static constexpr int kBlockRows = 256;

    // nullptr (logged) when the memory cannot be had; the picture starts black (all zero).
    static BigPicture *Create(int w, int h);
    ~BigPicture();
    BigPicture(const BigPicture &) = delete;
    BigPicture &operator=(const BigPicture &) = delete;

    int Width() const
    {
        return w_;
    }
    int Height() const
    {
        return h_;
    }
    std::uint16_t *Row(int y)
    {
        return blocks_[y / kBlockRows] + static_cast<std::size_t>(y % kBlockRows) * w_;
    }
    const std::uint16_t *Row(int y) const
    {
        return blocks_[y / kBlockRows] + static_cast<std::size_t>(y % kBlockRows) * w_;
    }
    std::size_t Bytes() const
    {
        return static_cast<std::size_t>(w_) * h_ * 2;
    }
    // Frees the rows of the blocks before y (written already).
    void FreeBefore(int y);

private:
    BigPicture() = default;
    int w_ = 0, h_ = 0, count_ = 0;
    std::uint16_t **blocks_ = nullptr;
};

struct BigOptions {
    // The file name's first part: <kind>_<w>x<h>_<time>.png in shots\ (letters, digits, '-', '_'; up to 63
    // characters). Read only during SaveBig.
    const char *kind = "city";
    int scale = 1;              // 1..8: each pixel repeated (nearest)
    bool transparent = false;   // pixels equal to key become transparent (an RGBA file)
    std::uint16_t key = 0;
    unsigned level = 1;         // miniz level (1: fast; the picture is written while the game runs)
    // Called on the writer thread when done: ok, and the file's name as shown to the player (shots\city_...png) or
    // what went wrong.
    void (*done)(bool ok, const char *text) = nullptr;
};

// Writes the picture as a PNG to path (the calling thread). False with error filled when it failed (nothing left).
bool WriteBig(BigPicture &pic, const wchar_t *path, const BigOptions &o, char *error, std::size_t size,
              bool freeAsWritten);

} // namespace core::capture
