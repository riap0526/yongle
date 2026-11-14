// Emperor's info for the core (core/game.h): the names, the settings file, the game's own size and this layer's
// entry points (games/emperor/emperor.h).
#include "core/game.h"
#include "games/emperor/emperor.h"

#ifndef ENGINE_VERSION
#define ENGINE_VERSION "dev" // set by CMakeLists.txt for the DLL; the offline tests build without it
#endif

namespace {

const core_game_info kEmperor = {
    "emperor",
    "Yongle",
    ENGINE_VERSION,
    L"Yongle",
    L"emperor_engine.ini",
    "emperor_engine.ini",
    1024, // the 1024 layout, the game's largest mode
    768,
    &emperor_register_settings,
    &emperor_attach,
    &emperor_editor_exe,
    "6373328bfc5c4886d9abc9544eb706e89e7d465b18176ea8205fe27aaee53c0e",
    "English GOG Emperor.exe 1.0.1.0",
};

} // namespace

extern "C" const core_game_info *game_info(void)
{
    return &kEmperor;
}
