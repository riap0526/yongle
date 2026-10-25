// Process start-up of the engine DLL: the work that has to happen before the game's own code runs.
//
// The proxy ddraw.dll is a static import of the game exe, so its DllMain runs before the exe's entry point --
// but under the loader lock, where user32 must not be called. core_startup_attach (from DllMain) therefore
// only installs hooks (kernel32 only, see core/hook/hook.h):
//   - the exe's entry point, found through its PE header (no game address): runs the start-up work after
//     every DLL is initialized and the loader lock is released, before the C runtime and WinMain;
//   - CreateWindowExA: fallback for the same work if the entry hook is missing or the DLL was loaded late
//     (as in the offline tests), and a log line for every top-level window the game creates.
// The start-up work is currently: make the process DPI aware (core/platform/dpi.h). With the emulated display
// back end, the virtual display's user32 hooks are installed here as well (core/display/virtual_display.h).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* From DllMain(DLL_PROCESS_ATTACH) after core_log_open. */
void core_startup_attach(void *module);

/* From DllMain(DLL_PROCESS_DETACH) when the DLL is unloaded with FreeLibrary (not on process exit). */
void core_startup_detach(void);

#ifdef __cplusplus
}
#endif
