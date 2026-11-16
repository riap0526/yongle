// City picture and minimap picture for this game family (Yongle since 0.2.0, and Pericles):
// a key saves the whole city the player can scroll to as one PNG at the game's own
// pixel size, without the interface, or the minimap of the whole map.
//   [ui] city_picture_key = F10           [ui] minimap_picture_key = Shift+F10
//   [ui] minimap_picture_scale = 1 | 2 | 4
//   [debug] city_picture_strip_rows = 64 (hidden: half rows kept per strip; small values for the seam check)
//   [debug] city_picture_seam_check = off (hidden: on also saves the same moment in as few strips as possible)
// A key asks for a picture; it is drawn at the end of a later city_render (core/scroll/picture's after-city callback),
// when the game layer says a drawing can be made now. The city is drawn in strips into our buffer with the game's
// variables changed for the drawing (the game layer's beginCityDrawing) and put back in the same frame; each strip
// (and each of the minimap's two drawings) runs inside the game layer's aroundDrawing, with this file's guard inside
// that. core/capture writes the PNG on its writer thread. Nothing here knows a game's addresses.
//
// Failure: a fault while drawing puts the game's variables back (the guard's list, then the rest), switches the
// pictures off for the rest of the run and tells the player that nothing was saved; the game goes on. Game thread,
// except Done (the writer thread; the notice queue has its own lock).
#pragma once

#include "core/diag/guard.h"
#include "core/display/city_strips.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>

namespace core::capture::city_picture {

enum class Drawing { City, Minimap };

// What the pictures need from the game (a constant of the game layer; every field required).
struct CityPictureGame {
    CityPictureGame(bool (*drawingWorks)(), bool (*cityScreen)(), int (*gameScreen)(),
                    void (*viewLimits)(int *left, int *right, int *top, int *bottom, int *mapRows),
                    void (*tileSize)(int *tileWidth, int *halfHeight), int viewTableCols, int viewTableRows,
                    int (*viewRows)(),
                    void (*beginCityDrawing)(core::diag::Restore &r, core::diag::Restore &r2,
                                             const core::display::StripPlan &p, BYTE *buf, int pitchPx),
                    void (*drawStrip)(int vx, int vy, int orientation),
                    void (*drawMinimap)(core::diag::Restore &r, BYTE *buf, int pitchPx, int rows, int x0, int y0,
                                        RECT *area),
                    void (*hold)(Drawing d), void (*drawn)(Drawing d), void (*release)(Drawing d),
                    bool (*drawingNow)(), bool (*aroundDrawing)(void (*fn)(void *), void *ctx),
                    const unsigned char *(*cityName)())
        : drawingWorks(drawingWorks), cityScreen(cityScreen), gameScreen(gameScreen), viewLimits(viewLimits),
          tileSize(tileSize), viewTableCols(viewTableCols), viewTableRows(viewTableRows), viewRows(viewRows),
          beginCityDrawing(beginCityDrawing), drawStrip(drawStrip), drawMinimap(drawMinimap), hold(hold),
          drawn(drawn), release(release), drawingNow(drawingNow), aroundDrawing(aroundDrawing), cityName(cityName)
    {
    }

    bool (*drawingWorks)(); // the city drawing's hooks are in and working (smooth scrolling's; Zeus: and its put-back)
    bool (*cityScreen)();   // the game's screen is one where the city is drawn below (a picture can be asked for)
    int (*gameScreen)();    // the game's screen number (the log)
    // The view's range the player can scroll to: columns left..right, half rows top..bottom (bottom: the topmost
    // row the view's top can reach plus the half rows it holds), the map's half rows.
    void (*viewLimits)(int *left, int *right, int *top, int *bottom, int *mapRows);
    void (*tileSize)(int *tileWidth, int *halfHeight); // tile_width (a column is tile_width + 2 px), half a tile's height
    int viewTableCols, viewTableRows; // the minimap's view table (columns x half rows drawn)
    int (*viewRows)();                // the city view's half rows now (the minimap's frame is drawn that tall)
    // The game's variables for drawing the strips into buf (pitchPx pixels a row, p's sizes): changed in r (put back
    // by the guard on a fault), the ones put back only after everything in r2.
    void (*beginCityDrawing)(core::diag::Restore &r, core::diag::Restore &r2, const core::display::StripPlan &p,
                             BYTE *buf, int pitchPx);
    void (*drawStrip)(int vx, int vy, int orientation); // the game's city_render once (timed as the picture's)
    // The minimap drawn into buf at (x0, y0), its view frame below the rows it uses; the variables changed in r.
    // area: the part of buf that can hold the map (right and bottom exclusive), preset to the view table's rows; a
    // game whose minimap also paints outside the map (Zeus's filler) narrows it. The picture is cropped to what was
    // drawn inside it.
    void (*drawMinimap)(core::diag::Restore &r, BYTE *buf, int pitchPx, int rows, int x0, int y0, RECT *area);
    // Around one picture: hold before anything of ours is changed (state the drawing changes on its own, copied);
    // drawn right after the last drawing (before anything is put back); release after r and r2 are put back.
    void (*hold)(Drawing d);
    void (*drawn)(Drawing d);
    void (*release)(Drawing d);
    // A picture can be drawn in this city_render (false: asked again at the next one, the request kept).
    bool (*drawingNow)();
    // fn(ctx) as one drawing the game puts back on its own (Yongle: just the call; Zeus: a drawing that never counts).
    // False (fn not called) when that cannot be done now.
    bool (*aroundDrawing)(void (*fn)(void *), void *ctx);
    // The player's city's name as the game has it (read up to 64 bytes or its end), nullptr when there is none
    // (logged by the game layer).
    const unsigned char *(*cityName)();
};

// From the game layer's settings registration: the five settings above (the [debug] ones not in the panel's template).
void RegisterSettings();

// From the game layer's Attach once the city drawing's hooks are in and its sites match: the keys, the after-city
// callback (core/scroll/picture) and the log line.
void Install(const CityPictureGame &g);

// Installed (the File menu's items are added only then).
bool Available();
// Game thread (a menu's items): as the keys, the city or minimap screenshot at a later city frame.
void RequestCity();
void RequestMinimap();

// A game text of at most size bytes, for the log ("picture: <what> "...""): printable characters as they are, others
// as \xNN.
void LogText(const char *what, const unsigned char *p, int size);

// For the offline tests: the after-city callback as core/scroll/picture calls it, our state back to rest (nothing
// installed, on again), the picture's file name stem, how the last picture's minimap drawings compared.
void AfterCityForTest(int orientation);
void ResetForTest();
void StemForTest(char *out, std::size_t size);
int MinimapDifferencesForTest();

} // namespace core::capture::city_picture
