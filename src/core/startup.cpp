#include "core/startup.h"

#include "core/config.h"
#include "core/diag/guard.h"
#include "core/display/virtual_display.h"
#include "core/hook/hook.h"
#include "core/log.h"
#include "core/output/sdl_output.h"
#include "core/platform/dpi.h"
#include "core/platform/timing.h"
#include "core/settings/settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstring>

// ---------------------------------------------------------------------------------------------
// Exe entry point hook. The entry point (WinMainCRTStartup for VC6) takes no arguments and never returns, so
// the detour is a naked stub: save all registers, run the start-up work, restore them and jump to the
// trampoline, leaving the stack exactly as the loader set it up.

namespace {

// Settings the core itself applies when they change in our panel (core/settings/settings.h).
bool ApplyTimer(int, const char *v, bool initial)
{
    if (!initial) {
        if (std::strcmp(v, "on") == 0) {
            core_timer_begin();
        }
        else {
            core_timer_end();
        }
    }
    return true;
}

bool ApplyPresent(int, const char *v, bool initial)
{
    if (initial) {
        return true;
    }
    // display <-> cap60: only the output thread's vsync interval changes. original presents on the game thread
    // without an output thread: switching to or from it needs a restart.
    core_present_mode m = std::strcmp(v, "cap60") == 0     ? CORE_PRESENT_CAP60
                          : std::strcmp(v, "display") == 0 ? CORE_PRESENT_DISPLAY
                                                           : CORE_PRESENT_ORIGINAL;
    return core::output::SetPresentMode(m);
}

void AttachCoreSettings()
{
    core::settings::OnChange(core::settings::Find("timing", "high_resolution_timer"), &ApplyTimer);
    core::settings::OnChange(core::settings::Find("display", "present"), &ApplyPresent);
}

} // namespace

extern "C" {
void *g_core_entry_original = nullptr;

__attribute__((used)) void core_startup_on_entry(void)
{
    core_log("exe entry point reached (loader lock released); system timer resolution %u us",
             core_timer_resolution_us());
    core_dpi_apply("exe entry point");
    if (core_config_get()->high_resolution_timer) {
        core_timer_begin();
    }
}

__attribute__((naked)) void core_startup_entry_detour(void)
{
    __asm__ volatile("pushal\n\t"
                     "call _core_startup_on_entry\n\t"
                     "popal\n\t"
                     "jmp *_g_core_entry_original\n\t");
}
}

namespace {

void *ExeEntryPoint()
{
    auto *base = reinterpret_cast<BYTE *>(GetModuleHandleW(nullptr));
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.AddressOfEntryPoint == 0) {
        return nullptr;
    }
    return base + nt->OptionalHeader.AddressOfEntryPoint;
}

// ---------------------------------------------------------------------------------------------
// CreateWindowExA hook

using CreateWindowExA_fn = HWND(WINAPI *)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE,
                                          LPVOID);
CreateWindowExA_fn g_createWindowExA = nullptr;
std::atomic<int> g_windowsLogged{0};
constexpr int kMaxWindowsLogged = 16;

HWND WINAPI CreateWindowExA_detour(DWORD exStyle, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                   HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    void *caller = __builtin_return_address(0);
    core_dpi_apply("first CreateWindowExA (entry hook did not run)");
    HWND hwnd = g_createWindowExA(exStyle, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (!parent && g_windowsLogged++ < kMaxWindowsLogged) {
        char atom[16];
        if (IS_INTRESOURCE(cls)) {
            wsprintfA(atom, "#%u", static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(cls)));
            cls = atom;
        }
        char where[MAX_PATH + 40];
        core_log("CreateWindowExA(class=\"%s\" title=\"%s\" style=0x%lx ex=0x%lx x=%d y=%d w=%d h=%d) -> %p  caller=%s",
                 cls ? cls : "", title ? title : "", style, exStyle, x, y, w, h, static_cast<void *>(hwnd),
                 core_describe_address(caller, where, sizeof(where)));
        core_dpi_log_window(hwnd, "created");
    }
    return hwnd;
}

} // namespace

extern "C" void core_startup_attach(void *module)
{
    (void)module;
    core::diag::InstallExceptionLog(); // first: everything after it is covered
    AttachCoreSettings();
    if (core_config_get()->backend == CORE_BACKEND_EMULATED) {
        core::display::InstallHooks(); // inactive until the emulated DirectDraw sets a mode
    }
    if (void *entry = ExeEntryPoint()) {
        core_hook_install("exe entry point", entry, reinterpret_cast<void *>(&core_startup_entry_detour),
                          &g_core_entry_original);
    }
    else {
        core_log("startup: exe entry point not found; DPI falls back to the CreateWindowExA hook");
    }
    core_hook_install_api("CreateWindowExA", L"user32.dll", "CreateWindowExA",
                          reinterpret_cast<void *>(&CreateWindowExA_detour),
                          reinterpret_cast<void **>(&g_createWindowExA));
}

extern "C" void core_startup_detach(void)
{
    core_timer_end();
    core_hook_remove_all();
}
