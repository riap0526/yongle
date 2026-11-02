// Offline test of core/platform/affinity (B plan, docs/archive/affinity.md section 7.1; not the game). Each scenario runs in a
// child process of its own (the feature starts once per process), with the process first put on one logical
// processor as GOG's SingleProcAffinity does:
//   kernel    what Windows does to a thread's own mask when the process's mask is widened or narrowed (printed)
//   choose    our mask for an original one (cores with and without a second hyperthread, one processor)
//   auto      Start: the other threads suspended and put back, ours on our mask, new threads (DLL_THREAD_ATTACH
//             simulated) on the original mask, a moved thread put back by the scan, three times -> reverted
//   external  another program changes the process's mask: no longer managed
//   original  [compat] cpu_affinity = original: nothing changes, graphics threads
//             included; no display driver query, no graphics line (as 0.1.2)
//   full      a process already on all processors: nothing to do
//   graphics  auto: the three ways a module counts as graphics (noted during the renderer's
//             setup, the display driver's (D3DKMT) or from its folder, the Direct3D runtime list); threads starting in
//             one -> ours, at their start and by the scan
//   emu       test_emu.exe (the DLL, with DLL_THREAD_ATTACH for real) started on one processor: the log lines, the
//             graphics modules found by the driver and the runtime list (the renderer's loads need
//             core/diag/environment's loader notification, which test_emu does not start)
// Usage: test_affinity.exe (all scenarios) | test_affinity.exe <scenario>
#include "core/log.h"
#include "core/platform/affinity.h"

#define WIN32_LEAN_AND_MEAN
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2 // GetModuleInformation from kernel32 (K32...)
#endif
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {

namespace aff = core::platform::affinity;

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("  %-74s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

DWORD_PTR ThreadMask(HANDLE h)
{
    struct Basic {
        LONG exitStatus;
        PVOID teb;
        HANDLE process, thread;
        ULONG_PTR affinity;
        LONG priority, basePriority;
    };
    using Query_fn = LONG(NTAPI *)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query_fn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    Basic b{};
    return query && query(h, 0, &b, sizeof(b), nullptr) == 0 ? b.affinity : 0;
}

DWORD_PTR ProcessMask()
{
    DWORD_PTR p = 0, s = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &p, &s);
    return p;
}

DWORD_PTR SystemMask()
{
    DWORD_PTR p = 0, s = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &p, &s);
    return s;
}

DWORD_PTR Lowest(DWORD_PTR m)
{
    return m & (~m + 1);
}

bool LogHas(const char *file, const char *text)
{
    static char buffer[1 << 20];
    FILE *f = std::fopen(file, "rb");
    if (!f) {
        return false;
    }
    size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);
    buffer[n] = 0;
    return std::strstr(buffer, text) != nullptr;
}

// A thread of "the game" (not ours): counts while it runs, so a thread left suspended would show.
struct Worker {
    HANDLE h = nullptr;
    std::atomic<long> count{0};
    std::atomic<bool> stop{false};
    bool busy = false;
    bool attach = false;    // simulates our DllMain's DLL_THREAD_ATTACH first
    const char *ours = nullptr; // our thread: OurThread(ours) after the attach
};

DWORD WINAPI WorkerMain(void *p)
{
    auto *w = static_cast<Worker *>(p);
    if (w->attach) {
        core_affinity_thread_attach();
    }
    if (w->ours) {
        aff::OurThread(w->ours);
    }
    while (!w->stop.load()) {
        w->count.fetch_add(1);
        if (!w->busy) {
            Sleep(1);
        }
    }
    if (w->attach) {
        core_affinity_thread_detach();
    }
    return 0;
}

void Run(Worker &w)
{
    w.h = CreateThread(nullptr, 0, &WorkerMain, &w, 0, nullptr);
    while (w.count.load() == 0) {
        Sleep(1);
    }
}

void Stop(Worker &w)
{
    w.stop = true;
    WaitForSingleObject(w.h, 5000);
    CloseHandle(w.h);
}

bool StillRuns(Worker &w)
{
    const long a = w.count.load();
    Sleep(30);
    return w.count.load() > a;
}

