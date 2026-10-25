/*
 * Proxy ddraw.dll for Emperor: Rise of the Middle Kingdom -- the DLL's entry point.
 *
 * Emperor.exe imports exactly one function from DDRAW.dll: DirectDrawCreate
 * (IAT 0x7a9020, thunk 0x5fb4c0, callers 0x44d307 and 0x52bc45). This DLL is
 * loaded from the game directory instead of the system copy. Depending on
 * emperor_engine.ini it returns our own DirectDraw (src/core/ddraw/ddraw_emu,
 * the default) or loads the real system ddraw.dll behind the logging wrapper
 * (src/core/ddraw/ddraw_wrap). The log is ddraw_proxy.log next to this DLL.
 *
 * The real DLL is loaded lazily on the first call, never inside DllMain
 * (loader lock). DllMain only opens the log and installs the start-up hooks
 * (src/core/startup.h), which make the process DPI aware before the game's
 * first window.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/config.h"
#include "core/ddraw/ddraw_emu.h"
#include "core/ddraw/ddraw_wrap.h"
#include "core/log.h"
#include "core/platform/timing.h"
#include "core/startup.h"
#include "games/emperor/emperor.h"

static HMODULE g_real;
static core_DirectDrawCreate_fn g_real_create;

/* Loads the system ddraw.dll by absolute path so we never load ourselves. */
static BOOL load_real(void)
{
    char path[MAX_PATH];
    static const char name[] = "\\ddraw.dll";
    UINT len, i;

    if (g_real_create)
        return TRUE;

    /* For a 32-bit process on 64-bit Windows this resolves to SysWOW64 via
       file system redirection. */
    len = GetSystemDirectoryA(path, MAX_PATH);
    if (len == 0 || len + sizeof(name) > MAX_PATH) {
        core_log("GetSystemDirectoryA failed (err %lu)", GetLastError());
        return FALSE;
    }
    for (i = 0; i < sizeof(name); i++)
        path[len + i] = name[i];

    g_real = LoadLibraryA(path);
    if (!g_real) {
        core_log("LoadLibraryA(%s) failed (err %lu)", path, GetLastError());
        return FALSE;
    }
    g_real_create = (core_DirectDrawCreate_fn)GetProcAddress(g_real, "DirectDrawCreate");
    if (!g_real_create) {
        core_log("GetProcAddress(DirectDrawCreate) failed (err %lu)", GetLastError());
        return FALSE;
    }
    core_log("loaded real ddraw: %s at %p, DirectDrawCreate=%p",
             path, (void *)g_real, (void *)g_real_create);
    return TRUE;
}

HRESULT WINAPI DirectDrawCreate(GUID *guid, void **dd, IUnknown *outer)
{
    void *caller = __builtin_return_address(0);

    /* emperor_engine.ini display.backend: our own DirectDraw (default) or the
       real one behind the logging wrapper, for comparison. */
    if (core_config_get()->backend == CORE_BACKEND_EMULATED)
        return core_ddraw_emu_create(guid, dd, outer, caller);
    if (!load_real())
        return E_FAIL;
    return core_ddraw_create(g_real_create, guid, dd, outer, caller);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(inst);
        core_log_open(inst, "ddraw_proxy.log");
        core_log("Yongle " YONGLE_VERSION " (ddraw proxy: emulated DirectDraw + SDL3 output thread, or logging "
                 "wrapper) attached (module %p, pid %lu, exe %s)",
                 (void *)inst, GetCurrentProcessId(), GetCommandLineA());
        emperor_register_settings();
        core_config_load(inst);
        /* The game's separate editor (EmperorEdit.exe, the Campaign Creator, which the main menu starts) loads this
           DLL from the game folder too: the real DirectDraw behind the logging wrapper and nothing else, so it runs
           as without the DLL (R2d; docs/resolution_ui/r2d.md). */
        if (emperor_editor_exe()) {
            core_config_force_system_backend();
            core_log("startup: EmperorEdit.exe: only the logging wrapper around the real DirectDraw (no display "
                     "hooks, no game hooks)");
            break;
        }
        core_startup_attach(inst);
        emperor_attach();
        break;
    case DLL_PROCESS_DETACH:
        core_ddraw_write_summary(reserved ? "process exit" : "DLL unload");
        core_log("ddraw proxy detaching%s", reserved ? " (process exit)" : "");
        /* On FreeLibrary the hooks must go before our code is unmapped; on process exit they stay. The timer
           resolution is normally given back when game_main returns; this is the fallback. */
        if (!reserved)
            core_startup_detach();
        else
            core_timer_end();
        /* On process exit the system unloads the real DLL itself. */
        if (!reserved && g_real)
            FreeLibrary(g_real);
        core_log_close();
        break;
    }
    return TRUE;
}
