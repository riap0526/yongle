#include "core/platform/affinity.h"

#include "core/diag/guard.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace core::platform::affinity {

namespace {

enum State : int { kOff, kBinding, kOn, kExternal, kReverted };
const char *const kStateName[] = {"off", "binding", "on", "external", "reverted"};

std::atomic<int> g_state{kOff};
std::atomic<DWORD_PTR> g_original{0}; // O: the process's mask at start (docs/archive/affinity.md section 3.1)
std::atomic<DWORD_PTR> g_ours{0};     // W: our threads' mask
std::atomic<long> g_attached{0}; // threads put on a mask as they started (DLL_THREAD_ATTACH)
bool g_started = false;          // Start ran (game thread)
DWORD g_gameTid = 0;
HANDLE g_gameThread = nullptr;
std::atomic<DWORD> g_onSince{0}; // GetTickCount when the feature went on (the scan's rate)
DWORD g_lastScan = 0;            // the watchdog's last scan
core::diag::Feature g_feature{"processor affinity (core/platform)"};

// ---------------------------------------------------------------------------------------------
// System calls resolved before anything is suspended (nothing below allocates or takes a lock).

using Query_fn = LONG(NTAPI *)(HANDLE, int, PVOID, ULONG, PULONG);
std::atomic<Query_fn> g_query{nullptr};

void Resolve()
{
    if (!g_query.load()) {
        g_query = reinterpret_cast<Query_fn>(
            reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    }
}

// A thread's affinity mask (ThreadBasicInformation); 0 when unknown.
DWORD_PTR ThreadMask(HANDLE h)
{
    struct Basic {
        LONG exitStatus;
        PVOID teb;
        HANDLE process, thread; // CLIENT_ID
        ULONG_PTR affinity;
        LONG priority, basePriority;
    };
    Query_fn query = g_query.load();
    Basic b{};
    return h && query && query(h, 0, &b, sizeof(b), nullptr) == 0 ? b.affinity : 0;
}

// Where a thread started (ThreadQuerySetWin32StartAddress); nullptr when unknown.
void *StartAddress(HANDLE h)
{
    Query_fn query = g_query.load();
    void *p = nullptr;
    return h && query && query(h, 9, &p, sizeof(p), nullptr) == 0 ? p : nullptr;
}

int Bits(DWORD_PTR mask)
{
    int n = 0;
    for (; mask; mask &= mask - 1) {
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------
// The allow-list: our own threads (fixed slots, no lock; a slot is freed when its thread ends)

struct Ours {
    std::atomic<DWORD> tid{0};
    std::atomic<const char *> name{nullptr};
};
constexpr int kOursMax = 16; // the output, the screenshot writer, the watchdog, the city background, parallel workers
Ours g_oursList[kOursMax];
std::atomic<bool> g_listFullLogged{false};

const char *OursName(DWORD tid)
{
    for (Ours &o : g_oursList) {
        if (o.tid.load() == tid) {
            const char *n = o.name.load();
            return n ? n : "ours";
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Names for the log

void ModuleOf(const void *p, char *out, size_t size)
{
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH];
    if (p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(p), &m) &&
        GetModuleFileNameW(m, path, MAX_PATH)) {
        const wchar_t *name = path;
        for (const wchar_t *c = path; *c; ++c) {
            if (*c == L'\\' || *c == L'/') {
                name = c + 1;
            }
        }
        char utf8[MAX_PATH];
        if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr)) {
            utf8[0] = 0;
        }
        std::snprintf(out, size, "%s+0x%lx", utf8,
                      static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(p) -
                                                 reinterpret_cast<std::uintptr_t>(m)));
        return;
    }
    std::snprintf(out, size, p ? "0x%p" : "unknown", p);
}

// "amdxx32.dll" of "amdxx32.dll+0x1234"
void ModuleName(const char *where, char *out, size_t size)
{
    std::snprintf(out, size, "%s", where);
    if (char *plus = std::strchr(out, '+')) {
        *plus = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// The graphics stack (docs/archive/affinity.md section 5): a thread that starts in one of these modules is a graphics thread.
// Found three ways, by module name (a module unloaded meanwhile leaves nothing behind that a later one could match):
//   renderer: first loaded on the output thread while it made the renderer (the loader's notification);
//   driver:   the display driver's user-mode DLLs for D3D9 and D3D10/11 (D3DKMTQueryAdapterInfo,
//             KMTQAITYPE_UMDRIVERNAME), and every module from the same folder (the driver package);
//   runtime:  Microsoft's Direct3D runtime modules (kRuntime), which an overlay may have loaded before the renderer.

enum How : unsigned { kHowRenderer = 1, kHowDriver = 2, kHowRuntime = 4 };

// Microsoft's graphics runtime: Direct3D 9 and 11, DXGI, DirectComposition; the D3D9-on-12 mapping layer with the
// Direct3D 12 runtime it runs on; WARP, the software Direct3D driver.
const wchar_t *const kRuntime[] = {L"d3d9.dll",     L"d3d11.dll", L"dxgi.dll",      L"dcomp.dll",
                                   L"d3d9on12.dll", L"d3d12.dll", L"d3d12core.dll", L"d3d10warp.dll"};

constexpr int kNameLen = 64;
constexpr int kSetupNames = 32;
wchar_t g_setupNames[kSetupNames][kNameLen]; // written in the loader's notification (one at a time), then counted
std::atomic<int> g_setupCount{0};
int g_setupLogged = 0;        // output thread only
bool g_graphicsLogged = false; // output thread only
thread_local bool t_graphicsSetup = false;

// Written by Start before the feature goes on (g_state), read after.
constexpr int kDrivers = 8;
struct Driver {
    wchar_t path[MAX_PATH]; // as D3DKMT gives it (a full path, or a bare name such as d3d10warp.dll)
    unsigned versions;      // 1 D3D9, 2 D3D10, 4 D3D11
};
Driver g_drivers[kDrivers];
int g_driverCount = 0;

wchar_t Fold(wchar_t c)
{
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

// Equal ignoring ASCII case, over at most n characters.
bool SameText(const wchar_t *a, const wchar_t *b, size_t n = static_cast<size_t>(-1))
{
    for (size_t i = 0; i < n; ++i) {
        if (Fold(a[i]) != Fold(b[i])) {
            return false;
        }
        if (!a[i]) {
            return true;
        }
    }
    return true;
}

const wchar_t *BaseName(const wchar_t *path)
{
    const wchar_t *name = path;
    for (const wchar_t *c = path; *c; ++c) {
        if (*c == L'\\' || *c == L'/') {
            name = c + 1;
        }
    }
    return name;
}

// How a module (its full path) counts as graphics; 0: it does not. Any thread (under the loader lock as well).
unsigned HowOfPath(const wchar_t *path)
{
    const wchar_t *name = BaseName(path);
    unsigned how = 0;
    const int n = g_setupCount.load();
    for (int i = 0; i < n && i < kSetupNames; ++i) {
        if (SameText(name, g_setupNames[i])) {
            how |= kHowRenderer;
        }
    }
    for (int i = 0; i < g_driverCount; ++i) {
        const wchar_t *d = g_drivers[i].path, *dn = BaseName(d);
        const size_t dir = static_cast<size_t>(dn - d); // with the backslash; 0 for a bare name
        if (SameText(name, dn) || (dir && name == path + dir && SameText(path, d, dir))) {
            how |= kHowDriver;
        }
    }
    for (const wchar_t *r : kRuntime) {
        if (SameText(name, r)) {
            how |= kHowRuntime;
        }
    }
    return how;
}

// How the module a thread starts in counts as graphics.
unsigned HowOfStart(const void *start)
{
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH];
    if (!start ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(start), &m)) {
        return 0;
    }
    const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    return n && n < MAX_PATH ? HowOfPath(path) : 0;
}

// "renderer, driver"
void HowText(unsigned how, char *out, size_t size)
{
    std::snprintf(out, size, "%s%s%s%s%s", how & kHowRenderer ? "renderer" : "",
                  how & kHowRenderer && how & (kHowDriver | kHowRuntime) ? ", " : "", how & kHowDriver ? "driver" : "",
                  how & kHowDriver && how & kHowRuntime ? ", " : "", how & kHowRuntime ? "runtime" : "");
}

// The display drivers' user-mode DLLs of every adapter (Start, game thread; not under the loader lock). Logged.
void QueryDrivers()
{
    using D3DKMT_HANDLE = UINT;
    struct AdapterInfo {
        D3DKMT_HANDLE adapter;
        LUID luid;
        ULONG sources;
        BOOL precisePresentRegionsPreferred;
    };
    struct EnumAdapters2 {
        ULONG count;
        AdapterInfo *adapters;
    };
    struct QueryInfo {
        D3DKMT_HANDLE adapter;
        UINT type; // KMTQAITYPE_UMDRIVERNAME = 1
        VOID *data;
        UINT size;
    };
    struct UmdFileName {
        UINT version; // KMTUMDVERSION_DX9 0, DX10 1, DX11 2
        WCHAR name[MAX_PATH];
    };
    struct CloseAdapter {
        D3DKMT_HANDLE adapter;
    };
    using Enum_fn = LONG(APIENTRY *)(EnumAdapters2 *);
    using QueryInfo_fn = LONG(APIENTRY *)(const QueryInfo *);
    using Close_fn = LONG(APIENTRY *)(const CloseAdapter *);
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    const auto enumerate = reinterpret_cast<Enum_fn>(
        reinterpret_cast<void *>(gdi ? GetProcAddress(gdi, "D3DKMTEnumAdapters2") : nullptr));
    const auto query = reinterpret_cast<QueryInfo_fn>(
        reinterpret_cast<void *>(gdi ? GetProcAddress(gdi, "D3DKMTQueryAdapterInfo") : nullptr));
    const auto close = reinterpret_cast<Close_fn>(
        reinterpret_cast<void *>(gdi ? GetProcAddress(gdi, "D3DKMTCloseAdapter") : nullptr));
    static AdapterInfo adapters[16];
    EnumAdapters2 e{16, adapters};
    const LONG status = enumerate && query && close ? enumerate(&e) : -1;
    if (status != 0) {
        core_log("affinity: the display drivers cannot be listed (D3DKMTEnumAdapters2 0x%lx); graphics modules are "
                 "found only as the renderer loads them and by the runtime list",
                 static_cast<unsigned long>(status));
        return;
    }
    const ULONG count = e.count < 16 ? e.count : 16;
    for (ULONG i = 0; i < count; ++i) {
        for (UINT v = 0; v < 3; ++v) {
            static UmdFileName u;
            u = UmdFileName{};
            u.version = v;
            QueryInfo q{adapters[i].adapter, 1, &u, sizeof(u)};
            if (query(&q) != 0 || !u.name[0]) {
                continue;
            }
            u.name[MAX_PATH - 1] = 0;
            int k = 0;
            while (k < g_driverCount && !SameText(g_drivers[k].path, u.name)) {
                ++k;
            }
            if (k == g_driverCount) {
                if (k == kDrivers) {
                    continue;
                }
                std::memcpy(g_drivers[k].path, u.name, sizeof(u.name));
                g_drivers[k].versions = 0;
                ++g_driverCount;
            }
            g_drivers[k].versions |= v == 0 ? 1u : v == 1 ? 2u : 4u;
        }
        CloseAdapter c{adapters[i].adapter};
        close(&c);
    }
    char list[1536] = "";
    size_t len = 0;
    for (int k = 0; k < g_driverCount; ++k) {
        char path[MAX_PATH * 2];
        if (!WideCharToMultiByte(CP_UTF8, 0, g_drivers[k].path, -1, path, sizeof(path), nullptr, nullptr)) {
            path[0] = 0;
        }
        const unsigned v = g_drivers[k].versions;
        const int w = std::snprintf(list + len, sizeof(list) - len, "%s%s (%s%s%s%s%s)", len ? "; " : "", path,
                                    v & 1 ? "D3D9" : "", v & 1 && v & 6 ? ", " : "", v & 2 ? "D3D10" : "",
                                    v & 2 && v & 4 ? ", " : "", v & 4 ? "D3D11" : "");
        len += w > 0 && static_cast<size_t>(w) < sizeof(list) - len ? static_cast<size_t>(w) : 0;
    }
    core_log("affinity: display drivers (D3DKMT, %lu adapters; with every module from their folders): %s",
             static_cast<unsigned long>(count), len ? list : "none");
}

// The mask a thread belongs on: ours for our threads and graphics threads, else the original.
DWORD_PTR Expected(DWORD tid, void *start, bool *graphics)
{
    *graphics = tid != g_gameTid && start && HowOfStart(start) != 0;
    return OursName(tid) || *graphics ? g_ours.load() : g_original.load();
}

// ---------------------------------------------------------------------------------------------
// Threads seen by the scan: processor time and how often each was put back

ULONGLONG Sum(const FILETIME &a, const FILETIME &b)
{
    return ((static_cast<ULONGLONG>(a.dwHighDateTime) << 32) | a.dwLowDateTime) +
           ((static_cast<ULONGLONG>(b.dwHighDateTime) << 32) | b.dwLowDateTime);
}

ULONGLONG Time(const FILETIME &f)
{
    return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

ULONGLONG WallNow()
{
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return Time(f);
}

struct Tracked {
    DWORD tid;
    ULONGLONG created; // with tid: the same thread (thread ids are reused)
    ULONGLONG lastCpu;
    char module[40];
    int putBack;
    bool seen;
};
constexpr int kTracked = 512;
Tracked g_tracked[kTracked]; // under g_scanLock
int g_trackedCount = 0;
SRWLOCK g_scanLock = SRWLOCK_INIT;
ULONGLONG g_lastScanWall = 0;
long g_putBackLogged = 0;

// Per rate-log window (under g_statsLock): processor time by group, in 100 ns, over the wall time the scans covered.
struct Group {
    char name[40];
    ULONGLONG cpu;
    int threads; // at the last scan
};
constexpr int kGroups = 10;
struct Stats {
    ULONGLONG wall = 0, game = 0, ours = 0, graphics = 0;
    int oursThreads = 0, graphicsThreads = 0, otherThreads = 0;
    Group others[kGroups] = {};
    int groupCount = 0;
    long putBack = 0; // since the last window
};
Stats g_stats;
SRWLOCK g_statsLock = SRWLOCK_INIT;

void Revert(const char *why, DWORD error);

// The group named name; the last one is "other" when they run out.
Group *FindGroup(Group *groups, int &count, const char *name)
{
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(groups[i].name, name) == 0) {
            return &groups[i];
        }
    }
    if (count < kGroups - 1) {
        std::snprintf(groups[count].name, sizeof(groups[count].name), "%s", name);
        return &groups[count++];
    }
    if (count == kGroups - 1) {
        std::snprintf(groups[count].name, sizeof(groups[count].name), "other");
        ++count;
    }
    return &groups[kGroups - 1];
}

// One pass over the process's threads (the watchdog, or Start once on): each thread that is neither ours nor a
// graphics one and not on the original mask is put back on it. listAll: one line per thread.
void Scan(bool listAll)
{
    AcquireSRWLockExclusive(&g_scanLock);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&g_scanLock);
        core_log("affinity: listing the threads failed (error %lu); tried again at the next scan", GetLastError());
        return;
    }
    const DWORD pid = GetCurrentProcessId();
    const ULONGLONG wall = WallNow();
    for (int i = 0; i < g_trackedCount; ++i) {
        g_tracked[i].seen = false;
    }
    Stats add;
    const char *failure = nullptr;
    DWORD failureError = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL more = Thread32First(snap, &te); more && !failure; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) {
            continue;
        }
        const DWORD tid = te.th32ThreadID;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, FALSE, tid);
        if (!h) {
            continue; // ended meanwhile
        }
        void *start = StartAddress(h);
        bool graphics = false;
        const DWORD_PTR expected = Expected(tid, start, &graphics);
        const char *ourName = OursName(tid);
        DWORD_PTR mask = ThreadMask(h);
        FILETIME c{}, e{}, k{}, u{};
        const bool times = GetThreadTimes(h, &c, &e, &k, &u) != 0;
        // Find (or add) this thread.
        Tracked *t = nullptr;
        for (int i = 0; i < g_trackedCount && !t; ++i) {
            if (g_tracked[i].tid == tid && g_tracked[i].created == Time(c)) {
                t = &g_tracked[i];
            }
        }
        if (!t) {
            for (int i = 0; i < g_trackedCount && !t; ++i) {
                if (!g_tracked[i].seen && g_tracked[i].tid == tid) {
                    t = &g_tracked[i]; // the same id, a new thread
                }
            }
            if (!t && g_trackedCount < kTracked) {
                t = &g_tracked[g_trackedCount++];
            }
            if (t) {
                t->tid = tid;
                t->created = Time(c);
                t->lastCpu = times ? Sum(k, u) : 0;
                t->putBack = 0;
                ModuleOf(start, t->module, sizeof(t->module));
            }
        }
        if (mask && mask != expected && !ourName) {
            const DWORD_PTR was = mask;
            if (!SetThreadAffinityMask(h, expected)) {
                failure = "putting a thread back on its mask failed";
                failureError = GetLastError();
            }
            else if (!graphics) {
                mask = expected;
                ++add.putBack;
                const int times_ = t ? ++t->putBack : 1;
                if (g_putBackLogged < 16) {
                    ++g_putBackLogged;
                    core_log("affinity: thread %lu (start %s) was on 0x%lx; put back on 0x%lx%s", tid,
                             t ? t->module : "?", static_cast<unsigned long>(was), static_cast<unsigned long>(expected),
                             g_putBackLogged == 16 ? " (further ones are counted in the cpu line)" : "");
                }
                if (times_ >= 3) {
                    failure = "a thread keeps being moved off the original mask (3 times)";
                }
            }
            else {
                mask = expected;
            }
        }
        if (listAll) {
            core_log("affinity: thread %lu (start %s; %s) 0x%lx", tid, t ? t->module : "?",
                     tid == g_gameTid ? "the game thread" : ourName ? ourName : graphics ? "graphics" : "other",
                     static_cast<unsigned long>(mask));
        }
        if (t) {
            t->seen = true;
            const ULONGLONG cpu = times ? Sum(k, u) : t->lastCpu;
            const ULONGLONG d = cpu >= t->lastCpu ? cpu - t->lastCpu : 0;
            t->lastCpu = cpu;
            if (tid == g_gameTid) {
                add.game += d;
            }
            else if (ourName) {
                add.ours += d;
                ++add.oursThreads;
            }
            else if (graphics) {
                add.graphics += d;
                ++add.graphicsThreads;
            }
            else {
                ++add.otherThreads;
                char name[40];
                ModuleName(t->module, name, sizeof(name));
                Group *g = FindGroup(add.others, add.groupCount, name);
                g->cpu += d;
                ++g->threads;
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    // Threads that ended: their slots are reused.
    int kept = 0;
    for (int i = 0; i < g_trackedCount; ++i) {
        if (g_tracked[i].seen) {
            g_tracked[kept++] = g_tracked[i];
        }
    }
    g_trackedCount = kept;
    const ULONGLONG span = g_lastScanWall && wall > g_lastScanWall ? wall - g_lastScanWall : 0;
    const bool first = g_lastScanWall == 0;
    g_lastScanWall = wall;
    ReleaseSRWLockExclusive(&g_scanLock);
    if (!first) { // the first scan only sets the starting points
        AcquireSRWLockExclusive(&g_statsLock);
        g_stats.wall += span;
        g_stats.game += add.game;
        g_stats.ours += add.ours;
        g_stats.graphics += add.graphics;
        g_stats.oursThreads = add.oursThreads;
        g_stats.graphicsThreads = add.graphicsThreads;
        g_stats.otherThreads = add.otherThreads;
        for (int j = 0; j < g_stats.groupCount; ++j) {
            g_stats.others[j].threads = 0; // counted again from this scan
        }
        for (int i = 0; i < add.groupCount; ++i) {
            Group *g = FindGroup(g_stats.others, g_stats.groupCount, add.others[i].name);
            g->cpu += add.others[i].cpu;
            g->threads += add.others[i].threads;
        }
        g_stats.putBack += add.putBack;
        ReleaseSRWLockExclusive(&g_statsLock);
    }
    if (failure) {
        Revert(failure, failureError);
    }
}

// ---------------------------------------------------------------------------------------------
// Widening (docs/archive/affinity.md section 3.3)

struct Victim {
    HANDLE h;
    DWORD tid;
    bool suspended;
    DWORD_PTR widened; // the thread's mask after the process was widened (before it was put back)
};
constexpr int kVictims = 256;
Victim g_victims[kVictims]; // game thread, during Start
int g_victimCount = 0;

bool Ended(HANDLE h)
{
    return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
}

void ResumeVictims()
{
    for (int i = 0; i < g_victimCount; ++i) {
        if (g_victims[i].suspended) {
            ResumeThread(g_victims[i].h);
            g_victims[i].suspended = false;
        }
    }
}

void CloseVictims()
{
    for (int i = 0; i < g_victimCount; ++i) {
        CloseHandle(g_victims[i].h);
    }
    g_victimCount = 0;
}

struct WidenResult {
    const char *failed = nullptr; // the step that failed
    DWORD error = 0;
    int suspended = 0, reset = 0; // threads suspended; of them, the ones Windows had moved to the wide mask
    DWORD_PTR selfWidened = 0;
    double microseconds = 0;
};

// Step 2: every thread but this one and ours, opened (may allocate: nothing is suspended yet).
bool CollectVictims(WidenResult &r)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        r.failed = "listing the threads";
        r.error = GetLastError();
        return false;
    }
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    bool ok = true;
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || OursName(te.th32ThreadID)) {
            continue;
        }
        if (g_victimCount >= kVictims) {
            r.failed = "more than 256 threads";
            ok = false;
            break;
        }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                              FALSE, te.th32ThreadID);
        if (!h) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) {
                continue; // ended meanwhile
            }
            r.failed = "opening a thread";
            r.error = GetLastError();
            ok = false;
            break;
        }
        g_victims[g_victimCount++] = Victim{h, te.th32ThreadID, false, 0};
    }
    CloseHandle(snap);
    return ok;
}

