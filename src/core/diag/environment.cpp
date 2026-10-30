#include "core/diag/environment.h"

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2 // GetProcessMemoryInfo and EnumProcessModules from kernel32 (K32...)
#endif
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace core::diag::env {

namespace {

// ---------------------------------------------------------------------------------------------
// Modules

// Modules of other programs that hook a game's drawing or presentation (named in the log when found).
struct Known {
    const wchar_t *file;
    const char *what;
};
const Known kKnown[] = {
    {L"gameoverlayrenderer.dll", "Steam overlay"},
    {L"discordhook.dll", "Discord overlay"},
    {L"rtsshooks.dll", "RivaTuner Statistics Server (also MSI Afterburner's overlay)"},
    {L"graphics-hook32.dll", "OBS game capture"},
    {L"nvspcap.dll", "NVIDIA ShadowPlay capture"},
    {L"reshade32.dll", "ReShade"},
    {L"fraps32.dll", "Fraps"},
    {L"dxgi.dll", "a DXGI wrapper (e.g. ReShade) when outside Windows' folder"},
    {L"d3d11.dll", "a Direct3D 11 wrapper when outside Windows' folder"},
    {L"d3d9.dll", "a Direct3D 9 wrapper when outside Windows' folder"},
};

wchar_t g_windowsDir[MAX_PATH] = L"";
size_t g_windowsLen = 0;

bool InWindows(const wchar_t *path)
{
    return g_windowsLen > 0 && _wcsnicmp(path, g_windowsDir, g_windowsLen) == 0 &&
           (path[g_windowsLen] == L'\\' || path[g_windowsLen] == L'/');
}

const wchar_t *FileName(const wchar_t *path)
{
    const wchar_t *name = path;
    for (const wchar_t *p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            name = p + 1;
        }
    }
    return name;
}

const char *KnownWhat(const wchar_t *path)
{
    const wchar_t *name = FileName(path);
    for (const Known &k : kKnown) {
        if (_wcsicmp(name, k.file) == 0) {
            return k.what;
        }
    }
    return nullptr;
}

void ToUtf8(const wchar_t *w, char *out, int size)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, nullptr, nullptr)) {
        out[0] = 0;
    }
}

// A module worth listing: not from Windows' folder, or a known overlay (the wrappers only count outside it).
bool Listed(const wchar_t *path)
{
    const char *what = KnownWhat(path);
    return !InWindows(path) || (what && std::strstr(what, "outside") == nullptr);
}

void LogModule(const char *how, const wchar_t *path)
{
    if (!Listed(path)) {
        return;
    }
    const char *what = KnownWhat(path);
    char utf8[MAX_PATH * 3];
    ToUtf8(path, utf8, sizeof(utf8));
    core_log("env: %s %s%s%s%s", how, utf8, what ? " (" : "", what ? what : "", what ? ")" : "");
}

void LogLoadedModules()
{
    HMODULE mods[512];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) {
        core_log("env: listing the modules failed (error %lu)", GetLastError());
        return;
    }
    const int n = static_cast<int>(bytes / sizeof(HMODULE)) < 512 ? static_cast<int>(bytes / sizeof(HMODULE)) : 512;
    int windows = 0;
    for (int i = 0; i < n; ++i) {
        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) {
            continue;
        }
        if (!Listed(path)) {
            ++windows;
            continue;
        }
        LogModule("module", path);
    }
    core_log("env: %d modules loaded at start-up, %d of them from %s (not listed)", n, windows, "Windows' folder");
}

// Modules loaded later (an overlay is often injected after start-up): the loader's notification copies the path
// here; TakeWindow logs it. The loader lock is held in the callback, so it only copies.
constexpr int kNotes = 32;
wchar_t g_notePath[kNotes][MAX_PATH];
std::atomic<int> g_noteWritten{0};
int g_noteLogged = 0; // TakeWindow's caller only
std::atomic<long> g_notesLost{0};

