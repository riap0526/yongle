// The game's own options in the options window (core/options/game_options): game and scroll speed, autosave, the
// message kinds, and the four sound switches and volumes. Here: where Emperor keeps them (the Emperor.inf block,
// written back by the game at exit) and how its own options dialogs change them; and a new player's starting speeds,
// given once Emperor.inf has been read.
#pragma once

#include <string>

namespace emperor::options_original {

// Start-up (emperor_attach, after resolution, before options_entry): checks the game's functions and gives them to
// the core's rows. false: they are not as expected (the rows are left out).
bool Attach();

// Game thread: the music, speech and ambient volumes handed to the sound manager again, as the game does it (the
// master volume changed).
void ReapplyVolumes();

// Game thread, once Emperor.inf has been read (screen_select_initial_mode returned; resolution's detour calls it when
// resolution hooks that function): a new player gets our starting speeds.
void AfterInfRead();

// For the offline test (src/games/emperor/test_restore.cpp): the rows given to the core without checking the game's
// functions.
void AttachForTest();

// The game's text (EmperorText.eng group, index; lang_get_string 0x5278b0), "" when there is none.
std::string GameText(int group, int index);

} // namespace emperor::options_original
