// City zoom (R5, docs/resolution_ui/r5_city_zoom.md): the scale definition of Augustus (100 = the game's own size,
// 200 = twice as much of the city seen, 50 = everything twice as large) and its limits. Nothing here knows a game:
// the game layer passes the sizes in.
//
// The city is drawn by the game into a buffer of the zoomed size (s/100 times the city's size on the screen) and then
// scaled onto the screen. The game's view (columns x half rows) covers that buffer; while it draws, one more column
// and two more half rows cover the smooth-scrolling remainder (games/emperor/scroll). The game's view must never be
// larger than the map's range: its column flags (5 x 231 bytes, no bounds check) and an overlay pass that reads the
// view table without a check rely on the view being clamped inside the map (r5_city_zoom.md section 8.2).
#pragma once

namespace core::display {

constexpr int kZoomMin = 50, kZoomMax = 400;

struct ZoomInput {
    int requested;    // percent (clamped to kZoomMin..kZoomMax)
    int showW, showH; // the city's size on the screen in pixels
    int colPx;        // a column step in pixels (tile width + 2)
    int halfH;        // a half row in pixels
    int mapCols, mapRows; // the map's range (view_clamp's limits); 0 or less: unknown
    // The half rows the game's clamp can hold (its bottom limit minus its top one), when that is less than mapRows
    // (Emperor keeps a margin above the map's bottom edge: r5_city_zoom.md section 9.1). A view taller than this is
    // pushed above the map's top. 0 or less: mapRows.
    int clampRows = 0;
    int maxPercent = 0; // a cap of the caller's (e.g. for speed); 0 or less: kZoomMax
};

struct ZoomLayout {
    int percent;        // in effect
    int limit;          // the largest percent the map's range (and the cap) allows (0 when unknown)
    int augustus;       // Augustus' limit (the whole map plus a 4 tile border fits, at least 100); for the log only
    bool capped;        // requested > limit
    bool unknownMap;    // no map range: 100 until there is one
    int bufW, bufH;     // the zoomed city in pixels (the part of the buffer that is shown)
    int cols, rows;     // the game's view in columns and half rows (without the drawing's extra column / rows)
};

// The buffer region shown for a percent: ceil(show * percent / 100) + 1 (one pixel more, so that enlarging it never
// leaves the last screen column or row unfilled).
int ZoomedSize(int show, int percent);

// The game's view for a percent: columns = ceil((bufW + 2) / colPx), half rows = ceil(bufH / halfH) + 1; with the
// drawing's extra column and two half rows this covers the buffer for any remainder (0 <= px < colPx,
// 0 <= py < 2 halfH).
void ZoomView(const ZoomInput &in, int percent, int *cols, int *rows);

// The view (plus the drawing's extra column and two half rows) within the map's range, and the view itself within
// what the clamp can hold.
bool ZoomFits(const ZoomInput &in, int percent);
bool ViewFitsMap(int cols, int rows, int mapCols, int mapRows, int clampRows);

// The layout for the input: percent = requested clamped to kZoomMin..min(limit, cap); 100 when the map is
// unknown; kZoomMin when even that does not fit (logged by the caller; the drawing checks again and draws at the
// game's own size then).
ZoomLayout ZoomFor(const ZoomInput &in);

} // namespace core::display