struct UnicodeStr {
    USHORT Length, MaximumLength;
    PWSTR Buffer;
};
struct DllNotificationData {
    ULONG Flags;
    const UnicodeStr *FullDllName;
    const UnicodeStr *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
constexpr ULONG kDllLoaded = 1;
using DllNotify = VOID(CALLBACK *)(ULONG reason, const DllNotificationData *data, PVOID context);
using LdrRegister_fn = LONG(NTAPI *)(ULONG flags, DllNotify fn, PVOID context, PVOID *cookie);
PVOID g_cookie = nullptr;

VOID CALLBACK OnDll(ULONG reason, const DllNotificationData *data, PVOID)
{
    if (reason != kDllLoaded || !data || !data->FullDllName || !data->FullDllName->Buffer) {
        return;
    }
    const int slot = g_noteWritten.load();
    if (slot - g_noteLogged >= kNotes) {
        g_notesLost.fetch_add(1);
        return;
    }
    wchar_t *dst = g_notePath[slot % kNotes];
    size_t len = data->FullDllName->Length / sizeof(wchar_t);
    len = len < MAX_PATH - 1 ? len : MAX_PATH - 1;
    std::memcpy(dst, data->FullDllName->Buffer, len * sizeof(wchar_t));
    dst[len] = 0;
    g_noteWritten.store(slot + 1); // the loader serialises loads, so there is one writer at a time
}

void LogNotedModules()
{
    const int written = g_noteWritten.load();
    for (; g_noteLogged < written; ++g_noteLogged) {
        LogModule("module loaded:", g_notePath[g_noteLogged % kNotes]);
    }
    if (long lost = g_notesLost.exchange(0)) {
        core_log("env: %ld module loads not recorded (too many at once)", lost);
    }
}

// ---------------------------------------------------------------------------------------------
// Power, priority and power throttling (EcoQoS: Task Manager's efficiency mode sets both)

struct Throttling {
    ULONG Version, ControlMask, StateMask;
};
constexpr int kProcessPowerThrottling = 4; // PROCESS_INFORMATION_CLASS
constexpr int kThreadPowerThrottling = 3;  // THREAD_INFORMATION_CLASS
using GetProcessInformation_fn = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
using GetThreadInformation_fn = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
GetProcessInformation_fn g_getProcessInformation = nullptr;
GetThreadInformation_fn g_getThreadInformation = nullptr;

// "on", "off" or "not set" for execution-speed throttling (bit 0, EcoQoS) in a throttling state.
const char *ThrottleWord(const Throttling &t)
{
    if (!(t.ControlMask & 1)) {
        return "not set (Windows decides)";
    }
    return (t.StateMask & 1) ? "on (EcoQoS)" : "off";
}

const char *PriorityWord(DWORD c)
{
    switch (c) {
    case IDLE_PRIORITY_CLASS: return "idle (efficiency mode sets this)";
    case BELOW_NORMAL_PRIORITY_CLASS: return "below normal";
    case NORMAL_PRIORITY_CLASS: return "normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "above normal";
    case HIGH_PRIORITY_CLASS: return "high";
    case REALTIME_PRIORITY_CLASS: return "realtime";
    default: return "unknown";
    }
}

struct ProcessState {
    DWORD priority = 0;
    char throttle[40] = "";
    int ac = -1;      // 1 on mains, 0 on battery, 255 unknown
    int saver = -1;   // battery saver (energy saver) on
    bool operator==(const ProcessState &o) const
    {
        return priority == o.priority && std::strcmp(throttle, o.throttle) == 0 && ac == o.ac && saver == o.saver;
    }
};
ProcessState g_state;
bool g_stateKnown = false;

ProcessState ReadState()
{
    ProcessState s;
    s.priority = GetPriorityClass(GetCurrentProcess());
    Throttling t{1, 0, 0};
    if (g_getProcessInformation &&
        g_getProcessInformation(GetCurrentProcess(), kProcessPowerThrottling, &t, sizeof(t))) {
        std::snprintf(s.throttle, sizeof(s.throttle), "%s", ThrottleWord(t));
    }
    else {
        std::snprintf(s.throttle, sizeof(s.throttle), "unknown");
    }
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        s.ac = ps.ACLineStatus;
        s.saver = ps.SystemStatusFlag;
    }
    return s;
}

