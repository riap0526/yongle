// Where the time goes (R2b.1-3, docs/archive/resolution_ui_r2b1.md): named phases timed with the performance counter, on the
// game thread (one sample per main-loop pass) and on the output thread (one sample per frame drawn). The last 240
// samples of each phase are kept for the graph (core/ui/perf_view), the last second for the averages, and the time
// since the last TakeWindow for the rate log. Nothing here knows a game: the game layer registers its own phases
// (e.g. "simulation") and marks the end of its passes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core::diag::perf {

enum class Thread { Game, Output };

constexpr int kMaxPhases = 16;
constexpr int kHistory = 240;

// Registers a phase (start-up, before the threads time anything); total: the phase that holds the others of its
// thread (e.g. the whole main-loop pass), so the rest of it can be shown as "other". Returns its id, or -1 when full.
int Register(const char *name, Thread thread, bool total = false);

std::int64_t Now(); // performance counter ticks

// Adds time to a phase in the current sample of its thread (the thread that owns it only).
void Add(int id, std::int64_t ticks);

struct Scope {
    explicit Scope(int phase) : id(phase), t0(phase >= 0 ? Now() : 0) {}
    ~Scope()
    {
        if (id >= 0) {
            Add(id, Now() - t0);
        }
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    int id;
    std::int64_t t0;
};

// The end of one sample of a thread: its phases' time goes to the history and the averages.
void EndSample(Thread thread);

// Readers (any thread).
struct PhaseView {
    const char *name;
    Thread thread;
    bool total;
    float avgMs;   // per sample over the last second
    float percent; // of its thread's total phase over the last second (0 when it has none)
};
struct View {
    int count;
    PhaseView phase[kMaxPhases];
    float gameOtherMs;      // the game thread's total minus its other phases (per sample, last second)
    float gamePerSecond;    // game samples (passes) in the last second
    float outputPerSecond;  // output samples (frames drawn) in the last second
};
void Read(View *out);

// The last samples (ms, oldest first) of a phase; returns how many (at most kHistory).
int History(int id, float *out, int max);
int Find(const char *name); // -1 when not registered

// For the rate log: "pass 11.8 ms = simulation 0.9 + city draw 7.1 + ... + other 1.2; output frame: upload 1.5,
// draw 0.4, present 4.0" averaged since the last call (empty when nothing was timed). maxOut (O1a, optional): the
// longest sample of each phase in the same window, and how many passes or frames took more than 50 and 100 ms --
// the averages hide single long frames.
void TakeWindow(char *out, std::size_t size, char *maxOut = nullptr, std::size_t maxSize = 0);

} // namespace core::diag::perf
