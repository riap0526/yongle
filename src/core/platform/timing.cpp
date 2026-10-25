#include "core/platform/timing.h"

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <atomic>

namespace {

std::atomic<bool> g_raised{false};

} // namespace

extern "C" unsigned core_timer_resolution_us(void)
{
    using QueryFn = LONG(NTAPI *)(PULONG, PULONG, PULONG);
    static const QueryFn query = reinterpret_cast<QueryFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution")));
    ULONG coarsest = 0, finest = 0, current = 0;
    if (!query || query(&coarsest, &finest, &current) < 0) {
        return 0;
    }
    return current / 10; // 100 ns units
}

extern "C" void core_timer_begin(void)
{
    if (g_raised.load()) {
        return;
    }
    unsigned before = core_timer_resolution_us();
    MMRESULT r = timeBeginPeriod(1);
    g_raised = r == TIMERR_NOERROR;
    core_log("timer: timeBeginPeriod(1) -> %u; system timer resolution %u us before, %u us now", r, before,
             core_timer_resolution_us());
}

extern "C" void core_timer_end(void)
{
    if (g_raised.exchange(false)) {
        MMRESULT r = timeEndPeriod(1);
        core_log("timer: timeEndPeriod(1) -> %u", r);
    }
}
