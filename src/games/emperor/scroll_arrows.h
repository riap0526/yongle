// Held scroll arrows at the original pace (as in Pericles; core/ui/scroll_repeat): the up / down arrows of the
// game's list scroll bars (messages, the file dialog, the campaign and mission lists...) scroll one line every 50 ms
// while held, counted from the press, instead of one line per drawn frame. The hook is on the game's image buttons
// function; nothing is saved.
#pragma once

namespace emperor::scroll_arrows {

// From emperor_attach: checks the function's bytes and hooks it; when they do not match nothing is installed (logged)
// and the arrows scroll as the game has them.
void Attach();

} // namespace emperor::scroll_arrows
