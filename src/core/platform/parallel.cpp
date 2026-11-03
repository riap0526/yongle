#include "core/platform/parallel.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/platform/affinity.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace core::platform::parallel {
namespace {

core::diag::Feature g_feature{"parallel workers (core/platform/parallel)"};

struct Job {
    core::display::RowsFn fn = nullptr;
    void *ctx = nullptr;
    int count = 0, pieces = 0;
};

// The job being worked: published under g_lock with a new generation, then the pieces are taken from g_next (the
// generation in the high half, so a worker still holding the previous job cannot take a piece of the next one).
std::mutex g_lock;
std::condition_variable g_wake;
Job g_job;          // under g_lock
unsigned g_gen = 0; // under g_lock
std::atomic<std::uint64_t> g_next{0};
std::atomic<int> g_done{0};
std::atomic<bool> g_failed{false};

int g_started = 0;           // workers running (caller only)
int g_limit = kMaxWorkers;   // LimitWorkers
bool g_triedStart = false;   // the first start was logged

void RunPiece(const Job &j, int piece)
{
    const int begin = piece * kRowsPerPiece;
    const int end = begin + kRowsPerPiece < j.count ? begin + kRowsPerPiece : j.count;
    j.fn(j.ctx, begin, end);
}

bool Claim(unsigned gen, int pieces, int *piece)
{
    std::uint64_t v = g_next.load();
    for (;;) {
        if (static_cast<unsigned>(v >> 32) != gen || static_cast<int>(v & 0xffffffffu) >= pieces) {
            return false;
        }
        if (g_next.compare_exchange_weak(v, v + 1)) {
            *piece = static_cast<int>(v & 0xffffffffu);
            return true;
        }
    }
}

struct Piece {
    const Job *job;
    int piece;
};

DWORD WINAPI Worker(LPVOID)
{
    core::platform::affinity::OurThread("parallel worker");
    unsigned seen = 0;
    for (;;) {
        Job j;
        unsigned gen;
        {
            std::unique_lock<std::mutex> l(g_lock);
            g_wake.wait(l, [&] { return g_gen != seen; });
            gen = seen = g_gen;
            j = g_job;
        }
        Piece p{&j, 0};
        while (Claim(gen, j.pieces, &p.piece)) {
            if (!core::diag::Enabled(g_feature) ||
                !core::diag::Guarded(
                    g_feature,
                    [](void *a) {
                        const Piece &q = *static_cast<const Piece *>(a);
                        RunPiece(*q.job, q.piece);
                    },
                    &p)) {
                g_failed.store(true); // the caller does the whole job again
            }
            g_done.fetch_add(1, std::memory_order_release);
        }
    }
}

// Workers wanted now: none without a processor apart from the game's.
int Wanted()
{
    const int apart = core::platform::affinity::ProcessorsApart();
    int n = g_limit < kMaxWorkers ? g_limit : kMaxWorkers;
    return apart < n ? apart : n;
}

// Starts the workers still missing; the number usable now (0: run on the caller).
int Ensure()
{
    const int want = Wanted();
    if (want <= 0) {
        if (!g_triedStart) {
            g_triedStart = true;
            core_log("parallel: no processor apart from the game's: our scaling runs on the game thread");
        }
        return 0;
    }
    const int before = g_started;
    while (g_started < want) {
        HANDLE h = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
        if (!h) {
            core_log("parallel: a worker thread could not be started (error %lu)", GetLastError());
            break;
        }
        CloseHandle(h);
        ++g_started;
    }
    if (g_started != before || !g_triedStart) {
        g_triedStart = true;
        core_log("parallel: %d worker thread(s) for our scaling, %d processor(s) apart from the game's", g_started,
                 core::platform::affinity::ProcessorsApart());
    }
    return g_started < want ? g_started : want;
}

} // namespace

void ForRows(int count, core::display::RowsFn fn, void *ctx)
{
    if (count <= 0) {
        return;
    }
    const int pieces = (count + kRowsPerPiece - 1) / kRowsPerPiece;
    if (pieces < 2 || !core::diag::Enabled(g_feature) || Ensure() <= 0) {
        fn(ctx, 0, count);
        return;
    }
    const Job j{fn, ctx, count, pieces};
    unsigned gen;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_job = j;
        gen = ++g_gen;
        g_done.store(0);
        g_failed.store(false);
        g_next.store(static_cast<std::uint64_t>(gen) << 32);
    }
    g_wake.notify_all();
    int piece = 0;
    while (Claim(gen, pieces, &piece)) {
        RunPiece(j, piece);
        g_done.fetch_add(1, std::memory_order_release);
    }
    // The pieces the workers took: usually a few microseconds behind.
    const DWORD start = GetTickCount();
    for (int spins = 0; g_done.load(std::memory_order_acquire) < pieces; ++spins) {
        if (spins < 4000) {
            YieldProcessor();
            continue;
        }
        SwitchToThread();
        if (GetTickCount() - start > 2000) {
            core::diag::Disable(g_feature, "a worker did not finish its piece within 2 s");
            g_failed.store(true);
            break;
        }
    }
    if (g_failed.load()) {
        fn(ctx, 0, count); // the same result (every row depends on the source only), on this thread
    }
}

int Workers()
{
    return core::diag::Enabled(g_feature) && Wanted() > 0 ? (g_started < Wanted() ? g_started : Wanted()) : 0;
}

void LimitWorkers(int n)
{
    g_limit = n < 0 ? 0 : n;
}

} // namespace core::platform::parallel
