// Logging shared by the two DirectDraw back ends: the logging wrapper around the system ddraw (ddraw_wrap) and
// our own implementation (ddraw_emu).
//
// Policy: the first kDetailCalls calls of each method are logged with their arguments, the result and the caller;
// after that only counters are kept. A summary table of all called methods is written every kSummaryMs, when the
// last IDirectDraw is released, and when the DLL unloads (core_ddraw_write_summary in ddraw_wrap.h).
// Nothing here allocates on the logging path (fixed buffers).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace core::ddlog {

constexpr long kDetailCalls = 64;
constexpr DWORD kSummaryMs = 60000;
constexpr DWORD kPresentRateMs = 10000;

// Per-method call statistics; one static instance per method (TRACE_STAT), linked into a global list.
struct Stat {
    const char *iface;
    const char *method;
    std::atomic<long> calls{0};
    std::atomic<long> failures{0};
    std::atomic<long> lastFailure{0};
    Stat *next = nullptr;
    Stat(const char *i, const char *m);
};

#define TRACE_STAT(IFACE, NAME) static ::core::ddlog::Stat s_stat(IFACE, NAME)
#define CALLER() __builtin_return_address(0)

// Counts the call; returns true while this method is still within its detailed-logging budget.
bool Hit(Stat &s, HRESULT hr);
void MaybePeriodicSummary();

struct Buf {
    char s[512];
    int n = 0;
    Buf() { s[0] = 0; }
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        if (n >= static_cast<int>(sizeof(s)) - 1) {
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        int m = std::vsnprintf(s + n, sizeof(s) - n, fmt, ap);
        va_end(ap);
        if (m > 0) {
            n += m;
            if (n > static_cast<int>(sizeof(s)) - 1) {
                n = sizeof(s) - 1;
            }
        }
    }
};

const char *HrName(HRESULT hr, char *tmp, size_t len);
void AddRect(Buf &b, const RECT *r);
void AddGuid(Buf &b, REFIID g);
void AddPixelFormat(Buf &b, const DDPIXELFORMAT &pf);
void AddDesc(Buf &b, const DDSURFACEDESC *d);
void LogCall(const char *iface, unsigned id, const char *method, const Buf &args, HRESULT hr, void *caller,
             const Buf *out = nullptr);

// Output monitor, fed with every successful blit onto (or unlock of) the primary surface, i.e. every frame that
// reaches the screen. Logs the geometry whenever it changes and, every kPresentRateMs, the present rate together
// with the IsLost call rate (games that poll IsLost once per loop iteration reveal their loop rate this way) and
// the system timer resolution.
void NotePresent(const char *how, unsigned srcId, SIZE srcSize, const RECT &src, unsigned primaryId, SIZE primarySize,
                 const RECT &dst, void *caller);
void CountIsLost();

} // namespace core::ddlog