// Step 3, with every victim suspended: system calls only -- no allocation, no log, no lock (a suspended thread may
// hold the heap's, the loader's or our log's). The results go into g_victims and r; the caller logs them after.
void WidenSuspended(WidenResult &r)
{
    const DWORD_PTR original = g_original.load(), ours = g_ours.load();
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < g_victimCount && !r.failed; ++i) {
        Victim &v = g_victims[i];
        if (SuspendThread(v.h) == static_cast<DWORD>(-1)) {
            if (!Ended(v.h)) {
                r.failed = "suspending a thread";
                r.error = GetLastError();
            }
            continue;
        }
        v.suspended = true;
        ++r.suspended;
    }
    if (!r.failed && !SetProcessAffinityMask(GetCurrentProcess(), original | ours)) {
        r.failed = "widening the process";
        r.error = GetLastError();
    }
    for (int i = 0; i < g_victimCount && !r.failed; ++i) {
        Victim &v = g_victims[i];
        if (!v.suspended) {
            continue;
        }
        v.widened = ThreadMask(v.h);
        if (v.widened != original) {
            ++r.reset;
        }
        if (!SetThreadAffinityMask(v.h, original) || ThreadMask(v.h) != original) {
            if (!Ended(v.h)) {
                r.failed = "putting a thread on the original mask";
                r.error = GetLastError();
            }
        }
    }
    if (!r.failed) {
        r.selfWidened = ThreadMask(GetCurrentThread());
        if (!SetThreadAffinityMask(GetCurrentThread(), original) || ThreadMask(GetCurrentThread()) != original) {
            r.failed = "putting the game thread on the original mask";
            r.error = GetLastError();
        }
    }
    if (r.failed) {
        SetProcessAffinityMask(GetCurrentProcess(), original); // back before anything runs again
    }
    ResumeVictims();
    QueryPerformanceCounter(&t1);
    r.microseconds = 1e6 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
}

