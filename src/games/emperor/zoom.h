// R5 city zoom (docs/resolution_ui/r5_city_zoom.md; R5b: a fixed scale from the settings, no mouse conversion yet).
//   [display] city_zoom = 50..400 (percent, Augustus' scale: 100 the game's own size, 200 twice as much of the city
//             seen); in effect at most what the map's range allows (core/display/city_zoom.h), worked out again when
//             the map or the city's size changes (view_clamp, the viewport hooks).
//   [display] city_zoom_shrink = average | nearest (seeing more: how the buffer is shrunk; seeing less: sharp
//             bilinear always).
// The game's view is the zoomed one all the time (games/emperor/resolution sets it), so view_clamp, centring, rotation,
// the minimap's frame and bookmarks follow it; games/emperor/scroll draws the city into a buffer of that size and
// scales it onto the screen. R5b: while the scale is not 100, clicks on the city do not reach the game.
#pragma once

namespace emperor::zoom {

// From emperor_register_settings (DllMain).
void RegisterSettings();

// Checks and installs view_clamp's and city_render_tiles_at's hooks; false (logged) when a site does not match: the
// city then stays at 100.
bool Attach();

// The scale in effect for the city on the screen (showW x showH pixels, columns of colPx, half rows of halfH) and the
// current map: the game's view in columns and half rows for it. Logged when it changes. 100 (and the given view
// untouched) when zoom is not available (not attached, switched off after an error, or no map yet).
int ViewFor(int showW, int showH, int colPx, int halfH, int *cols, int *rows);

// The percent and the zoomed buffer size last worked out (100 when off).
int Percent();
void BufferSize(int *w, int *h);

// The current map's range (columns x half rows; 0 or less before a map), for the rate log.
void MapRange(int *cols, int *rows);

// Seeing more: averaged shrinking (else nearest).
bool AverageShrink();

// True when the view drawn now (plus the extra column and two half rows) is within the map's range; checked by the
// drawing before every zoomed frame.
bool ViewFits(int cols, int rows);

// The game's present (end of a frame): the first frame with the small city view (0x46a320) is saved as
// shots\smallview_<tag>_game_... (where that view is shown is not known yet).
void OnPresent();

// Zoom off for the rest of the run (smooth scrolling, which draws the zoomed city, was switched off after an error):
// the view is laid out again at 100 right away, before the city is drawn the game's way.
void Off(const char *why);

// The game's own layouts and small tiles: no zoom (100) until the next ViewFor.
void ResetView();

// Before drawing: lays the view out again when the map's range changed or the view does not fit it.
void Recheck();

// games/emperor/resolution: lays the city's view out again (the viewport hooks' work).
void SetRelayout(void (*fn)());

} // namespace emperor::zoom
