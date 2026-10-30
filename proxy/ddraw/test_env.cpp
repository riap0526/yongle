// Offline test of core/diag/environment (not the game; O1a, docs/optimization.md): the start-up lines (modules from
// outside Windows' folder, processors, power, priority), a module loaded later, the processor time of a busy and an
// idle thread, and the resources line. Writes test_env.log next to the exe and checks its lines.
#include "core/diag/environment.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

namespace env = core::diag::env;

HANDLE g_idleReady = nullptr, g_idleStop = nullptr;

DWORD WINAPI Idle(void *)
{
    env::RegisterThread(env::Thread::Output);
    SetEvent(g_idleReady);
    WaitForSingleObject(g_idleStop, INFINITE);
    return 0;
}

void Busy(DWORD ms)
{
    const DWORD start = GetTickCount();
    volatile unsigned x = 0;
    while (GetTickCount() - start < ms) {
        for (int i = 0; i < 10000; ++i) {
            x = x * 1664525u + 1013904223u;
        }
        env::NoteCore(env::Thread::Game);
    }
}

// The log's text (it is written as lines arrive).
char *ReadLog(char *buffer, size_t size)
{
    buffer[0] = 0;
    FILE *f = std::fopen("test_env.log", "rb");
    if (!f) {
        return buffer;
    }
    size_t n = std::fread(buffer, 1, size - 1, f);
    buffer[n] = 0;
    std::fclose(f);
    return buffer;
}

} // namespace

int main()
{
    DeleteFileA("test_env.log"); // the log appends
    core_log_open(nullptr, "test_env.log");
    env::Start();
    env::RegisterThread(env::Thread::Game);
    g_idleReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_idleStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE idle = CreateThread(nullptr, 0, &Idle, nullptr, 0, nullptr);
    WaitForSingleObject(g_idleReady, INFINITE);

    char line[512];
    env::TakeWindow(line, sizeof(line)); // the first window only starts the counting
    Busy(1500);
    HMODULE later = LoadLibraryW(L"dbghelp.dll"); // from Windows' folder: noted, not listed
    env::TakeWindow(line, sizeof(line));
    std::printf("cpu: %s\n", line);
    int game = -1, output = -1;
    if (const char *g = std::strstr(line, "game thread ")) {
        game = std::atoi(g + 12);
    }
    if (const char *o = std::strstr(line, "output thread ")) {
        output = std::atoi(o + 14);
    }
    Check(game >= 80, "busy game thread: most of the time on a processor");
    Check(output >= 0 && output <= 5, "idle output thread: almost none");
    Check(std::strstr(line, "logical processors") && std::strstr(line, "% busy"), "process and system shares");

    static char log[256 * 1024];
    ReadLog(log, sizeof(log));
    Check(std::strstr(log, "env: module ") && std::strstr(log, "test_env.exe"),
          "start-up: modules from outside Windows' folder listed (the test itself)");
    Check(std::strstr(log, "modules loaded at start-up") != nullptr, "start-up: module count");
    Check(std::strstr(log, "logical processors") && std::strstr(log, "env: power plan") &&
              std::strstr(log, "env: start: priority class normal"),
          "start-up: processors, power plan, priority class");
    Check(std::strstr(log, "env: game thread: priority 0") && std::strstr(log, "env: output thread: priority 0"),
          "each thread's priority and power throttling, once");
    Check(std::strstr(log, "env: files: the log, emperor_engine.ini and shots") != nullptr,
          "start-up: where our files are (O5c; this folder is not redirected)");
    Check(std::strstr(log, "resources: working set") && std::strstr(log, "GDI objects"), "resources line");
    Check(!std::strstr(log, "dbghelp"), "a module from Windows' folder loaded later is not listed");

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    env::TakeWindow(line, sizeof(line));
    ReadLog(log, sizeof(log));
    Check(std::strstr(log, "env: changed: priority class below normal") &&
              std::strstr(log, "env: game thread: priority -1"),
          "a change of the priority class or a thread's priority is logged");

    // The process's affinity (O1, 2026-10-30): at start, in the cpu line, and a line when it changes (as Task Manager
    // does), seen by the game thread within a second.
    Check(std::strstr(log, "env: affinity at start: process 0x") && std::strstr(log, "env: game thread affinity 0x") &&
              std::strstr(log, "env: output thread affinity 0x"),
          "start-up: the process's and each thread's affinity");
    Check(std::strstr(line, "logical processors (affinity 0x") != nullptr, "the cpu line has the process's affinity");
    DWORD_PTR all = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &all, &system);
    const DWORD_PTR one = all & (~all + 1); // the lowest processor of the process
    SetProcessAffinityMask(GetCurrentProcess(), one);
    Busy(1100);
    SetProcessAffinityMask(GetCurrentProcess(), all);
    Busy(1100);
    ReadLog(log, sizeof(log));
    char changed[96];
    std::snprintf(changed, sizeof(changed), "env: process affinity changed from 0x%lx to 0x%lx (1 of ",
                  static_cast<unsigned long>(all), static_cast<unsigned long>(one));
    Check(std::strstr(log, changed) && std::strstr(log, "; threads: game thread 0x"), "a change to one processor is logged");
    std::snprintf(changed, sizeof(changed), "env: process affinity changed from 0x%lx to 0x%lx (",
                  static_cast<unsigned long>(one), static_cast<unsigned long>(all));
    Check(std::strstr(log, changed) != nullptr, "and the change back");

    SetEvent(g_idleStop);
    WaitForSingleObject(idle, INFINITE);
    if (later) {
        FreeLibrary(later);
    }
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    core_log_close();
    return g_failures ? 1 : 0;
}