void LogReverted(const char *why, DWORD error)
{
    const DWORD_PTR original = g_original.load();
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    char err[32] = "";
    if (error) {
        std::snprintf(err, sizeof(err), " (error %lu)", error);
    }
    core_log("affinity: %s%s; the process is back on 0x%lx as without this feature (process 0x%lx, game thread 0x%lx)",
             why, err, static_cast<unsigned long>(original), static_cast<unsigned long>(process),
             static_cast<unsigned long>(ThreadMask(g_gameThread)));
}

// docs/archive/affinity.md section 3.7: the process back on the original mask (each thread's mask has to stay within it).
void Revert(const char *why, DWORD error)
{
    int s = g_state.load();
    while (s == kBinding || s == kOn) {
        if (g_state.compare_exchange_weak(s, kReverted)) {
            if (!SetProcessAffinityMask(GetCurrentProcess(), g_original.load())) {
                core_log("affinity: putting the process back on 0x%lx failed too (error %lu); our threads may stay "
                         "on other processors (the game runs on)",
                         static_cast<unsigned long>(g_original.load()), GetLastError());
            }
            LogReverted(why, error);
            return;
        }
    }
}

void StartGuarded(void *)
{
    WidenResult r;
    if (!CollectVictims(r)) {
        CloseVictims();
        Revert(r.failed, r.error); // nothing was changed
        return;
    }
    WidenSuspended(r);
    CloseVictims();
    const DWORD_PTR original = g_original.load(), ours = g_ours.load();
    if (r.failed) {
        Revert(r.failed, r.error);
        return;
    }
    core_log("affinity: %d other threads suspended for %.0f us while the process was widened to 0x%lx; %d of them had "
             "then been moved by Windows to a wider mask (the game thread: 0x%lx); all put back on 0x%lx",
             r.suspended, r.microseconds, static_cast<unsigned long>(original | ours), r.reset,
             static_cast<unsigned long>(r.selfWidened), static_cast<unsigned long>(original));
    g_onSince = GetTickCount() ? GetTickCount() : 1;
    g_state = kOn;
    // Step 5: ours that run already (the stall watchdog) on our mask.
    for (Ours &o : g_oursList) {
        if (DWORD tid = o.tid.load()) {
            if (HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid)) {
                SetThreadAffinityMask(h, ours);
                CloseHandle(h);
            }
        }
    }
    // Steps 4 and 6: threads made between the listing and the suspension, and one line per thread.
    Scan(true);
}

} // namespace

