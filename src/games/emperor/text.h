// Emperor's text as our UTF-8 (CLAUDE.md: text is UTF-8 inside the engine). For now the game's own strings are
// Windows-1252 and are converted; the Chinese translation (roadmap) replaces strings here, by the game's text group
// and index where they are known, without changing the callers (e.g. the loading screen's status line, R2a).
#pragma once

#include <cstddef>

namespace emperor::text {

// The game's string (Windows-1252) as UTF-8; out is always terminated.
void GameToUtf8(const char *game, char *out, size_t size);

} // namespace emperor::text
