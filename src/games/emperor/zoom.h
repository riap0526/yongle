// City zoom: the scale from the settings, clicks converted, the
// wheel (25 a notch around the cursor, smooth) and the middle button (back to 100); the wheel's scale is not saved.
//   [video] city_zoom = 50..150 (percent, Augustus' scale: 100 the game's own size, 150 half as much again of the city
//           seen); in effect at most what the map's range allows (core/display/city_zoom.h), worked out again when
//           the map or the city's size changes (view_clamp, the viewport hooks).
//   [video] city_zoom_shrink = average | nearest (seeing more: how the buffer is shrunk; seeing less: sharp
//           bilinear always).
// The zoom is the core's (core/scroll/zoom.h, shared with Zeus); this file gives it Emperor's addresses (the map's
// range, the bottom margin, the city screens) and hooks view_clamp. games/emperor/resolution lays the view out for it
// (ViewFor), games/emperor/scroll passes its drawing and camera callbacks to the picture and the camera.
#pragma once

namespace emperor::zoom {

// From emperor_register_settings (DllMain): the core's [video] city_zoom and city_zoom_shrink, at this place in the
// panel.
void RegisterSettings();

// Checks and installs view_clamp's hook and the core's zoom (the wheel's mouse filter); false (logged) when view_clamp
// does not match: the city then stays at 100.
bool Attach();

// The current map's range (columns x half rows; 0 or less before a map), for the rate log.
void MapRange(int *cols, int *rows);

// The limits view_clamp works with now (city picture): view x from left to
// right - columns, view y from top to bottom - half rows (bottom: the map's bottom less the margin the game keeps
// below the screen); mapRows: the map's range in half rows.
void ViewLimits(int *left, int *right, int *top, int *bottom, int *mapRows);

// games/emperor/resolution: lays the city's view out again (the viewport hooks' work).
void SetRelayout(void (*fn)());

// For the offline test (test_restore): the core's zoom given Emperor's view, without view_clamp's hook.
void InstallForTest();

} // namespace emperor::zoom
