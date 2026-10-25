// Function hooks for the whole DLL, built on MinHook. Every hook -- system APIs in core, game functions later
// from games/<game>/ -- is installed through here, so all of them are logged the same way and removed together.
//
// Threading: install hooks during start-up (DllMain or the exe entry hook) or from the game thread; the
// functions are not meant to be called concurrently. Installing only calls kernel32 (MinHook allocates the
// trampolines, briefly suspends the other threads and patches code), so it is allowed under the loader lock;
// the detours themselves must not assume they run outside it unless the hooked function guarantees that.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Hooks target so that calls go to detour; *original receives a pointer that calls the unhooked function.
   name is only for the log. Returns nonzero on success (the hook is then enabled). */
int core_hook_install(const char *name, void *target, void *detour, void **original);

/* Same for an exported function of a module that is already loaded (e.g. L"user32.dll", "CreateWindowExA"). */
int core_hook_install_api(const char *name, const wchar_t *module, const char *proc, void *detour, void **original);

/* Disables and removes every hook. Only for DLL unload (FreeLibrary); on process exit nothing is restored. */
void core_hook_remove_all(void);

#ifdef __cplusplus
}
#endif