// The original mask: one processor, as GOG's compatibility settings do it.
DWORD_PTR NarrowToOne()
{
    const DWORD_PTR o = Lowest(SystemMask());
    SetProcessAffinityMask(GetCurrentProcess(), o);
    return o;
}

// ---------------------------------------------------------------------------------------------

int Kernel()
{
    const DWORD_PTR system = SystemMask(), o = NarrowToOne();
    Worker w;
    Run(w);
    SetThreadAffinityMask(w.h, o);
    SetProcessAffinityMask(GetCurrentProcess(), system);
    const DWORD_PTR afterWiden = ThreadMask(w.h), mainAfter = ThreadMask(GetCurrentThread());
    std::printf("  widening the process from 0x%lx to 0x%lx: a thread set to 0x%lx explicitly is then 0x%lx (%s); "
                "the calling thread 0x%lx\n",
                static_cast<unsigned long>(o), static_cast<unsigned long>(system), static_cast<unsigned long>(o),
                static_cast<unsigned long>(afterWiden),
                afterWiden == o ? "kept" : afterWiden == system ? "reset to the process's mask" : "other",
                static_cast<unsigned long>(mainAfter));
    Check(afterWiden != 0, "the thread's mask can be read after widening");
    SetThreadAffinityMask(w.h, system);
    SetProcessAffinityMask(GetCurrentProcess(), o);
    const DWORD_PTR afterNarrow = ThreadMask(w.h);
    std::printf("  narrowing the process back to 0x%lx: the thread on 0x%lx is then 0x%lx\n",
                static_cast<unsigned long>(o), static_cast<unsigned long>(system),
                static_cast<unsigned long>(afterNarrow));
    Check(afterNarrow != 0 && (afterNarrow & ~o) == 0, "narrowing the process brings every thread within it");
    Stop(w);
    return g_failures;
}

int Choose()
{
    const std::uintptr_t ht[] = {0x3, 0xc, 0x30};
    Check(aff::ChooseOurs(0x1, 0x3f, ht, 3) == 0x3c, "hyperthreads: the game's core and its sibling left out");
    const std::uintptr_t plain[] = {0x1, 0x2, 0x4, 0x8};
    Check(aff::ChooseOurs(0x1, 0xf, plain, 4) == 0xe, "no hyperthreads: only the game's processor left out");
    const std::uintptr_t one[] = {0x3};
    Check(aff::ChooseOurs(0x1, 0x3, one, 1) == 0x2, "one core with two hyperthreads: the other hyperthread");
    const std::uintptr_t single[] = {0x1};
    Check(aff::ChooseOurs(0x1, 0x1, single, 1) == 0, "one logical processor: none");
    Check(aff::ChooseOurs(0x1, 0xf, nullptr, 0) == 0xe, "no core information: without the original only");
    return g_failures;
}

// Our mask as Start chooses it (for the checks).
DWORD_PTR ExpectedOurs(DWORD_PTR o)
{
    static BYTE buffer[16 * 1024];
    DWORD length = sizeof(buffer);
    std::uintptr_t cores[64];
    int n = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                         reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer),
                                         &length)) {
        for (DWORD off = 0; off < length && n < 64;) {
            auto *p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer + off);
            for (WORD g = 0; g < p->Processor.GroupCount; ++g) {
                if (p->Processor.GroupMask[g].Group == 0) {
                    cores[n++] = p->Processor.GroupMask[g].Mask;
                }
            }
            off += p->Size;
        }
    }
    return aff::ChooseOurs(o, SystemMask(), cores, n);
}

void PollAfterASecond()
{
    Sleep(1100);
    aff::Poll();
}

