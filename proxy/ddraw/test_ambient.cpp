// Offline test of core/sound/ambient_rate (both games; no DLL, no game): N follows the city passes per second so that
// passes/s / N is the rate wanted; nothing is written before the game has read its file (N 0); switching back to
// original puts the file's N back, and a rate again takes over (R -> original -> R); other screens leave N alone.
#include "core/sound/ambient_rate.h"

#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what, long value = 0)
{
    std::printf("%-64s %s (%ld)\n", what, ok ? "ok" : "FAILED", value);
    if (!ok) ++g_failures;
}

// Runs passes at `perSecond` (a divisor of 1000: the clock is in milliseconds) for `ms` milliseconds from *now.
void Run(core::sound::AmbientRate &r, unsigned long *now, int perSecond, int ms, bool city, int *n)
{
    const int passes = perSecond * ms / 1000;
    for (int i = 0; i < passes; ++i) {
        *now += 1000 / perSecond;
        r.Pass(*now, city, n);
    }
}

} // namespace

int main()
{
    core::sound::AmbientRate r;
    unsigned long now = 1000;
    int n = 0;
    r.SetRate(0.5);
    Run(r, &now, 400, 1000, true, &n);
    Check(n == 0 && r.FileN() == 0, "before the game read its file: N stays 0, nothing kept", n);

    n = 300; // sound_fx_load read FREQUENCY_L2
    Run(r, &now, 500, 2000, true, &n);
    Check(r.FileN() == 300, "the file's N kept the first time it is not 0", r.FileN());
    Check(n == 1000, "0.5/s at 500 passes/s: N = 1000", n);

    Run(r, &now, 100, 2000, true, &n);
    Check(n == 200, "0.5/s at 100 passes/s: N = 200 (follows the passes)", n);

    r.SetRate(0); // -> original
    Run(r, &now, 100, 1000, true, &n);
    Check(n == 300, "R -> original: the file's N back", n);
    Run(r, &now, 500, 1000, true, &n);
    Check(n == 300, "original: N not touched at other pass rates", n);

    r.SetRate(1.0); // -> R again
    Run(r, &now, 500, 1000, true, &n);
    Check(n == 500, "original -> 1.0/s at 500 passes/s: N = 500", n);

    int before = n;
    Run(r, &now, 1000, 1000, false, &n);
    Check(n == before, "other screens: N left as it is", n);

    r.SetRate(4.0);
    Run(r, &now, 4, 3000, true, &n);
    Check(n == 2, "4 passes/s at 4.0/s: N held at 2 (N = 1 would never hold)", n);

    // A pause (switched away, our panel open): no pass for 5 s, then the passes as before. N must not fall (the
    // window spanning the pause measured about 0 passes/s: N 2, a burst of ambient sounds; seen in testing).
    r.SetRate(0.5);
    Run(r, &now, 100, 2000, true, &n);
    Check(n == 200, "0.5/s at 100 passes/s: N = 200 before the pause", n);
    now += 5000;
    int lowest = n;
    for (int i = 0; i < 200; ++i) { // 2 s at 100 passes/s after the pause, N watched at every pass
        now += 10;
        r.Pass(now, true, &n);
        lowest = n < lowest ? n : lowest;
    }
    Check(lowest == 200 && n == 200, "after a 5 s pause: N stays 200 at every pass (no burst)", lowest);
    now += 200; // a gap shorter than kGapMs (a slow frame): still measured
    Run(r, &now, 100, 2000, true, &n);
    Check(n == 200, "a 200 ms frame: N as before", n);
    Run(r, &now, 50, 2000, true, &n);
    Check(n == 100, "then 50 passes/s: N follows (100)", n);

    r.SetRate(0.25);
    Run(r, &now, 1000, 2000, true, &n);
    Check(n == 4000, "0.25/s at 1000 passes/s: N = 4000", n);
    r.SetRate(0.02);
    Run(r, &now, 1000, 1000, true, &n);
    Check(n == 32767, "N would be 50000: held at 32767", n);

    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
