// City picture (docs/archive/city_export.md sections 4.2, 9.2): the whole city the player can scroll to, drawn by
// the game in horizontal strips and put together. Each strip is one view as wide as the map's range (the game's
// drawing adds one column, so the whole range is covered at once), drawn into our buffer with the view's origin T
// pixels below the buffer's top (what stands above the map's top row still shows); only its top S half rows are kept,
// the next strip starts there, and the R half rows below them (overlap) are drawn again by it. Nothing here knows a
// game: the game layer passes the map's limits (those of its own view clamp) and the tile sizes in.
#pragma once

namespace core::display {

struct StripInput {
    int left, right;  // the view's column limits: view x from left to right - columns
    int top;          // the view's top limit (half rows, even)
    int bottom;       // the clamp's bottom limit: view y + view rows <= bottom
    int mapRows;      // the map's range in half rows (the view plus the drawing's 2 half rows must fit in it)
    int colPx, halfH; // a column step (tile width + 2) and a half row, in pixels
    int keepRows;     // S: half rows kept per strip (even, at least 2)
    int overlapRows;  // R: half rows drawn again by the next strip (at least 2)
    int topRows;      // T in half rows: room above the map's top row for what stands above it
};

struct StripPlan {
    int imageW, imageH;   // the picture
    int topPx;            // T in pixels (the view's origin in the buffer)
    int cols;             // the view's columns (the drawing adds one)
    int viewRows;         // the view's half rows (the drawing adds two)
    int keepRows;         // S as used (fewer when the map is small)
    int count;            // strips
    int bufW, bufH;       // the buffer one strip is drawn into (pixels; the pitch is up to the caller)
    int clipRight;        // the clip's right edge in the buffer (exclusive): the view's columns + 1
    int clipBottom;       // the clip's bottom (exclusive): the view's half rows + 2
};

struct Strip {
    int viewY;   // the view's top half row (even)
    int fromY;   // the first buffer row copied
    int toY;     // the picture row it goes to
    int rows;    // how many rows
};

// False when the map's range does not leave a view to draw (logged by the caller).
bool PlanStrips(const StripInput &in, StripPlan *out);
// Strip k (0 <= k < count). The first strip also copies the T rows above the view's origin.
Strip StripAt(const StripInput &in, const StripPlan &plan, int k);

} // namespace core::display