int Auto()
{
    core_log_open(nullptr, "test_affinity_auto.log");
    const DWORD_PTR o = NarrowToOne(), w = ExpectedOurs(o);
    Worker idle, busy, early;
    busy.busy = true;
    early.ours = "early";
    Run(idle);
    Run(busy);
    Run(early); // ours before Start, like the stall watchdog
    aff::Start(aff::Mode::Auto);
    Check(std::strcmp(aff::StateName(), "on") == 0, "Start: the feature is on");
    Check(ProcessMask() == (o | w), "the process is on the original and our processors");
    Check(ThreadMask(idle.h) == o && ThreadMask(busy.h) == o, "the other threads are on the original processor");
    Check(ThreadMask(GetCurrentThread()) == o, "the starting (game) thread is on the original processor");
    Check(ThreadMask(early.h) == w, "our thread from before Start is on our processors");
    Check(StillRuns(idle) && StillRuns(busy) && StillRuns(early), "every thread runs again after the suspension");
    Worker late, lateOurs;
    late.attach = true;
    lateOurs.attach = true;
    lateOurs.ours = "late";
    Run(late);
    Run(lateOurs);
    Check(ThreadMask(late.h) == o, "a new thread (DLL_THREAD_ATTACH) is on the original processor");
    Check(ThreadMask(lateOurs.h) == w, "a new thread of ours is on our processors");
    aff::Poll(); // the first scan
    for (int i = 1; i <= 3; ++i) {
        SetThreadAffinityMask(idle.h, o | w);
        PollAfterASecond();
        char what[96];
        std::snprintf(what, sizeof(what), "a thread moved to all processors is put back (%d)", i);
        if (i < 3) {
            Check(ThreadMask(idle.h) == o && std::strcmp(aff::StateName(), "on") == 0, what);
        }
    }
    Check(std::strcmp(aff::StateName(), "reverted") == 0, "moved a third time: the feature reverts");
    Check(ProcessMask() == o, "reverted: the process is back on the original processor");
    Check((ThreadMask(lateOurs.h) & ~o) == 0 && (ThreadMask(early.h) & ~o) == 0,
          "reverted: our threads are within the original processor too");
    Check(StillRuns(idle) && StillRuns(lateOurs), "reverted: the threads run on");
    Stop(idle);
    Stop(busy);
    Stop(early);
    Stop(late);
    Stop(lateOurs);
    core_log_close();
    Check(LogHas("test_affinity_auto.log", "other threads suspended for"), "log: the suspension");
    Check(LogHas("test_affinity_auto.log", "(start "), "log: one line per thread with its start");
    Check(LogHas("test_affinity_auto.log", "put back on"), "log: a thread put back");
    Check(LogHas("test_affinity_auto.log", "keeps being moved"), "log: why it reverted");
    Check(LogHas("test_affinity_auto.log", "the process is back on"), "log: reverted");
    return g_failures;
}

int External()
{
    core_log_open(nullptr, "test_affinity_external.log");
    const DWORD_PTR o = NarrowToOne(), w = ExpectedOurs(o);
    if (w == Lowest(w)) {
        std::printf("  (skipped: our mask has a single processor)\n");
        return 0;
    }
    Worker idle;
    Run(idle);
    aff::Start(aff::Mode::Auto);
    aff::Poll();
    SetProcessAffinityMask(GetCurrentProcess(), o | Lowest(w)); // as Task Manager would
    PollAfterASecond();
    Check(std::strcmp(aff::StateName(), "external") == 0, "another program's mask: no longer managed");
    SetThreadAffinityMask(idle.h, o | Lowest(w));
    PollAfterASecond();
    Check(ThreadMask(idle.h) == (o | Lowest(w)), "then a moved thread is left where it is");
    Stop(idle);
    core_log_close();
    Check(LogHas("test_affinity_external.log", "by another program"), "log: changed by another program");
    char line[400];
    aff::TakeWindow(line, sizeof(line));
    Check(std::strstr(line, "left to another program") != nullptr, "cpu line: left to another program");
    return g_failures;
}

int Original()
{
    core_log_open(nullptr, "test_affinity_original.log");
    const DWORD_PTR o = NarrowToOne();
    Worker idle;
    Run(idle);
    aff::Start(aff::Mode::Original); // graphics threads included: nothing moves
    Check(std::strcmp(aff::StateName(), "off") == 0 && ProcessMask() == o, "original: nothing changes");
    Check(aff::DriverPath(0) == nullptr, "original: the display drivers are not even queried");
    // This program "loaded while the renderer was made": its threads would be graphics ones under auto.
    aff::BeginGraphicsSetup();
    aff::NoteModuleLoaded(L"test_affinity.exe", 17);
    aff::EndGraphicsSetup();
    Worker late, graphics;
    late.attach = true;
    graphics.attach = true;
    Run(late);
    Run(graphics);
    Check(ThreadMask(late.h) == o, "original: a new thread keeps the process's mask");
    Check(ThreadMask(graphics.h) == o, "original: a graphics thread keeps the process's mask too");
    PollAfterASecond();
    Check(ThreadMask(graphics.h) == o && ThreadMask(idle.h) == o && ProcessMask() == o,
          "original: no scan moves anything");
    SetThreadAffinityMask(idle.h, o); // as the game might: left alone
    PollAfterASecond();
    Check(std::strcmp(aff::StateName(), "off") == 0, "original: still off");
    Stop(idle);
    Stop(late);
    Stop(graphics);
    core_log_close();
    Check(LogHas("test_affinity_original.log", "original ([compat] cpu_affinity)"), "log: original");
    Check(!LogHas("test_affinity_original.log", "graphics modules") &&
              !LogHas("test_affinity_original.log", "display drivers"),
          "log: no graphics or driver lines");
    return g_failures;
}

