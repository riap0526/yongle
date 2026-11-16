#include "core/diag/state_line.h"

#include <cstdio>

namespace core::diag {

void Format(const StateLine &s, char *out, std::size_t size)
{
    std::snprintf(out, size,
                  "screen 0x%02x, mode %dx%d, city clip (%d,%d)-(%d,%d), view %d x %d (tiles %dx%d), dialog offset "
                  "(%d,%d), game speed %d%%%s%s",
                  static_cast<unsigned>(s.screen) & 0xff, s.modeW, s.modeH, s.clipLeft, s.clipTop, s.clipRight,
                  s.clipBottom, s.viewCols, s.viewRows, s.tileW, s.tileH, s.dialogX, s.dialogY, s.gameSpeed,
                  s.extra[0] ? ", " : "", s.extra);
}

} // namespace core::diag
