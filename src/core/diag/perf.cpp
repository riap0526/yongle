#include "core/diag/perf.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace core::diag::perf {

namespace {

struct Phase {
    const char *name;
    Thread thread;
    bool total;
    std::atomic<std::int64_t> current{0}; // ticks in the sample being timed (its thread only adds)
    float history[kHistory];              // ms per sample, by its thread's ring position
    double windowMs;                       // since the last TakeWindow
    double windowMaxMs;                    // the longest sample since the last TakeWindow
};

struct Ring {
    std::int64_t when[kHistory]; // when each sample ended
    int next = 0, filled = 0;
    long windowSamples = 0;
    long windowOver[2] = {0, 0}; // samples whose total phase took more than kSlowMs[0] / kSlowMs[1]
};

constexpr double kSlowMs[2] = {50.0, 100.0};

Phase g_phase[kMaxPhases];
std::atomic<int> g_count{0};
Ring g_ring[2];
SRWLOCK g_lock = SRWLOCK_INIT;

double TicksPerMs()
{
    static const double v = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    return v;
}

int Index(Thread t)
{
    return t == Thread::Game ? 0 : 1;
}

// "per pass: total 52.0 (3 over 50 ms, 1 over 100 ms), city 21.0, ...; per output frame: ..." -- the longest sample of
// each phase since the last TakeWindow (g_lock held by the caller).
void WindowMax(char *out, std::size_t size)
{
    const int n = g_count.load();
    size_t len = 0;
    for (int t = 0; t < 2 && len < size; ++t) {
        const Ring &r = g_ring[t];
        if (r.windowSamples <= 0) {
            continue;
        }
        int w = std::snprintf(out + len, size - len, "%s%s", len ? "; " : "", t == 0 ? "per pass:" : "per output frame:");
        len += w > 0 ? static_cast<size_t>(w) : 0;
        bool first = true;
        for (int i = 0; i < n && len < size; ++i) {
            const Phase &p = g_phase[i];
            if (Index(p.thread) != t) {
                continue;
            }
            if (p.total) {
                w = std::snprintf(out + len, size - len, "%s total %.1f (%ld over %.0f ms, %ld over %.0f ms)",
                                  first ? "" : ",", p.windowMaxMs, r.windowOver[0], kSlowMs[0], r.windowOver[1],
                                  kSlowMs[1]);
            }
            else {
                w = std::snprintf(out + len, size - len, "%s %s %.1f", first ? "" : ",", p.name, p.windowMaxMs);
            }
            len += w > 0 ? static_cast<size_t>(w) : 0;
            first = false;
        }
    }
    if (len >= size) {
        out[size - 1] = 0;
    }
}

} // namespace

std::int64_t Now()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

int Register(const char *name, Thread thread, bool total)
{
    AcquireSRWLockExclusive(&g_lock);
    int n = g_count.load();
    int id = -1;
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(g_phase[i].name, name) == 0 && g_phase[i].thread == thread) {
            id = i; // registered again (e.g. a second attach): the same phase
        }
    }
    if (id < 0 && n < kMaxPhases) {
        id = n;
        Phase &p = g_phase[id];
        p.name = name;
        p.thread = thread;
        p.total = total;
        p.current = 0;
        std::memset(p.history, 0, sizeof(p.history));
        p.windowMs = 0;
        p.windowMaxMs = 0;
        g_count.store(n + 1);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return id;
}

void Add(int id, std::int64_t ticks)
{
    if (id >= 0 && id < g_count.load()) {
        g_phase[id].current.fetch_add(ticks, std::memory_order_relaxed);
    }
}

void EndSample(Thread thread)
{
    const double perMs = TicksPerMs();
    AcquireSRWLockExclusive(&g_lock);
    Ring &r = g_ring[Index(thread)];
    const int pos = r.next;
    const int n = g_count.load();
    for (int i = 0; i < n; ++i) {
        Phase &p = g_phase[i];
        if (p.thread != thread) {
            continue;
        }
        const double ms = static_cast<double>(p.current.exchange(0)) / perMs;
        p.history[pos] = static_cast<float>(ms);
        p.windowMs += ms;
        if (ms > p.windowMaxMs) {
            p.windowMaxMs = ms;
        }
        if (p.total) {
            for (int k = 0; k < 2; ++k) {
                r.windowOver[k] += ms > kSlowMs[k] ? 1 : 0;
            }
        }
    }
    r.when[pos] = Now();
    r.next = (pos + 1) % kHistory;
    if (r.filled < kHistory) {
        ++r.filled;
    }
    ++r.windowSamples;
    ReleaseSRWLockExclusive(&g_lock);
}

