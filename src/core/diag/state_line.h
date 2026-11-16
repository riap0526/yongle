// The game's state in one log line, the same fields in the same order in every game: for the canary reports
// (core/ddraw/surface_memory.h SetStateDescriber) and the problem report (F12). The game layer reads its variables
// and fills a StateLine; Format writes it.
#pragma once

#include <cstddef>

namespace core::diag {

struct StateLine {
    // Every field is given; there are no defaults. tileW x tileH: a city tile's width and height in pixels (two half
    // rows). extra: what only this game has, at the end of the line ("" when it has nothing more).
    StateLine(int screen_, int modeW_, int modeH_, int clipLeft_, int clipTop_, int clipRight_, int clipBottom_,
              int viewCols_, int viewRows_, int tileW_, int tileH_, int dialogX_, int dialogY_, int gameSpeed_,
              const char *extra_)
        : screen(screen_), modeW(modeW_), modeH(modeH_), clipLeft(clipLeft_), clipTop(clipTop_),
          clipRight(clipRight_), clipBottom(clipBottom_), viewCols(viewCols_), viewRows(viewRows_), tileW(tileW_),
          tileH(tileH_), dialogX(dialogX_), dialogY(dialogY_), gameSpeed(gameSpeed_), extra(extra_)
    {
    }

    int screen;                                    // the game's screen number
    int modeW, modeH;                              // the display mode's size
    int clipLeft, clipTop, clipRight, clipBottom;  // the city's clip
    int viewCols, viewRows;                        // the city view in columns and half rows
    int tileW, tileH;
    int dialogX, dialogY;                          // where the 640 x 480 dialog area starts
    int gameSpeed;                                 // percent
    const char *extra;
};

// "screen 0x01, mode 2560x1440, city clip (0,40)-(2397,1440), view 41 x 96 (tiles 58x30), dialog offset (960,480),
// game speed 90%, <extra>"
void Format(const StateLine &s, char *out, std::size_t size);

} // namespace core::diag
