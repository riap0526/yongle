#include "core/diag/stall.h"

#include "core/diag/guard.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

namespace core::diag::stall {

namespace {

constexpr DWORD kStallMs = 1000;  // one pass or one frame this long is a stall
constexpr DWORD kPollMs = 100;    // how often the watchdog looks
constexpr int kReports = 20;      // stalls logged per run
constexpr DWORD kStackBytes = 8192; // copied from esp up

const char *const kName[] = {"game thread", "output thread"};
const char *const kUnit[] = {"pass", "frame"};

struct Watched {
    std::atomic<HANDLE> handle{nullptr};
    std::atomic<DWORD> since{0};   // GetTickCount at Begin; 0: not in a pass or frame
    std::atomic<DWORD> sampled{0}; // the since of the stall sampled last (its end is logged)
};
Watched g_thread[2];
std::atomic<int> g_reports{0};

struct Capture {
    bool ok;
    DWORD eip, esp, ebp, size;
    BYTE stack[kStackBytes];
};
Capture g_capture[2]; // watchdog thread only

// The thread is suspended only while its registers and stack are copied. In that time this thread calls only
// SuspendThread, GetThreadContext, ReadProcessMemory and ResumeThread (system calls that take no user-mode lock) and
// writes into the static g_capture: no memory is allocated, nothing is logged, no symbol or module is looked up -- the
// suspended thread may hold the heap's, the loader's or our log's lock. Suspending is asynchronous; GetThreadContext
// returns only once the thread has really stopped, so the stack is read after it. Reading from esp up stays in the
// committed stack (its guard page is below esp) and stops at the first page that cannot be read. Everything else
// (logging, the module+offset of each address, our symbols) runs after ResumeThread, in Report and LogCapturedStack.
void Take(Watched &w, Capture &c)
{
    c.ok = false;
    c.size = 0;
    HANDLE h = w.handle.load();
    if (!h || SuspendThread(h) == static_cast<DWORD>(-1)) {
        return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(h, &ctx)) {
        c.eip = ctx.Eip;
        c.esp = ctx.Esp;
        c.ebp = ctx.Ebp;
        // Page by page from esp: ReadProcessMemory stops at the first page that cannot be read (past the stack).
        DWORD at = ctx.Esp;
        while (c.size < kStackBytes) {
            DWORD n = 4096 - (at & 4095);
            if (n > kStackBytes - c.size) {
                n = kStackBytes - c.size;
            }
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(at), c.stack + c.size, n, &got) ||
                got != n) {
                break;
            }
            c.size += n;
            at += n;
        }
        c.ok = true;
    }
    ResumeThread(h);
}

void Report(int which, DWORD ms)
{
    core_log("stall: the %s has been in one %s for %lu ms; where both threads are (module+offset):", kName[which],
             kUnit[which], ms);
    Take(g_thread[0], g_capture[0]);
    Take(g_thread[1], g_capture[1]);
    for (int i = 0; i < 2; ++i) {
        const Capture &c = g_capture[i];
        if (!c.ok) {
            core_log("  %s: not captured", kName[i]);
            continue;
        }
        LogCapturedStack(kName[i], c.eip, c.esp, c.ebp, c.stack, c.size);
    }
}

DWORD WINAPI Watchdog(void *)
{
    for (;;) {
        Sleep(kPollMs);
        for (int i = 0; i < 2; ++i) {
            Watched &w = g_thread[i];
            const DWORD since = w.since.load();
            // The time after since is read: a Begin between the two would otherwise look like a 49-day stall (the
            // second test logged "4294967280 ms" for a 31 ms frame).
            const DWORD now = GetTickCount();
            if (since == 0 || static_cast<LONG>(now - since) < static_cast<LONG>(kStallMs) ||
                w.sampled.load() == since) {
                continue;
            }
            w.sampled = since;
            if (g_reports.fetch_add(1) < kReports) {
                Report(i, now - since);
            }
            else {
                w.sampled = 0; // not reported: its end is not logged either
            }
        }
    }
}

} // namespace

void Start()
{
    HANDLE t = CreateThread(nullptr, 64 * 1024, &Watchdog, nullptr, 0, nullptr);
    if (!t) {
        core_log("stall: the watchdog thread could not be started (%lu); stalls are not sampled", GetLastError());
        return;
    }
    CloseHandle(t);
}

void RegisterThread(Thread which)
{
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        return;
    }
    if (HANDLE old = g_thread[static_cast<int>(which)].handle.exchange(h)) {
        CloseHandle(old);
    }
}

void Begin(Thread which)
{
    DWORD now = GetTickCount();
    g_thread[static_cast<int>(which)].since = now ? now : 1;
}

void End(Thread which)
{
    Watched &w = g_thread[static_cast<int>(which)];
    const DWORD since = w.since.exchange(0);
    if (since != 0 && w.sampled.load() == since) {
        core_log("stall: the %s's %s ended after %lu ms", kName[static_cast<int>(which)],
                 kUnit[static_cast<int>(which)], GetTickCount() - since);
    }
}

} // namespace core::diag::stall