void Read(View *out)
{
    std::memset(out, 0, sizeof(*out));
    const double perMs = TicksPerMs();
    const std::int64_t now = Now(), second = static_cast<std::int64_t>(perMs * 1000);
    AcquireSRWLockShared(&g_lock);
    int recent[2] = {0, 0};
    for (int t = 0; t < 2; ++t) {
        const Ring &r = g_ring[t];
        for (int k = 0; k < r.filled; ++k) {
            int pos = (r.next - 1 - k + kHistory) % kHistory;
            if (now - r.when[pos] > second) {
                break;
            }
            ++recent[t];
        }
    }
    out->gamePerSecond = static_cast<float>(recent[0]);
    out->outputPerSecond = static_cast<float>(recent[1]);
    const int n = g_count.load();
    float totals[2] = {0, 0}, others[2] = {0, 0};
    for (int i = 0; i < n; ++i) {
        const Phase &p = g_phase[i];
        const int t = Index(p.thread);
        const Ring &r = g_ring[t];
        double sum = 0;
        for (int k = 0; k < recent[t]; ++k) {
            sum += p.history[(r.next - 1 - k + kHistory) % kHistory];
        }
        PhaseView &v = out->phase[i];
        v.name = p.name;
        v.thread = p.thread;
        v.total = p.total;
        v.avgMs = recent[t] ? static_cast<float>(sum / recent[t]) : 0.0f;
        if (p.total) {
            totals[t] = v.avgMs;
        }
        else {
            others[t] += v.avgMs;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    out->count = n;
    for (int i = 0; i < n; ++i) {
        PhaseView &v = out->phase[i];
        const float total = totals[Index(v.thread)];
        v.percent = total > 0 ? 100.0f * v.avgMs / total : 0.0f;
    }
    out->gameOtherMs = totals[0] > others[0] ? totals[0] - others[0] : 0.0f;
}

int History(int id, float *out, int max)
{
    if (id < 0 || id >= g_count.load() || max <= 0) {
        return 0;
    }
    AcquireSRWLockShared(&g_lock);
    const Phase &p = g_phase[id];
    const Ring &r = g_ring[Index(p.thread)];
    int n = r.filled < max ? r.filled : max;
    for (int k = 0; k < n; ++k) { // oldest first
        out[k] = p.history[(r.next - n + k + kHistory) % kHistory];
    }
    ReleaseSRWLockShared(&g_lock);
    return n;
}

int Find(const char *name)
{
    const int n = g_count.load();
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(g_phase[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

void TakeWindow(char *out, std::size_t size, char *maxOut, std::size_t maxSize)
{
    if (!size) {
        return;
    }
    out[0] = 0;
    if (maxOut && maxSize) {
        maxOut[0] = 0;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (maxOut && maxSize) {
        WindowMax(maxOut, maxSize);
    }
    const int n = g_count.load();
    size_t len = 0;
    for (int t = 0; t < 2; ++t) {
        const long samples = g_ring[t].windowSamples;
        if (samples <= 0) {
            continue;
        }
        double total = 0, others = 0;
        bool haveTotal = false;
        for (int i = 0; i < n; ++i) {
            const Phase &p = g_phase[i];
            if (Index(p.thread) == t) {
                if (p.total) {
                    total = p.windowMs / samples;
                    haveTotal = true;
                }
                else {
                    others += p.windowMs / samples;
                }
            }
        }
        int w = std::snprintf(out + len, size - len, "%s%s", len ? "; " : "", t == 0 ? "per pass:" : "per output frame:");
        len += w > 0 ? static_cast<size_t>(w) : 0;
        if (haveTotal && len < size) {
            w = std::snprintf(out + len, size - len, " total %.2f ms", total);
            len += w > 0 ? static_cast<size_t>(w) : 0;
        }
        for (int i = 0; i < n && len < size; ++i) {
            const Phase &p = g_phase[i];
            if (Index(p.thread) == t && !p.total) {
                w = std::snprintf(out + len, size - len, ", %s %.2f", p.name, p.windowMs / samples);
                len += w > 0 ? static_cast<size_t>(w) : 0;
            }
        }
        if (haveTotal && t == 0 && len < size) {
            w = std::snprintf(out + len, size - len, ", other %.2f", total > others ? total - others : 0.0);
            len += w > 0 ? static_cast<size_t>(w) : 0;
        }
        if (len >= size) {
            len = size - 1;
        }
    }
    for (int i = 0; i < n; ++i) {
        g_phase[i].windowMs = 0;
        g_phase[i].windowMaxMs = 0;
    }
    for (Ring &r : g_ring) {
        r.windowSamples = 0;
        r.windowOver[0] = r.windowOver[1] = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace core::diag::perf