std::uintptr_t ChooseOurs(std::uintptr_t original, std::uintptr_t system, const std::uintptr_t *cores, int count)
{
    std::uintptr_t without = system & ~original;
    for (int i = 0; i < count; ++i) {
        if (cores[i] & original) {
            without &= ~cores[i];
        }
    }
    if (without) {
        return without;
    }
    return system & ~original;
}

unsigned GraphicsHow(const wchar_t *path)
{
    return path ? HowOfPath(path) : 0;
}

const wchar_t *DriverPath(int i)
{
    return i >= 0 && i < g_driverCount ? g_drivers[i].path : nullptr;
}

int ProcessorsApart()
{
    if (g_state.load() == kOn) {
        return Bits(g_ours.load());
    }
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
        return 0;
    }
    const int n = Bits(process);
    return n > 1 ? n - 1 : 0;
}

const char *StateName()
{
    const int s = g_state.load();
    return s >= 0 && s <= kReverted ? kStateName[s] : "?";
}

void Start(Mode mode)
{
    if (g_started) {
        return;
    }
    g_started = true;
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
        core_log("affinity: the process's mask cannot be read (error %lu); left as it is", GetLastError());
        return;
    }
    g_original = process;
    if (mode == Mode::Original) {
        core_log("affinity: original ([compat] cpu_affinity): the process keeps 0x%lx (%d of %d logical processors)",
                 static_cast<unsigned long>(process), Bits(process), Bits(system));
        return;
    }
    if ((process & system) == system) {
        core_log("affinity: auto: the process already runs on all %d logical processors (0x%lx); nothing to do",
                 Bits(system), static_cast<unsigned long>(system));
        return;
    }
    // Physical cores of processor group 0 (a 32-bit process sees one group).
    static BYTE buffer[16 * 1024];
    DWORD length = sizeof(buffer);
    std::uintptr_t cores[64];
    int coreCount = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                         reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer),
                                         &length)) {
        for (DWORD off = 0; off < length && coreCount < 64;) {
            auto *p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer + off);
            if (p->Size == 0) {
                break;
            }
            if (p->Relationship == RelationProcessorCore) {
                for (WORD g = 0; g < p->Processor.GroupCount; ++g) {
                    if (p->Processor.GroupMask[g].Group == 0) {
                        cores[coreCount++] = p->Processor.GroupMask[g].Mask;
                    }
                }
            }
            off += p->Size;
        }
    }
    const std::uintptr_t ours = ChooseOurs(process, system, cores, coreCount);
    if (!ours) {
        core_log("affinity: auto: no processor besides 0x%lx; left as it is", static_cast<unsigned long>(process));
        return;
    }
    bool sibling = false;
    for (int i = 0; i < coreCount; ++i) {
        sibling = sibling || ((cores[i] & process) && (cores[i] & ~process));
    }
    Resolve();
    g_gameTid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_gameThread,
                    THREAD_QUERY_INFORMATION, FALSE, 0);
    g_ours = ours;
    QueryDrivers(); // before the feature goes on: read by the threads afterwards
    core_log("affinity: auto: the game's threads stay on 0x%lx; ours and the graphics threads use 0x%lx (%d of %d "
             "logical processors%s)",
             static_cast<unsigned long>(process), static_cast<unsigned long>(ours), Bits(ours), Bits(system),
             sibling && (ours & process) == 0 && Bits(ours) < Bits(system & ~process)
                 ? "; the game's core and its other hyperthread left out"
                 : "");
    g_state = kBinding; // from now on DLL_THREAD_ATTACH puts each new thread on the original mask
    if (!core::diag::Guarded(g_feature, &StartGuarded, nullptr)) {
        ResumeVictims(); // abandoned after an exception: nothing may stay suspended
        CloseVictims();
        Revert("an exception while widening (logged above)", 0);
    }
}