int Full()
{
    core_log_open(nullptr, "test_affinity_full.log");
    aff::Start(aff::Mode::Auto);
    Check(std::strcmp(aff::StateName(), "off") == 0 && ProcessMask() == SystemMask(),
          "a process on all processors: nothing to do");
    core_log_close();
    Check(LogHas("test_affinity_full.log", "nothing to do"), "log: nothing to do");
    return g_failures;
}

int Graphics()
{
    core_log_open(nullptr, "test_affinity_graphics.log");
    const DWORD_PTR o = NarrowToOne(), w = ExpectedOurs(o);
    aff::Start(aff::Mode::Auto);
    // The runtime list and the display driver (D3DKMT) need no renderer.
    Check(aff::GraphicsHow(L"C:\\Windows\\System32\\D3D11.dll") == 4 && aff::GraphicsHow(L"C:\\x\\dxgi.dll") == 4 &&
              aff::GraphicsHow(L"C:\\x\\d3d9.dll") == 4 && aff::GraphicsHow(L"C:\\x\\dcomp.dll") == 4,
          "runtime list: d3d9, d3d11, dxgi, dcomp (any folder, any case)");
    Check(aff::GraphicsHow(L"C:\\Game\\mss32.dll") == 0 && aff::GraphicsHow(L"C:\\Windows\\System32\\DSOUND.DLL") == 0,
          "Miles and DirectSound are not graphics");
    if (const wchar_t *d = aff::DriverPath(0)) {
        std::printf("  display driver: %ls\n", d);
        for (int i = 1; aff::DriverPath(i); ++i) {
            std::printf("  display driver: %ls\n", aff::DriverPath(i));
        }
        Check((aff::GraphicsHow(d) & 2) != 0, "driver: its DLL");
        const wchar_t *slash = std::wcsrchr(d, L'\\');
        if (slash) {
            wchar_t other[MAX_PATH], deeper[MAX_PATH];
            std::swprintf(other, MAX_PATH, L"%.*lsHELPER32.DLL", static_cast<int>(slash - d + 1), d);
            std::swprintf(deeper, MAX_PATH, L"%.*lssub\\helper32.dll", static_cast<int>(slash - d + 1), d);
            Check(aff::GraphicsHow(other) == 2, "driver: another module from its folder");
            Check(aff::GraphicsHow(deeper) == 0, "driver: not one from a folder below it");
        }
    }
    else {
        std::printf("  (no display driver reported here: the driver checks skipped)\n");
    }
    // A module "loaded while the renderer was made": this program, so the workers start in it.
    aff::BeginGraphicsSetup();
    aff::NoteModuleLoaded(L"test_affinity.exe", 17);
    aff::NoteModuleLoaded(L"gone_again.dll", 14); // loaded and unloaded again, as resourcepolicyclient.dll in D3D9
    aff::EndGraphicsSetup();
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    Check(aff::GraphicsHow(self) == 1, "renderer: a module noted while the renderer was made");
    Worker atStart, plain;
    atStart.attach = true;
    Run(atStart); // DLL_THREAD_ATTACH: straight on ours
    Check(ThreadMask(atStart.h) == w, "a graphics thread is on our processors from its start");
    Run(plain); // no attach: the process's whole mask until the scan
    aff::Poll();
    Check(ThreadMask(plain.h) == w, "the scan puts a graphics thread on our processors");
    // A thread in a module that is none of the three: kernel32's Sleep.
    auto *sleep = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep")));
    HANDLE other = CreateThread(nullptr, 0, sleep, reinterpret_cast<void *>(3000), 0, nullptr);
    PollAfterASecond();
    Check(ThreadMask(other) == o, "any other new thread is put back on the original processor");
    Check(std::strcmp(aff::StateName(), "on") == 0, "the feature stays on");
    WaitForSingleObject(other, 5000);
    CloseHandle(other);
    Stop(atStart);
    Stop(plain);
    core_log_close();
    Check(LogHas("test_affinity_graphics.log", "affinity: graphics modules (renderer:"), "log: the graphics modules");
    Check(LogHas("test_affinity_graphics.log", "test_affinity.exe (renderer)"), "log: a module with how it was found");
    Check(LogHas("test_affinity_graphics.log", "gone_again.dll (renderer; unloaded again)"),
          "log: a module unloaded again, by name");
    Check(LogHas("test_affinity_graphics.log", "affinity: display drivers (D3DKMT"), "log: the display drivers");
    return g_failures;
}

