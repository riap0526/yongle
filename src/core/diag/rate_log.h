// The part of the periodic rate log that is the same for every game (each game layer writes its own "rates:" line
// first, with its own fields): where the time went (perf phases: "time" and "time max"), how evenly frames were shown
// ("intervals") and the processor time given to each thread ("cpu"). Each of these takes its window, so the next
// call starts a new one. Game thread. Nothing here knows a game.
#pragma once

namespace core::diag {

// why: the same suffix as the game's rates line (e.g. "", " (game ended)", " (report #2)").
void LogRateTail(const char *why);

} // namespace core::diag