void OurThread(const char *name)
{
    const DWORD tid = GetCurrentThreadId();
    bool listed = false;
    for (Ours &o : g_oursList) {
        DWORD expected = 0;
        if (o.tid.compare_exchange_strong(expected, tid)) {
            o.name = name; // until then the scan calls it "ours"
            listed = true;
            break;
        }
    }
    if (!listed) {
        if (!g_listFullLogged.exchange(true)) {
            core_log("affinity: no room on the list for our %s thread; it stays on the original mask", name);
        }
        return;
    }
    if (g_state.load() == kOn) {
        const DWORD_PTR ours = g_ours.load();
        SetThreadAffinityMask(GetCurrentThread(), ours);
        core_log("affinity: our %s thread (%lu) on 0x%lx (threads put on a mask at their start so far: %ld)", name,
                 tid, static_cast<unsigned long>(ours), g_attached.load());
    }
}

void Poll()
{
    if (g_state.load() != kOn) {
        return;
    }
    const DWORD now = GetTickCount();
    const DWORD interval = now - g_onSince.load() < 60000 ? 1000 : 5000;
    if (now - g_lastScan < interval) {
        return;
    }
    g_lastScan = now;
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) &&
        process != (g_original.load() | g_ours.load())) {
        int s = kOn;
        if (g_state.compare_exchange_strong(s, kExternal)) {
            core_log("affinity: the process's mask was changed to 0x%lx by another program (e.g. Task Manager); "
                     "left as it is: threads are no longer put back",
                     static_cast<unsigned long>(process));
        }
        return;
    }
    if (!core::diag::Guarded(g_feature, [](void *) { Scan(false); }, nullptr)) {
        Revert("an exception while scanning the threads (logged above)", 0);
    }
}

