// [ui] intro_movie = play | skip: whether the game plays its intro movie at start-up (both games). Only what has no
// game in it: the choices and their panel labels, and the value as read at start-up. Each game layer registers the
// item itself (core::settings::Register(Item{"ui", "intro_movie", ...}) with these choices), so its name and
// description are the game's (one game has a sentence about its own settings file; a check compares the texts),
// then hands the index to Watch; how the movie is left out is the game layer's (its own hook).
#pragma once

namespace core::settings::intro_movie {

inline constexpr int kChoiceCount = 2;
extern const char *const kChoices[kChoiceCount];      // "play", "skip"
extern const char *const kChoiceLabels[kChoiceCount]; // shown in the panel

// From the game layer's settings registration, with the index Register returned: Skip() then holds the value read
// at start-up (the setting takes effect the next time the game starts; a change in the panel does not alter it).
void Watch(int index);

// [ui] intro_movie = skip, as read at start-up.
bool Skip();

} // namespace core::settings::intro_movie