// The DLL itself, with DllMain's DLL_THREAD_ATTACH: test_emu.exe on one processor (as GOG starts the game).
int Emu()
{
    DeleteFileW(L"ddraw_proxy.log");
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"test_emu.exe .\\ddraw.dll panel";
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        Check(false, "test_emu.exe starts");
        return g_failures;
    }
    SetProcessAffinityMask(pi.hProcess, Lowest(SystemMask()));
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Check(code == 0, "test_emu.exe passes on one processor");
    const char *log = "ddraw_proxy.log";
    char stay[64];
    std::snprintf(stay, sizeof(stay), "affinity: auto: the game's threads stay on 0x%lx;",
                  static_cast<unsigned long>(Lowest(SystemMask())));
    Check(LogHas(log, stay), "log: auto, the game's threads on the one processor it was started on");
    Check(LogHas(log, "other threads suspended for"), "log: the suspension");
    Check(LogHas(log, "; the game thread) 0x"), "log: the game thread's line");
    Check(LogHas(log, "affinity: our output thread"), "log: our output thread on our processors");
    Check(!LogHas(log, "at their start so far: 0)"), "DLL_THREAD_ATTACH reaches our DllMain");
    Check(LogHas(log, "affinity: display drivers (D3DKMT"), "log: the display drivers");
    Check(LogHas(log, "affinity: graphics modules (renderer:") && LogHas(log, "d3d11.dll (runtime)"),
          "log: the graphics modules (found by the runtime list without the loader notification)");
    Check(LogHas(log, "their threads on our mask"), "graphics threads on our mask by default");
    // (The modules loaded during the renderer's setup come from the loader notification of core/diag/environment,
    // which test_emu does not start; the graphics scenario covers that part.)
    Check(!LogHas(log, "the process is back on"), "not reverted");
    return g_failures;
}

struct Scenario {
    const char *name;
    int (*run)();
};
const Scenario kScenarios[] = {{"kernel", &Kernel},     {"choose", &Choose}, {"auto", &Auto},
                               {"external", &External}, {"original", &Original}, {"full", &Full},
                               {"graphics", &Graphics}, {"emu", &Emu}};

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1) {
        for (const Scenario &s : kScenarios) {
            if (std::strcmp(argv[1], s.name) == 0) {
                if (SystemMask() == Lowest(SystemMask()) && std::strcmp(s.name, "choose") != 0) {
                    std::printf("  (skipped: one logical processor)\n");
                    return 0;
                }
                const int r = s.run();
                std::fflush(stdout);
                return r ? 1 : 0;
            }
        }
        return 2;
    }
    int failed = 0;
    for (const Scenario &s : kScenarios) {
        std::printf("%s\n", s.name);
        std::fflush(stdout);
        wchar_t self[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::swprintf(cmd, MAX_PATH + 32, L"\"%ls\" %hs", self, s.name);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES; // the child prints into the same output
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        DWORD code = 1;
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 180000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (code != 0) {
            std::printf("  scenario %s FAILED (exit %lu)\n", s.name, code);
            ++failed;
        }
    }
    std::printf("RESULT: %s (%d scenario(s) failed)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
