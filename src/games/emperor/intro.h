// 0.2.0 item 2: [display] intro_movie = play | skip (docs/roadmap.md "0.2.0").
// The game reads [Config] PlayIntroMovie from its own Emperor.ini in config_ini_load (once, from game_main) and plays
// the intro movie before the main loop when play_intro_movie is not 0 (docs/symbols.md). With skip, play_intro_movie
// is set to 0 right after config_ini_load. Emperor.ini is never written: without the DLL the game does what its file
// says, and PlayIntroMovie=no in that file still skips the movie with play.
#pragma once

namespace emperor::intro {

// From emperor_register_settings (DllMain): [display] intro_movie.
void RegisterSettings();

// Checks config_ini_load and installs its hook; left out (logged) when it does not match: the movie then plays as the
// game decides.
bool Attach();

} // namespace emperor::intro
