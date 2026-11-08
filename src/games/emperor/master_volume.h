// [sound] master_volume (docs/resolution_ui/original_settings.md §14.6): one volume over all of the game's sounds and
// music, as Augustus has. Not a setting of the game: the game's own volumes (Emperor.inf) stay as the player set
// them; ours scales what the game hands to Miles (mss32.dll) when it sets a sound's volume, and gives the game back
// the volume it asked for when it reads one (its music fades read the volume and step it).
#pragma once

namespace emperor::master_volume {

void RegisterSettings(); // start-up, before the settings are loaded
bool Attach();           // after the game's Miles is loaded (an import of the exe); false: not installed

} // namespace emperor::master_volume
