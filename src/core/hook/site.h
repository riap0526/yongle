// A hooked or called function of the game's exe (image base 0x400000, no relocations): its address and the first bytes
// it has in the known build. Every hook is installed only after Matches() confirmed those bytes. Only the mechanism:
// every address is in the game layer (games/<game>/), which takes these names into its own namespace
// (games/<game>/site.h).
//
// Game variables are reached through Var<T> or At(), game functions through Function(site) or Original(...), all
// relative to the image: in the game the shift is 0; the offline tests of the failure fallbacks (each game's
// test_restore.cpp) put a copy of the game's address range in memory of their own and set the shift, so the detours
// run on that copy. One DLL serves one game, so there is one shift.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

#include "core/hook/hook.h"

namespace core::site {

// 0 in the game. Offline tests only: where their copy of the game's address range starts, minus 0x400000.
inline DWORD g_imageShift = 0;

// A variable of the game (e.g. const Var<int> kGameSpeed{0x...}; int s = *kGameSpeed;).
template <typename T> struct Var {
    DWORD address;
    volatile T *ptr() const
    {
        return reinterpret_cast<volatile T *>(address + g_imageShift);
    }
    volatile T &operator*() const
    {
        return *ptr();
    }
};

// The game's memory at one of its addresses (arrays and tables: e.g. At<unsigned char>(kTable)).
template <typename T> T *At(DWORD address)
{
    return reinterpret_cast<T *>(address + g_imageShift);
}

struct Site {
    const char *name;
    DWORD address;
    BYTE expect[8];
};

inline bool Matches(const Site &s)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void *>(s.address), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        return false;
    }
    return std::memcmp(reinterpret_cast<const void *>(s.address), s.expect, sizeof(s.expect)) == 0;
}

template <typename F> bool Install(const Site &s, F detour, F &original)
{
    return core_hook_install(s.name, reinterpret_cast<void *>(s.address), reinterpret_cast<void *>(detour),
                             reinterpret_cast<void **>(&original)) != 0;
}

template <typename F> F Function(const Site &s)
{
    return reinterpret_cast<F>(s.address + g_imageShift);
}

// The unhooked function: the trampoline once the hook is installed; before that (offline tests) the game's
// function itself.
template <typename F> F Original(F trampoline, const Site &s)
{
    return trampoline ? trampoline : Function<F>(s);
}

// A return address in the game's code, as the game's own address (for comparing with known call sites).
inline DWORD GameAddress(const void *p)
{
    return reinterpret_cast<DWORD>(p) - g_imageShift;
}

// Replaces size bytes of the game's code at address (the caller has checked what is there).
inline bool PatchCode(DWORD address, const BYTE *bytes, size_t size)
{
    void *p = reinterpret_cast<void *>(address);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(p, bytes, size);
    VirtualProtect(p, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    return true;
}

// A detour by the name of its site, for the offline tests (each module lists its detours).
struct Detour {
    const Site *site;
    void *detour;
};

} // namespace core::site
