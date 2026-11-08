// City picture and minimap picture (0.2.0, docs/archive/city_export.md): a key saves the whole city the player
// can scroll to as one PNG at the game's own pixel size, without the interface, or the minimap of the whole map.
//   [ui] city_picture_key = F10           [ui] minimap_picture_key = Shift+F10
//   [ui] minimap_picture_scale = 1 | 2 | 4
//   [debug] city_picture_strip_rows = 64 (hidden: half rows kept per strip; small values for the seam check)
//   [debug] city_picture_seam_check = off (hidden: on also saves the same moment in as few strips as possible)
// The game draws the city in strips into our buffer right after it drew the frame (games/emperor/scroll's after-city
// hook), with the overlay, the building preview and the animation timers held still meanwhile, and everything put
// back in the same frame; core/capture writes the PNG on its writer thread.
#pragma once

namespace emperor::city_picture {

// From emperor_register_settings (DllMain).
void RegisterSettings();
// After games/emperor/scroll attached (its city_render hook draws the pictures): the keys and the hook. False (logged)
// when a site does not match or smooth scrolling's hooks are not there.
bool Attach(bool scrollAttached);

// True once attached (the File menu's items are added only then).
bool Available();
// Game thread (the File menu's items): as the keys, the city or minimap screenshot at the next city frame.
void RequestCity();
void RequestMinimap();

} // namespace emperor::city_picture
