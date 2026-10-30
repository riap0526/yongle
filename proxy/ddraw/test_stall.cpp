// Offline test of core/diag/stall (not the game; O1, docs/optimization.md §1.4): a frame of 200 ms is not sampled; a
// frame of 1.5 s is, with both threads' registers, the frame-pointer chain and the stack scan logged after they run
// on, and its end is logged. Writes test_stall.log next to the exe and checks its lines.
#include "core/diag/stall.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
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

namespace stall = core::diag::stall;

__attribute__((noinline)) void StuckHere(DWORD ms)
{
    Sleep(ms);
    __asm__ volatile("" ::: "memory"); // keeps the call to Sleep a call (not a tail jump)
}

DWORD WINAPI Output(void *)
{
    stall::RegisterThread(stall::Thread::Output);
    stall::Begin(stall::Thread::Output); // a short frame: not a stall
    StuckHere(200);
    stall::End(stall::Thread::Output);
    stall::Begin(stall::Thread::Output); // a long one
    StuckHere(1500);
    stall::End(stall::Thread::Output);
    return 0;
}

char *ReadLog(char *buffer, size_t size)
{
    buffer[0] = 0;
    FILE *f = std::fopen("test_stall.log", "rb");
    if (!f) {
        return buffer;
    }
    size_t n = std::fread(buffer, 1, size - 1, f);
    buffer[n] = 0;
    std::fclose(f);
    return buffer;
}

int Count(const char *text, const char *what)
{
    int n = 0;
    for (const char *p = std::strstr(text, what); p; p = std::strstr(p + 1, what)) {
        ++n;
    }
    return n;
}

} // namespace

int main()
{
    DeleteFileA("test_stall.log"); // the log appends
    core_log_open(nullptr, "test_stall.log");
    stall::RegisterThread(stall::Thread::Game); // registered, not in a pass
    stall::Start();
    HANDLE t = CreateThread(nullptr, 0, &Output, nullptr, 0, nullptr);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    Sleep(300);
    static char log[1 << 16];
    ReadLog(log, sizeof(log));
    Check(Count(log, "stall: the output thread has been in one frame for") == 1, "one stall reported (the 1.5 s frame, not the 200 ms one)");
    Check(Count(log, "stall: the game thread has been") == 0, "the game thread (not in a pass) is not reported");
    Check(std::strstr(log, "  output thread: eip ") != nullptr, "the output thread's registers are logged");
    Check(std::strstr(log, "  game thread: eip ") != nullptr, "the game thread's registers are logged too");
    Check(std::strstr(log, "output thread stack scan [esp+") != nullptr, "the output thread's stack scan is logged");
    Check(std::strstr(log, "test_stall.exe+") != nullptr, "a return address into the test's own code is found");
    Check(std::strstr(log, "stall: the output thread's frame ended after") != nullptr, "the stall's end is logged");
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
