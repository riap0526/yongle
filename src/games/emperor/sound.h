// O4 (docs/archive/optimization.md): the game's sound, through Miles (mss32.dll), on the game thread only.
//   - [sound] background_sound (O4a): pause (default) -- when the game window loses the focus (WM_KILLFOCUS, or
//     WM_ACTIVATE minimized) the game calls sound_stop_all, which closes the music and stops every sound, so another
//     song starts when one comes back; we pause them instead and let them go on as soon as the window has the focus
//     again (before the game sees that message, so before music_update, and also with our panel open).
//     original: the game's own way.
//   - [sound] ambient_sounds (O4b): the city's random ambient sounds so many times per second (0.5 by default;
//     games/emperor/ambient_rate.h) instead of once in N main-loop passes (original).
//   - The left-right position (pan) of a sound is kept within Miles's 0..127 (the game gives 128 at the right edge).
//   - [debug] sound_probe: every 10 s in the city the log counts the ambient sounds (`sound probe:`); off by default.
#pragma once

namespace emperor::sound {

// From emperor_register_settings (DllMain): [sound] background_sound and ambient_sounds.
void RegisterSettings();

// Checks the sites and installs the hooks; each part is left out (logged) when its site does not match.
bool Attach();

// Once per main-loop pass (game thread), before the game's pass: sounds paused while switched away go on, the
// ambient sounds' N follows the passes per second, and the 10 s counts are written.
void OnLoop();

// Game thread: one of our windows (options, Hotkeys) is open: no new random ambient sound starts meanwhile, as under the
// game's own dialogs (the music goes on, the ambient sound playing ends by itself). Their clicks no longer get lost
// among them.
void SetQuiet(bool quiet);

} // namespace emperor::sound