void LogState(const char *how, const ProcessState &s)
{
    core_log("env: %s: priority class %s, power throttling %s, power %s%s", how, PriorityWord(s.priority), s.throttle,
             s.ac == 1 ? "AC" : s.ac == 0 ? "battery" : "unknown", s.saver == 1 ? ", energy saver on" : "");
}

// The Windows 11 power mode (Settings > Power) and the power plan, once at start-up.
void LogPowerPlan()
{
    HMODULE pp = LoadLibraryW(L"powrprof.dll");
    if (!pp) {
        return;
    }
    using Active_fn = DWORD(WINAPI *)(HKEY, GUID **);
    using Name_fn = DWORD(WINAPI *)(HKEY, const GUID *, const GUID *, const GUID *, UCHAR *, DWORD *);
    using Overlay_fn = DWORD(WINAPI *)(GUID *);
    auto active = reinterpret_cast<Active_fn>(GetProcAddress(pp, "PowerGetActiveScheme"));
    auto name = reinterpret_cast<Name_fn>(GetProcAddress(pp, "PowerReadFriendlyName"));
    auto overlay = reinterpret_cast<Overlay_fn>(GetProcAddress(pp, "PowerGetEffectiveOverlayScheme"));
    char plan[160] = "unknown";
    GUID *scheme = nullptr;
    if (active && active(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
        wchar_t w[64] = L"";
        DWORD size = sizeof(w);
        if (name && name(nullptr, scheme, nullptr, nullptr, reinterpret_cast<UCHAR *>(w), &size) == ERROR_SUCCESS) {
            ToUtf8(w, plan, sizeof(plan));
        }
        LocalFree(scheme);
    }
    char mode[64] = "not reported";
    GUID g{};
    if (overlay && overlay(&g) == ERROR_SUCCESS) {
        // The overlay schemes Windows uses for the power mode slider (powersetting.h).
        static const GUID kEfficiency = {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
        static const GUID kPerformance = {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
        static const GUID kBalanced = {};
        std::snprintf(mode, sizeof(mode), "%s", IsEqualGUID(g, kEfficiency)    ? "best power efficiency"
                                                : IsEqualGUID(g, kPerformance) ? "best performance"
                                                : IsEqualGUID(g, kBalanced)    ? "balanced"
                                                                               : "other");
    }
    core_log("env: power plan \"%s\", power mode %s", plan, mode);
    FreeLibrary(pp);
}

// ---------------------------------------------------------------------------------------------
// Processor time and kinds of cores

constexpr int kMaxCpus = 256;
unsigned char g_cpuClass[kMaxCpus]; // efficiency class by group * 64 + number (higher: faster cores)
unsigned char g_topClass = 0;
bool g_hybrid = false;

void ReadCpuClasses()
{
    using CpuSets_fn = BOOL(WINAPI *)(PVOID, ULONG, PULONG, HANDLE, ULONG);
    auto cpuSets = reinterpret_cast<CpuSets_fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation"));
    if (!cpuSets) {
        return;
    }
    static BYTE buffer[64 * 1024];
    ULONG length = 0;
    if (!cpuSets(buffer, sizeof(buffer), &length, GetCurrentProcess(), 0)) {
        return;
    }
    // SYSTEM_CPU_SET_INFORMATION: Size (DWORD), Type (DWORD), then CpuSet: Id (DWORD), Group (WORD),
    // LogicalProcessorIndex (BYTE), CoreIndex, LastLevelCacheIndex, NumaNodeIndex, EfficiencyClass (BYTEs).
    int lowest = 255, highest = -1;
    for (ULONG off = 0; off + 8 <= length;) {
        const BYTE *p = buffer + off;
        DWORD size = *reinterpret_cast<const DWORD *>(p);
        if (size < 20 || off + size > length) {
            break;
        }
        if (*reinterpret_cast<const DWORD *>(p + 4) == 0) { // CpuSetInformation
            const int group = *reinterpret_cast<const WORD *>(p + 12);
            const int index = p[14];
            const int cls = p[18];
            const int at = group * 64 + index;
            if (at < kMaxCpus) {
                g_cpuClass[at] = static_cast<unsigned char>(cls);
            }
            lowest = cls < lowest ? cls : lowest;
            highest = cls > highest ? cls : highest;
        }
        off += size;
    }
    g_hybrid = highest > lowest;
    g_topClass = static_cast<unsigned char>(highest < 0 ? 0 : highest);
    core_log("env: %lu logical processors%s", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
             g_hybrid ? "; hybrid (performance and efficiency cores): the cores our threads run on are counted" : "");
}

struct Measured {
    std::atomic<HANDLE> handle{nullptr};
    std::atomic<long> onTop{0}, onOther{0}; // NoteCore samples on the fastest class of cores / on others
    ULONGLONG lastCpu = 0;                   // TakeWindow's caller only
    bool lastValid = false;
    char throttle[40] = "";
    int priority = THREAD_PRIORITY_ERROR_RETURN;
};
Measured g_thread[3];
const char *const kName[] = {"game thread", "output thread", "screenshot writer"};

// ---------------------------------------------------------------------------------------------
// Processor affinity (O1, 2026-10-30): GOG's compatibility settings include SingleProcAffinity, which keeps the
// whole process on one logical processor (docs/optimization.md section 1.7). A thread's mask is always within the
// process's.

int Bits(DWORD_PTR mask)
{
    int n = 0;
    for (; mask; mask &= mask - 1) {
        ++n;
    }
    return n;
}

// A thread's affinity mask from NtQueryInformationThread (ThreadBasicInformation, read only); 0 when unknown.
DWORD_PTR ThreadMask(HANDLE h)
{
    struct Basic {
        LONG exitStatus;
        PVOID teb;
        HANDLE process, thread; // CLIENT_ID
        ULONG_PTR affinity;
        LONG priority, basePriority;
    };
    using Query_fn = LONG(NTAPI *)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query =
        reinterpret_cast<Query_fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    Basic b{};
    return h && query && query(h, 0, &b, sizeof(b), nullptr) == 0 ? b.affinity : 0;
}

std::atomic<DWORD_PTR> g_processMask{0}; // the process's mask as last logged
DWORD g_maskCheckedAt = 0;               // the game thread's last look (GetTickCount)

// "0x1 (1 of 20 logical processors), system 0xfffff"
void DescribeProcessMask(DWORD_PTR process, DWORD_PTR system, char *out, size_t size)
{
    std::snprintf(out, size, "0x%lx (%d of %d logical processors), system 0x%lx",
                  static_cast<unsigned long>(process), Bits(process), Bits(system), static_cast<unsigned long>(system));
}

// Logs a line when the process's mask is not the one last logged, with each measured thread's mask.
void CheckProcessMask()
{
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
        return;
    }
    const DWORD_PTR before = g_processMask.exchange(process);
    if (before == process) {
        return;
    }
    char text[160];
    DescribeProcessMask(process, system, text, sizeof(text));
    char threads[200] = "";
    size_t len = 0;
    for (int t = 0; t < 3; ++t) {
        if (HANDLE h = g_thread[t].handle.load()) {
            int w = std::snprintf(threads + len, sizeof(threads) - len, "%s%s 0x%lx", len ? ", " : "", kName[t],
                                  static_cast<unsigned long>(ThreadMask(h)));
            len += w > 0 && static_cast<size_t>(w) < sizeof(threads) - len ? static_cast<size_t>(w) : 0;
        }
    }
    core_log("env: process affinity changed from 0x%lx to %s; threads: %s", static_cast<unsigned long>(before), text,
             threads);
}

ULONGLONG Sum(const FILETIME &a, const FILETIME &b)
{
    return ((static_cast<ULONGLONG>(a.dwHighDateTime) << 32) | a.dwLowDateTime) +
           ((static_cast<ULONGLONG>(b.dwHighDateTime) << 32) | b.dwLowDateTime);
}

ULONGLONG g_lastWall = 0, g_lastProcess = 0, g_lastIdle = 0, g_lastBusyAll = 0;
DWORD g_lastResources = 0;

ULONGLONG WallNow() // 100 ns units, like the processor times
{
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

void LogResources()
{
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof(m);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m));
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    const double mb = 1024.0 * 1024.0;
    core_log("resources: working set %.0f MB (peak %.0f), private %.0f MB, address space used %.0f of %.0f MB, "
             "handles %lu, GDI objects %lu, USER objects %lu",
             m.WorkingSetSize / mb, m.PeakWorkingSetSize / mb, m.PrivateUsage / mb,
             (ms.ullTotalVirtual - ms.ullAvailVirtual) / mb, ms.ullTotalVirtual / mb, handles, gdi, user);
}

// O5c (docs/optimization.md, note 4): where our files really are. A 32-bit program without a manifest that
// writes to a protected folder (C:\Program Files (x86), where GOG Galaxy installs by default) is redirected by Windows
// to %LOCALAPPDATA%\VirtualStore\<the same path>: the log, emperor_engine.ini and shots\ are then there. Windows makes
// that folder only when something was redirected, so its existence tells.
void LogFileLocation()
{
    HMODULE self = nullptr;
    wchar_t folder[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LogFileLocation), &self)) {
        return;
    }
    DWORD len = GetModuleFileNameW(self, folder, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    while (len > 0 && folder[len - 1] != L'\\' && folder[len - 1] != L'/') {
        --len;
    }
    folder[len] = L'\0';
    char utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, folder, -1, utf8, sizeof(utf8), nullptr, nullptr);
    DWORD virt = 0, size = 0;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenVirtualizationEnabled, &virt, sizeof(virt), &size);
        CloseHandle(token);
    }
    wchar_t store[MAX_PATH * 2] = L"";
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const bool hasDrive = len > 3 && folder[1] == L':';
    if (n > 0 && n < MAX_PATH && hasDrive) {
        std::swprintf(store, MAX_PATH * 2, L"%ls\\VirtualStore\\%ls", local, folder + 3);
    }
    const DWORD attr = store[0] ? GetFileAttributesW(store) : INVALID_FILE_ATTRIBUTES;
    if (virt && attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        char where[MAX_PATH * 6];
        WideCharToMultiByte(CP_UTF8, 0, store, -1, where, sizeof(where), nullptr, nullptr);
        core_log("env: files: Windows redirects this folder's writes (file virtualization): the log, "
                 "emperor_engine.ini and shots\\ are in %s, not in %s (installing the game outside Program Files "
                 "avoids this)",
                 where, utf8);
    }
    else {
        core_log("env: files: the log, emperor_engine.ini and shots\\ are in %s (file virtualization %s)", utf8,
                 virt ? "on, but nothing of this folder is redirected" : "off");
    }
}

} // namespace

