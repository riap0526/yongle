#include "core/hook/hook.h"

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <MinHook.h>

namespace {

bool g_ready = false;
int g_installed = 0;

bool Ready()
{
    if (g_ready) {
        return true;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        core_log("hook: MH_Initialize failed: %s", MH_StatusToString(st));
        return false;
    }
    g_ready = true;
    return true;
}

} // namespace

extern "C" int core_hook_install(const char *name, void *target, void *detour, void **original)
{
    char where[MAX_PATH + 40];
    core_describe_address(target, where, sizeof(where));
    if (!Ready()) {
        core_log("hook %s at %s: not installed (MinHook unavailable)", name, where);
        return 0;
    }
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st == MH_OK) {
        st = MH_EnableHook(target);
        if (st != MH_OK) {
            MH_RemoveHook(target);
        }
    }
    if (st != MH_OK) {
        core_log("hook %s at %s: FAILED (%s)", name, where, MH_StatusToString(st));
        return 0;
    }
    ++g_installed;
    core_log("hook %s at %s -> detour %p, original via trampoline %p", name, where, detour, *original);
    return 1;
}

extern "C" int core_hook_install_api(const char *name, const wchar_t *module, const char *proc, void *detour,
                                     void **original)
{
    HMODULE mod = GetModuleHandleW(module);
    if (!mod) {
        core_log("hook %s: module %ls is not loaded", name, module);
        return 0;
    }
    FARPROC target = GetProcAddress(mod, proc);
    if (!target) {
        core_log("hook %s: %ls!%s not found (err %lu)", name, module, proc, GetLastError());
        return 0;
    }
    return core_hook_install(name, reinterpret_cast<void *>(target), detour, original);
}

extern "C" void core_hook_remove_all(void)
{
    if (!g_ready) {
        return;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_ready = false;
    core_log("hook: removed all %d hook(s)", g_installed);
    g_installed = 0;
}
