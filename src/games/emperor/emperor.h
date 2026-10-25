// Emperor: Rise of the Middle Kingdom (GOG, Emperor.exe SHA256 6373328b...53c0e) -- the game-specific layer.
// Everything that knows an Emperor address lives under games/emperor/ (CLAUDE.md); core never does.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* From DllMain(DLL_PROCESS_ATTACH), before core_config_load: registers this layer's settings (core/settings), so
   they are read from emperor_engine.ini with the core's. Kernel32 only. */
void emperor_register_settings(void);

/* From DllMain(DLL_PROCESS_ATTACH), after core_startup_attach. Checks that the exe is the known build (the bytes at
   every hooked address) and installs the game hooks; on any mismatch it installs nothing and logs why. */
void emperor_attach(void);

/* From DllMain(DLL_PROCESS_ATTACH), before core_startup_attach: nonzero when the process is the game's separate
   editor, EmperorEdit.exe (the main menu's Campaign Creator starts it and exits). It loads this DLL from the game
   folder too; it gets only the logging wrapper around the real DirectDraw, so it runs as without the DLL (R2d). */
int emperor_editor_exe(void);

#ifdef __cplusplus
}

#include "games/emperor/site.h"

namespace emperor {
// For the offline tests (proxy/ddraw/test_restore.cpp): game_loop_step's and sim_tick_due's detours.
const Detour *MainDetours(int *count);
} // namespace emperor
#endif
