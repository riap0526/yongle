// Our messages in the game's own notice boxes (0.2.0 item 6, docs/archive/game_notices.md): the boxes the game shows at the
// top of the city ("Game saved"), with their look, place and time on screen (15 s), through the game's
// city_warning_show_text 0x547d30. The game's own "Warnings" switch (Help menu) does not hide ours: they answer
// something the player just did. Shown on the game thread, from the game's notice drawing (city_warnings_draw
// 0x547a20, hooked), so Post can be called from any thread.
#pragma once

namespace emperor::notice {

// After the game's hooks: hooks city_warnings_draw. False (logged) when a site does not match; Post then shows the
// text on our own caption line (core/output SetNotice), as before.
bool Attach();

// Any thread: shows text (at most 99 characters) in a notice box at the next game frame. red: in the game's red
// warning font, as the game shows errors and what needs attention.
void Post(const char *text, bool red = false);

// Any thread: the city's zoom in percent. The zoom box is made once and then rewritten in place while the zoom
// changes (the number counts along as the zoom moves), staying 15 s after the last change, as every notice does.
void Zoom(int percent);

} // namespace emperor::notice
