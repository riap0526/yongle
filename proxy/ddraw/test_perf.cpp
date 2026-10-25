// Offline test of core/diag/perf (not the game; R2b.1-3): phases add their time to the current sample of their
// thread; a sample ends per thread; averages over the last second, the total's "other", the history (oldest first)
// and the window for the rate log; registering the same name twice gives the same phase.
#include "core/diag/perf.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
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

namespace perf = core::diag::perf;

std::int64_t Ms(double ms)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<std::int64_t>(ms * f.QuadPart / 1000.0);
}

bool Near(float a, float b)
{
    return std::fabs(a - b) < 0.01f;
}

} // namespace

int main()
{
    const int pass = perf::Register("pass", perf::Thread::Game, true);
    const int sim = perf::Register("simulation", perf::Thread::Game);
    const int draw = perf::Register("city draw", perf::Thread::Game);
    const int out = perf::Register("output frame", perf::Thread::Output, true);
    const int upload = perf::Register("upload", perf::Thread::Output);
    Check(pass == 0 && sim == 1 && draw == 2 && out == 3 && upload == 4, "phases registered in order");
    Check(perf::Register("simulation", perf::Thread::Game) == sim && perf::Find("city draw") == draw &&
              perf::Find("nothing") == -1,
          "the same name again: the same phase; Find");

    // 10 game passes: 10 ms each = 2 ms simulation + 5 ms city draw (+ 3 ms other).
    for (int i = 0; i < 10; ++i) {
        perf::Add(pass, Ms(10));
        perf::Add(sim, Ms(2));
        perf::Add(draw, Ms(5));
        perf::EndSample(perf::Thread::Game);
    }
    // 4 output frames of 4 ms with 1.5 ms upload.
    for (int i = 0; i < 4; ++i) {
        perf::Add(out, Ms(4));
        perf::Add(upload, Ms(1.5));
        perf::EndSample(perf::Thread::Output);
    }
    perf::View v;
    perf::Read(&v);
    Check(v.count == 5 && v.gamePerSecond == 10 && v.outputPerSecond == 4, "samples in the last second per thread");
    Check(Near(v.phase[pass].avgMs, 10) && Near(v.phase[sim].avgMs, 2) && Near(v.phase[draw].avgMs, 5),
          "averages per sample");
    Check(Near(v.phase[sim].percent, 20) && Near(v.phase[draw].percent, 50) && Near(v.gameOtherMs, 3),
          "percent of the thread's total, and the rest as other (3 ms)");
    Check(Near(v.phase[upload].avgMs, 1.5f) && Near(v.phase[upload].percent, 37.5f),
          "output thread: averages of its own samples");
    float h[perf::kHistory];
    perf::Add(pass, Ms(20));
    perf::EndSample(perf::Thread::Game);
    int n = perf::History(pass, h, perf::kHistory);
    Check(n == 11 && Near(h[0], 10) && Near(h[10], 20), "history: oldest first, the latest last");
    n = perf::History(pass, h, 3);
    Check(n == 3 && Near(h[2], 20) && Near(h[0], 10), "history: the last max samples");
    char text[512];
    perf::TakeWindow(text, sizeof(text));
    std::printf("window: %s\n", text);
    Check(std::strstr(text, "per pass: total 10.91 ms") && std::strstr(text, "simulation 1.82") &&
              std::strstr(text, "other ") && std::strstr(text, "per output frame: total 4.00 ms, upload 1.50"),
          "rate log window: averages since the last call");
    perf::TakeWindow(text, sizeof(text));
    Check(text[0] == 0, "nothing timed since: empty");
    Sleep(1100);
    perf::Read(&v);
    Check(v.gamePerSecond == 0 && v.phase[pass].avgMs == 0, "older than a second: not in the averages");
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
