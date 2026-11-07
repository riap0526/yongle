// Programs known to draw on the screen (core/diag/screen_programs.h).
#include "core/diag/screen_programs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace core::diag::screen_programs {

namespace {

struct Entry {
    const wchar_t *exe;
    const char *name;
};

// Executable names as the programs ship them (2026-11; the names are from the vendors' installs as commonly reported,
// not checked on every version [assumed]). Several entries may belong to one program.
constexpr Entry kList[] = {
    {L"RadeonSoftware.exe", "AMD Software"},
    {L"AMDRSServ.exe", "AMD Software"},
    {L"amdow.exe", "AMD Software"},
    {L"AMDRSSrcExt.exe", "AMD Software"},
    {L"GameBar.exe", "Xbox Game Bar"},
    {L"GameBarFTServer.exe", "Xbox Game Bar"},
    {L"Discord.exe", "Discord"},
    {L"RTSS.exe", "RivaTuner Statistics Server"},
    {L"MSIAfterburner.exe", "MSI Afterburner"},
    {L"wallpaper32.exe", "Wallpaper Engine"},
    {L"wallpaper64.exe", "Wallpaper Engine"},
    {L"obs64.exe", "OBS Studio"},
    {L"obs32.exe", "OBS Studio"},
    {L"NVIDIA Overlay.exe", "NVIDIA overlay"},
    {L"NVIDIA Share.exe", "NVIDIA overlay"},
    {L"nvsphelper64.exe", "NVIDIA overlay"},
    {L"gameoverlayui.exe", "Steam overlay"},
    {L"Apollo.exe", "Apollo"},
    {L"sunshine.exe", "Sunshine or Apollo"},
    {L"PowerToys.exe", "PowerToys"},
    {L"PowerToys.AlwaysOnTop.exe", "PowerToys Always On Top"},
};

} // namespace

const char *Match(const wchar_t *exe)
{
    if (!exe) {
        return nullptr;
    }
    for (const Entry &e : kList) {
        if (_wcsicmp(e.exe, exe) == 0) {
            return e.name;
        }
    }
    return nullptr;
}

bool Find(char *out, std::size_t size, int *processes)
{
    out[0] = 0;
    *processes = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        std::snprintf(out, size, "not known (the process list could not be read, error %lu)", GetLastError());
        return false;
    }
    bool found[sizeof(kList) / sizeof(kList[0])] = {};
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        ++*processes;
        for (std::size_t i = 0; i < sizeof(kList) / sizeof(kList[0]); ++i) {
            if (_wcsicmp(kList[i].exe, pe.szExeFile) == 0) {
                found[i] = true;
            }
        }
    }
    CloseHandle(snap);
    // One entry per program (the list's entries with the same name together): "name (a.exe, b.exe)".
    constexpr std::size_t kCount = sizeof(kList) / sizeof(kList[0]);
    std::size_t used = 0;
    auto put = [&](const char *fmt, auto... args) {
        if (used + 1 >= size) {
            return;
        }
        const int n = std::snprintf(out + used, size - used, fmt, args...);
        used = n > 0 && used + n < size ? used + n : size - 1;
    };
    bool done[kCount] = {};
    for (std::size_t i = 0; i < kCount; ++i) {
        if (!found[i] || done[i]) {
            continue;
        }
        put("%s%s (", used ? ", " : "", kList[i].name);
        bool firstExe = true;
        for (std::size_t j = i; j < kCount; ++j) {
            if (found[j] && !done[j] && std::strcmp(kList[j].name, kList[i].name) == 0) {
                put("%s%ls", firstExe ? "" : ", ", kList[j].exe);
                firstExe = false;
                done[j] = true;
            }
        }
        put("%s", ")");
    }
    if (used == 0) {
        std::snprintf(out, size, "none of the list");
    }
    return true;
}

} // namespace core::diag::screen_programs
