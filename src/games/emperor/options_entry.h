// The ways into the options window (docs/resolution_ui/original_settings.md §12.2, §12.4, §14.7, S2d): the game's
// Options menu in the city becomes General Settings / Video / Audio (each opens the window at that page) and the
// game's own Autosave switch; the main menu's "Visit Website" becomes "Options".
#pragma once

namespace emperor::options_entry {

// Start-up, after options_window and options_original: installs nothing when the window is not available (the game's
// own Options menu and main menu stay). false: not installed.
bool Attach();

} // namespace emperor::options_entry
