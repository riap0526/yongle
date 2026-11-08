// The game's own options in the options window (docs/archive/resolution_ui_original_settings.md §12.1, §14 S2c): game and
// scroll speed, autosave, the message kinds, and the four sound switches and volumes. The values stay where the game
// keeps them (the Emperor.inf block, written back by the game at exit), so the game without our DLL reads the same;
// a change is carried out as the game's own options dialogs do it.
#pragma once

#include <string>

#include <vector>

#include "core/options/window.h"

namespace emperor::options_original {

// Start-up (emperor_attach, before the game's own start-up sets its defaults): checks the game's functions, and
// gives a new player (no Emperor.inf yet) our starting game and scroll speed. false: the functions are not as
// expected (the rows are left out).
bool Attach();

// Game thread: the music, speech and ambient volumes handed to the sound manager again, as the game does it (the
// master volume changed).
void ReapplyVolumes();

// Rows for the window; nothing when Attach failed. Game thread (they read the game's values).
void SpeedRows(std::vector<core::options::Row> &rows);
void AutosaveRows(std::vector<core::options::Row> &rows);
void MessageRows(std::vector<core::options::Row> &rows);
void SoundRows(std::vector<core::options::Row> &rows);

// The game's text (EmperorText.eng group, index; lang_get_string 0x5278b0), "" when there is none.
std::string GameText(int group, int index);

} // namespace emperor::options_original