void Start()
{
    LogFileLocation();
    UINT n = GetWindowsDirectoryW(g_windowsDir, MAX_PATH);
    g_windowsLen = n < MAX_PATH ? n : 0;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    g_getProcessInformation =
        reinterpret_cast<GetProcessInformation_fn>(GetProcAddress(k32, "GetProcessInformation"));
    g_getThreadInformation = reinterpret_cast<GetThreadInformation_fn>(GetProcAddress(k32, "GetThreadInformation"));
    LogLoadedModules();
    auto reg = reinterpret_cast<LdrRegister_fn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    if (!reg || reg(0, &OnDll, nullptr, &g_cookie) != 0) {
        core_log("env: modules loaded later are not recorded (no loader notification)");
    }
    ReadCpuClasses();
    LogPowerPlan();
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
        char text[160];
        DescribeProcessMask(process, system, text, sizeof(text));
        core_log("env: affinity at start: process %s", text);
        g_processMask = process;
    }
    g_state = ReadState();
    g_stateKnown = true;
    LogState("start", g_state);
    g_lastResources = GetTickCount() - kResourceMs; // the first window logs them too
}

void RegisterThread(Thread which)
{
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        return;
    }
    Measured &m = g_thread[static_cast<int>(which)];
    if (HANDLE old = m.handle.exchange(h)) {
        CloseHandle(old);
    }
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    core_log("env: %s affinity 0x%lx (process 0x%lx)", kName[static_cast<int>(which)],
             static_cast<unsigned long>(ThreadMask(h)), static_cast<unsigned long>(process));
}

