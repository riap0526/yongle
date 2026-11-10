// The ways into the options window: the game's
// Options menu in the city becomes General Settings / Video / Audio (each opens the window at that page) and the
// game's own Autosave switch; the main menu's "Visit Website" becomes "Options".
// Also the File menu's Save city screenshot and Save minimap screenshot.
#pragma once

namespace emperor::options_entry {

// Start-up, after options_window, options_original and city_picture: the Options part only with the window, the File
// part only with the screenshots; when neither is available nothing is installed (the game's menus stay). false:
// the Options part is not installed.
bool Attach();

} // namespace emperor::options_entry