void BeginGraphicsSetup()
{
    t_graphicsSetup = true;
}

void EndGraphicsSetup()
{
    t_graphicsSetup = false;
    const int n = g_setupCount.load() < kSetupNames ? g_setupCount.load() : kSetupNames;
    const int state = g_state.load();
    const bool first = !g_graphicsLogged;
    if ((!first && n <= g_setupLogged) || (state != kOn && state != kBinding)) {
        return; // nothing new (a renderer made again), or the feature does not run
    }
    g_graphicsLogged = true;
    static char list[2048];
    list[0] = 0;
    size_t len = 0;
    auto add = [&](const wchar_t *name, unsigned how, const char *more) {
        char utf8[kNameLen * 2], text[40];
        if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr)) {
            utf8[0] = 0;
        }
        HowText(how, text, sizeof(text));
        const int w = std::snprintf(list + len, sizeof(list) - len, "%s%s (%s%s)", len ? ", " : "", utf8, text, more);
        len += w > 0 && static_cast<size_t>(w) < sizeof(list) - len ? static_cast<size_t>(w) : 0;
    };
    // The loaded modules that count as graphics: the first time all of them; later the renderer's new ones.
    static HMODULE modules[512];
    DWORD bytes = 0;
    bool loaded[kSetupNames] = {};
    if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
        for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 512; ++i) {
            wchar_t path[MAX_PATH];
            const DWORD l = GetModuleFileNameW(modules[i], path, MAX_PATH);
            if (!l || l >= MAX_PATH) {
                continue;
            }
            const unsigned how = HowOfPath(path);
            bool fresh = false;
            for (int k = 0; k < n; ++k) {
                if (SameText(BaseName(path), g_setupNames[k])) {
                    loaded[k] = true;
                    fresh = k >= g_setupLogged;
                }
            }
            if (how && (first || fresh)) {
                add(BaseName(path), how, "");
            }
        }
    }
    for (int k = g_setupLogged; k < n; ++k) {
        if (!loaded[k]) {
            add(g_setupNames[k], HowOfPath(g_setupNames[k]), "; unloaded again");
        }
    }
    g_setupLogged = n;
    core_log("affinity: graphics modules%s (renderer: first loaded while the renderer was made; driver: the display "
             "driver's or from its folder; runtime: the Direct3D runtime list; their threads on our mask): %s",
             first ? "" : " added by a renderer made again", list);
}

