#include "core/hook/hook.h"

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <MinHook.h>

#include <cstring>

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

extern "C" void *core_real_export(const wchar_t *module, const char *proc)
{
    auto *base = reinterpret_cast<BYTE *>(GetModuleHandleW(module));
    if (!base) {
        return nullptr;
    }
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) {
        return nullptr;
    }
    auto *exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY *>(base + dir.VirtualAddress);
    auto *names = reinterpret_cast<DWORD *>(base + exp->AddressOfNames);
    auto *ordinals = reinterpret_cast<WORD *>(base + exp->AddressOfNameOrdinals);
    auto *functions = reinterpret_cast<DWORD *>(base + exp->AddressOfFunctions);
    DWORD lo = 0, hi = exp->NumberOfNames; // the names are sorted
    while (lo < hi) {
        const DWORD mid = (lo + hi) / 2;
        const int c = std::strcmp(reinterpret_cast<const char *>(base + names[mid]), proc);
        if (c == 0) {
            const DWORD rva = functions[ordinals[mid]];
            if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) {
                return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(base), proc)); // forwarded
            }
            return base + rva;
        }
        if (c < 0) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    return nullptr;
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
    void *real = core_real_export(module, proc);
    if (real && real != reinterpret_cast<void *>(target)) {
        char where[MAX_PATH + 40];
        core_log("hook %s: a compatibility shim stands in front of %ls!%s (calls reach %s first); hooked there, so "
                 "the exe's calls are still seen",
                 name, module, proc, core_describe_address(reinterpret_cast<void *>(target), where, sizeof(where)));
    }
    return core_hook_install(name, reinterpret_cast<void *>(target), detour, original);
}

extern "C" int core_hook_log_shimmed_imports(void)
{
    auto *base = reinterpret_cast<BYTE *>(GetModuleHandleW(nullptr));
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return 0;
    }
    int shimmed = 0, total = 0;
    for (auto *d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); d->Name; ++d) {
        const char *dll = reinterpret_cast<const char *>(base + d->Name);
        wchar_t wdll[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, dll, -1, wdll, MAX_PATH);
        if (!GetModuleHandleW(wdll) || !d->OriginalFirstThunk) {
            continue;
        }
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA32 *>(base + d->OriginalFirstThunk);
        auto *addrs = reinterpret_cast<IMAGE_THUNK_DATA32 *>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
                continue;
            }
            const char *proc = reinterpret_cast<const char *>(
                reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name);
            ++total;
            void *real = core_real_export(wdll, proc);
            void *now = reinterpret_cast<void *>(static_cast<uintptr_t>(addrs->u1.Function));
            if (real && now != real) {
                char where[MAX_PATH + 40];
                core_log("hook: the exe's import %s!%s goes to %s (a compatibility shim)", dll, proc,
                         core_describe_address(now, where, sizeof(where)));
                ++shimmed;
            }
        }
    }
    core_log("hook: %d of the exe's %d named imports go through a compatibility shim", shimmed, total);
    return shimmed;
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
