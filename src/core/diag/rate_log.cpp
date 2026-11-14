// The common part of the rate log (rate_log.h).
#include "core/diag/rate_log.h"

#include "core/diag/environment.h"
#include "core/diag/perf.h"
#include "core/log.h"
#include "core/output/sdl_output.h"

namespace core::diag {

void LogRateTail(const char *why)
{
    char times[768], longest[768];
    perf::TakeWindow(times, sizeof(times), longest, sizeof(longest));
    if (times[0]) {
        core_log("time%s (ms): %s", why, times);
        core_log("time max%s (ms): %s", why, longest);
    }
    char intervals[512];
    core::output::TakeIntervals(intervals, sizeof(intervals)); // how evenly the frames were shown
    if (intervals[0]) {
        core_log("intervals%s (ms): %s", why, intervals);
    }
    char cpu[512];
    env::TakeWindow(cpu, sizeof(cpu)); // processor time actually given to our threads
    if (cpu[0]) {
        core_log("cpu%s: %s", why, cpu);
    }
}

} // namespace core::diag