void NoteCore(Thread which)
{
    if (which == Thread::Game && GetTickCount() - g_maskCheckedAt >= 1000) {
        g_maskCheckedAt = GetTickCount();
        CheckProcessMask();
    }
    if (!g_hybrid) {
        return;
    }
    PROCESSOR_NUMBER pn{};
    GetCurrentProcessorNumberEx(&pn);
    const int at = pn.Group * 64 + pn.Number;
    Measured &m = g_thread[static_cast<int>(which)];
    if (at < kMaxCpus && g_cpuClass[at] == g_topClass) {
        m.onTop.fetch_add(1, std::memory_order_relaxed);
    }
    else {
        m.onOther.fetch_add(1, std::memory_order_relaxed);
    }
}

void TakeWindow(char *out, std::size_t size)
{
    if (!size) {
        return;
    }
    out[0] = 0;
    LogNotedModules();
    if (g_stateKnown) {
        ProcessState s = ReadState();
        if (!(s == g_state)) {
            LogState("changed", s);
            g_state = s;
        }
    }
    const ULONGLONG wall = WallNow();
    const ULONGLONG span = g_lastWall ? wall - g_lastWall : 0;
    size_t len = 0;
    auto put = [&](const char *fmt, auto... args) {
        if (len < size) {
            int w = std::snprintf(out + len, size - len, fmt, args...);
            len += w > 0 ? static_cast<size_t>(w) : 0;
        }
    };
    for (int t = 0; t < 3; ++t) {
        Measured &m = g_thread[t];
        HANDLE h = m.handle.load();
        if (!h) {
            continue;
        }
        FILETIME c, e, k, u;
        if (!GetThreadTimes(h, &c, &e, &k, &u)) {
            continue;
        }
        const ULONGLONG cpu = Sum(k, u);
        if (m.lastValid && span > 0) {
            put("%s%s %.0f%% of the time on a processor", len ? ", " : "", kName[t], 100.0 * (cpu - m.lastCpu) / span);
            const long top = m.onTop.exchange(0), other = m.onOther.exchange(0);
            if (g_hybrid && top + other > 0) {
                put(" (on performance cores %.0f%%)", 100.0 * top / (top + other));
            }
        }
        m.lastCpu = cpu;
        m.lastValid = true;
        // A thread's priority or power throttling changed (e.g. by another program): logged as it is seen.
        Throttling th{1, 0, 0};
        char word[40] = "not reported by Windows";
        if (g_getThreadInformation && g_getThreadInformation(h, kThreadPowerThrottling, &th, sizeof(th))) {
            std::snprintf(word, sizeof(word), "%s", ThrottleWord(th));
        }
        const int priority = GetThreadPriority(h);
        if (std::strcmp(word, m.throttle) != 0 || priority != m.priority) {
            core_log("env: %s: priority %d, power throttling %s", kName[t], priority, word);
            std::snprintf(m.throttle, sizeof(m.throttle), "%s", word);
            m.priority = priority;
        }
    }
    FILETIME c, e, k, u, idle, sk, su;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        const ULONGLONG process = Sum(k, u);
        if (g_lastProcess && span > 0) {
            put("%sprocess %.2f of %lu logical processors", len ? "; " : "", static_cast<double>(process - g_lastProcess) / span,
                GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            DWORD_PTR mask = 0, system = 0;
            if (GetProcessAffinityMask(GetCurrentProcess(), &mask, &system)) {
                put(" (affinity 0x%lx)", static_cast<unsigned long>(mask));
            }
        }
        g_lastProcess = process;
    }
    if (GetSystemTimes(&idle, &sk, &su)) { // kernel time includes the idle time
        const ULONGLONG i = Sum(idle, FILETIME{}), all = Sum(sk, su);
        if (g_lastBusyAll && all > g_lastBusyAll) {
            put(", system %.0f%% busy", 100.0 * (1.0 - static_cast<double>(i - g_lastIdle) / (all - g_lastBusyAll)));
        }
        g_lastIdle = i;
        g_lastBusyAll = all;
    }
    g_lastWall = wall;
    if (GetTickCount() - g_lastResources >= kResourceMs) {
        g_lastResources = GetTickCount();
        LogResources();
    }
}

} // namespace core::diag::env
