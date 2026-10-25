// R2c: the China map at modern resolutions (docs/archive/resolution_ui_r2c.md §5).
//
// The game keeps drawing the map exactly as it always does -- picture at (0, 40), origin (0, 40), scroll offsets 0
// (they are saved in the savegame and stay untouched) -- but into a buffer of ours. We then show it in the map area
// (left of the sidebar, below the top bar): the map inside the dialog panel's fret border, scaled to the area
// (display.china_map = scaled, the default; whole-number scales only with display.china_map_integer) or at its own
// size (native), over the same map blurred and darkened. City markers, names, routes and armies are part of the
// map's pixels, so they move and scale with it. The mouse is converted back to the game's own map coordinates only
// inside the map's input functions; the sidebar, the top bar, panels and dialogs keep the real position.
//
// Only when the whole map fits the area at its own size (otherwise the game scrolls it, as at 1024x768: then the
// game's own drawing is left alone), never on the editor's map (screen 0x15).
#pragma once

#include "games/emperor/site.h"

namespace emperor::china_map {

// Start-up, before the settings file is read: display.china_map and display.china_map_integer.
void RegisterSettings();

// Checks the sites and installs the hooks. False (nothing installed) when a site does not match.
bool Attach();

// For the offline tests (proxy/ddraw/test_restore.cpp).
const Detour *Detours(int *count);
void SetModeForTest(int mode, bool integer); // mode: 0 scaled, 1 native, 2 original

} // namespace emperor::china_map
