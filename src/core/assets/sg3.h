// Reading images from the player's own .sg3/.555 image packs (the format of Impressions' city builders: Caesar 3,
// Pharaoh, Zeus, Emperor), in memory only: nothing decoded is ever written out (CLAUDE.md: the game's art is the
// publisher's; this reads the user's own files on the user's computer, as the game itself does).
//
// Layout (tools/mockups/sgimg.py, from which this is ported; field meanings as in Akhenaten's imagepak.cpp, read for
// reference only): an 80-byte header (version at +4, last entry index at +16), 300 group ids, bitmap names, then
// the entries from byte 40680, 64 bytes each (72 from version 214): data offset, length, uncompressed length, -,
// mirror (5 x int32), width and height (int16 at +20), type (+50), compressed (+51), external (+52). Pixels are
// 16-bit 555 in the .555 file next to the .sg3: either plain rows, or RLE where 0xff n skips n transparent pixels
// and any other byte n is followed by n pixels. 0x7c1f (magenta) is transparent, and so is 0xf81f (the font
// pack's plain glyphs; bit 15 is unused in 555, so it is never a colour).
//
// Only the plain (non-isometric, internal) images needed so far are decoded. Nothing here knows a game.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace core::assets {

class Sg3Pack {
public:
    // Reads the .sg3 index (the .555 pixel file is read per image). False (logged) when it cannot be read.
    bool Open(const wchar_t *sg3Path);
    int Count() const { return static_cast<int>(entries_.size()); }
    bool Size(int index, int *w, int *h) const;
    // Decodes image `index` into w x h RGB565 pixels (out has w * h entries); transparent pixels become `transparent`.
    // False (logged) when the image is not a plain internal one or its data cannot be read.
    bool Decode565(int index, WORD transparent, std::vector<WORD> *out) const;

private:
    struct Entry {
        int offset, length, w, h;
        BYTE type, compressed, external;
    };
    std::vector<Entry> entries_;
    wchar_t pixels_[MAX_PATH] = L"";
};

} // namespace core::assets
