// Memory of an emulated DirectDraw surface (R2b.1, docs/archive/resolution_ui_r2b1.md): the rows the game sees, then spare
// rows filled with a canary pattern, with a guard page (reserved, never committed) before and after:
//
//   [guard page][rows 0 .. rows-1][spare rows (canary) ... to the page end][guard page]
//
// The game gets the same pointer, pitch and height as before; nothing it reads changes. A write a little past the
// last row (the original game's city clip passes the screen's bottom in small-tile mode: 775 > 768 at 1024x768)
// lands in the spare rows instead of whatever memory follows, and CheckCanary reports it; anything further, or
// before row 0, hits a guard page (an access violation, logged by core/diag). Nothing here knows a game.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core::ddraw {

constexpr int kSpareRows = 64; // more than one row of city tiles (40 px) below the last row

struct SurfaceMemory {
    std::uint8_t *base = nullptr; // the whole reservation (guard pages included)
    std::size_t size = 0;
    std::uint8_t *bits = nullptr; // row 0
    int pitch = 0, rows = 0;
    std::size_t spareBytes = 0; // from bits + pitch * rows to the committed end (whole spare rows and the page rest)

    std::uint8_t *Spare() const { return bits + static_cast<std::size_t>(pitch) * rows; }
};

// pitch > 0 and a multiple of 4, rows > 0. False (m left empty) when the memory cannot be had.
bool AllocSurfaceMemory(SurfaceMemory &m, int pitch, int rows, int spareRows = kSpareRows);
void FreeSurfaceMemory(SurfaceMemory &m);

// What was written into the spare area: rows counted from row 0 of the surface (so >= rows), and how many bytes
// differ from the pattern.
struct CanaryDamage {
    int firstRow = 0, lastRow = 0;
    std::size_t bytes = 0;
};

// True when the spare area still holds the pattern. Otherwise fills damage and writes the pattern again (the next
// write is reported anew). The first SetCanaryAllowance rows are not checked.
bool CheckCanary(const SurfaceMemory &m, CanaryDamage *damage);

// The spare rows a game draws into on purpose (e.g. a city clip that passes the screen's bottom by a few rows: the
// original game's small tiles, 775 > 768 at 1024x768; R2b.1-5): writes there are expected, so the canary is checked
// only below them. Any thread; 0 (the default) checks all spare rows.
void SetCanaryAllowance(int rows);
int CanaryAllowance();

// The game layer describes its state for a canary report (e.g. the screen and the city's clip); game thread.
// nullptr removes it.
using StateDescriber = void (*)(char *out, std::size_t size);
void SetStateDescriber(StateDescriber describe);
void DescribeState(char *out, std::size_t size); // "" without a describer

} // namespace core::ddraw
