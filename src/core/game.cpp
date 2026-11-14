// The game's info for the core (core/game.h).
#include "core/game.h"

namespace {

// Neutral values until the entry point sets the game's (the offline tests that do not set one).
const core_game_info kNoGame = {"engine", "Engine", "dev", L"Engine", L"engine.ini", "engine.ini", 1024, 768,
                                nullptr, nullptr, nullptr, nullptr, nullptr};
const core_game_info *g_game = &kNoGame;

} // namespace

extern "C" void core_game_set(const core_game_info *info)
{
    g_game = info ? info : &kNoGame;
}

extern "C" const core_game_info *core_game(void)
{
    return g_game;
}