void NoteModuleLoaded(const wchar_t *name, unsigned long length)
{
    if (!t_graphicsSetup || !name) {
        return;
    }
    const int n = g_setupCount.load();
    if (n >= kSetupNames) {
        return;
    }
    const unsigned long l = length < kNameLen - 1 ? length : kNameLen - 1;
    for (int i = 0; i < n; ++i) {
        if (SameText(g_setupNames[i], name, l) && g_setupNames[i][l] == 0) {
            return; // loaded again (a renderer made again)
        }
    }
    std::memcpy(g_setupNames[n], name, l * sizeof(wchar_t));
    g_setupNames[n][l] = 0;
    g_setupCount = n + 1; // the loader serialises loads: one writer at a time
}

void TakeWindow(char *out, std::size_t size)
{
    if (!size) {
        return;
    }
    out[0] = 0;
    const int s = g_state.load();
    if (s == kExternal) {
        std::snprintf(out, size, "; affinity: left to another program");
        return;
    }
    if (s != kOn) {
        return;
    }
    AcquireSRWLockExclusive(&g_statsLock);
    Stats st = g_stats;
    g_stats.wall = g_stats.game = g_stats.ours = g_stats.graphics = 0;
    for (int i = 0; i < g_stats.groupCount; ++i) {
        g_stats.others[i].cpu = 0;
    }
    g_stats.putBack = 0;
    ReleaseSRWLockExclusive(&g_statsLock);
    if (st.wall == 0) {
        return;
    }
    const double w = static_cast<double>(st.wall);
    ULONGLONG others = 0;
    for (int i = 0; i < st.groupCount; ++i) {
        others += st.others[i].cpu;
    }
    size_t len = 0;
    auto put = [&](const char *fmt, auto... args) {
        if (len < size) {
            int n = std::snprintf(out + len, size - len, fmt, args...);
            len += n > 0 ? static_cast<size_t>(n) : 0;
        }
    };
    put("; affinity: on 0x%lx the game thread %.0f%% and %d others %.0f%%", static_cast<unsigned long>(g_original.load()),
        100.0 * st.game / w, st.otherThreads, 100.0 * others / w);
    // The busiest modules first.
    bool used[kGroups] = {};
    int listed = 0;
    for (int k = 0; k < st.groupCount && listed < 5; ++k) {
        int best = -1;
        for (int i = 0; i < st.groupCount; ++i) {
            if (!used[i] && (best < 0 || st.others[i].cpu > st.others[best].cpu)) {
                best = i;
            }
        }
        used[best] = true;
        if (100.0 * st.others[best].cpu / w < 0.5 && listed > 0) {
            break;
        }
        put("%s%s %.0f%% in %d", listed ? ", " : " (", st.others[best].name, 100.0 * st.others[best].cpu / w,
            st.others[best].threads);
        ++listed;
    }
    if (listed) {
        put("%s", ")");
    }
    put(", ours on 0x%lx %.0f%% in %d", static_cast<unsigned long>(g_ours.load()), 100.0 * st.ours / w,
        st.oursThreads);
    put(", graphics on 0x%lx %.0f%% in %d", static_cast<unsigned long>(g_ours.load()), 100.0 * st.graphics / w,
        st.graphicsThreads);
    put("; threads put back: %ld (at their start %ld)", st.putBack, g_attached.load());
}

} // namespace core::platform::affinity

extern "C" void core_affinity_thread_attach(void)
{
    using namespace core::platform::affinity;
    const int s = g_state.load();
    if (s != kBinding && s != kOn) {
        return;
    }
    DWORD_PTR mask = g_original.load();
    if (s == kOn && HowOfStart(StartAddress(GetCurrentThread())) != 0) {
        mask = g_ours.load();
    }
    SetThreadAffinityMask(GetCurrentThread(), mask);
    g_attached.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void core_affinity_thread_detach(void)
{
    using namespace core::platform::affinity;
    const DWORD tid = GetCurrentThreadId();
    for (Ours &o : g_oursList) {
        DWORD expected = tid;
        if (o.tid.compare_exchange_strong(expected, 0)) {
            o.name = nullptr;
        }
    }
}
