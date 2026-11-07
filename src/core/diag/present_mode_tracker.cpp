// The present mode log's bookkeeping (core/diag/present_mode.h), without Windows calls: offline-testable.
#include "core/diag/present_mode.h"

#include <cstdio>
#include <cstring>

namespace core::diag::present_mode {

const char *ModeName(int mode)
{
    switch (mode) {
    case kComposed:
        return "composed";
    case kOverlay:
        return "overlay";
    case kNone:
        return "none";
    case kCompositionFailure:
        return "composition failure";
    case kUnknown:
        return "unknown";
    default: {
        static char other[24]; // one thread (the drawing thread) only
        std::snprintf(other, sizeof(other), "value %d", mode);
        return other;
    }
    }
}

bool Tracker::Query(bool ok, int mode, long hr, std::int64_t ticks, unsigned long nowMs, int *from, bool *firstError)
{
    ++queries_;
    ticksSum_ += ticks;
    if (ticks > ticksMax_) {
        ticksMax_ = ticks;
    }
    *firstError = false;
    if (!ok) {
        ++errors_;
        if (failRun_++ == 0) {
            failRunStart_ = nowMs;
        }
        failRunMs_ = nowMs - failRunStart_;
        bool seen = false;
        for (int i = 0; i < seenHrCount_; ++i) {
            seen = seen || seenHr_[i] == hr;
        }
        if (!seen && seenHrCount_ < kMaxErrorKinds) {
            seenHr_[seenHrCount_++] = hr;
            *firstError = true;
        }
        return false;
    }
    failRun_ = 0;
    failRunMs_ = 0;
    ++perMode_[mode >= 0 && mode < kModes ? mode : kModes];
    if (mode == mode_) {
        return false;
    }
    *from = mode_;
    mode_ = mode;
    ++changes_;
    return true;
}

bool Tracker::ChangeLineAllowed(unsigned long nowMs)
{
    if (!lineWindowStarted_ || nowMs - lineWindowMs_ >= 1000) {
        lineWindowStarted_ = true;
        lineWindowMs_ = nowMs;
        linesInWindow_ = 0;
    }
    if (linesInWindow_ >= kChangeLines) {
        ++notLogged_;
        return false;
    }
    ++linesInWindow_;
    return true;
}

void Tracker::Remember(unsigned long nowMs, int from, int to, long hr, long presentsBefore, const char *text)
{
    Event &e = ring_[events_ % kEvents];
    e.ms = nowMs;
    e.from = from;
    e.to = to;
    e.hr = hr;
    e.presents = presentsBefore;
    std::snprintf(e.text, sizeof(e.text), "%s", text ? text : "");
    ++events_;
}

void Tracker::EventLine(int i, unsigned long nowMs, char *out, std::size_t size) const
{
    const long first = events_ > kEvents ? events_ - kEvents : 0;
    const Event &e = ring_[(first + i) % kEvents];
    const double age = (nowMs - e.ms) / 1000.0;
    if (e.from == kUnknown && e.to == kUnknown) {
        std::snprintf(out, size, "%.3f s ago: the query failed (0x%08lx); %s", age, static_cast<unsigned long>(e.hr),
                      e.text);
        return;
    }
    char from[24];
    std::snprintf(from, sizeof(from), "%s", ModeName(e.from)); // ModeName's "value N" buffer is shared
    std::snprintf(out, size, "%.3f s ago: %s -> %s after %ld presents; %s", age, from, ModeName(e.to), e.presents,
                  e.text);
}

bool Tracker::Period(unsigned long nowMs, long long dxgiPresents, long long dxgiRefreshes, double ticksPerUs,
                     const char *where, bool force, bool keep, char *out, std::size_t size)
{
    out[0] = 0;
    if (!started_) {
        started_ = true;
        lastLineMs_ = nowMs;
        lastDxgiPresents_ = dxgiPresents;
        lastDxgiRefreshes_ = dxgiRefreshes;
        if (!force) {
            return false;
        }
    }
    const unsigned long span = nowMs - lastLineMs_;
    if (!force && span < kPeriodMs) {
        return false;
    }
    const long long dxP = dxgiPresents >= 0 && lastDxgiPresents_ >= 0 ? dxgiPresents - lastDxgiPresents_ : -1;
    const long long dxR = dxgiRefreshes >= 0 && lastDxgiRefreshes_ >= 0 ? dxgiRefreshes - lastDxgiRefreshes_ : -1;
    char dxgi[64];
    if (dxP >= 0 && dxR >= 0) {
        std::snprintf(dxgi, sizeof(dxgi), "DXGI present count +%lld, present refresh count +%lld", dxP, dxR);
    }
    else {
        std::snprintf(dxgi, sizeof(dxgi), "DXGI counts not known");
    }
    const double us = ticksPerUs > 0 ? 1.0 / ticksPerUs : 0;
    std::snprintf(out, size,
                  "present mode: %.1f s: %ld presents; mode %s (composed %ld, overlay %ld, none %ld, composition "
                  "failure %ld, other %ld, errors %ld; changes %ld, %ld of them not logged); %s; query mean %.1f us, "
                  "max %.1f us; %s",
                  span / 1000.0, presents_, ModeName(mode_), perMode_[kComposed], perMode_[kOverlay], perMode_[kNone],
                  perMode_[kCompositionFailure], perMode_[kModes], errors_, changes_, notLogged_, dxgi,
                  queries_ > 0 ? static_cast<double>(ticksSum_) / queries_ * us : 0.0,
                  static_cast<double>(ticksMax_) * us, where);
    if (keep) {
        return true;
    }
    lastLineMs_ = nowMs;
    lastDxgiPresents_ = dxgiPresents;
    lastDxgiRefreshes_ = dxgiRefreshes;
    presents_ = 0;
    for (long &n : perMode_) {
        n = 0;
    }
    errors_ = 0;
    changes_ = notLogged_ = 0;
    queries_ = 0;
    ticksSum_ = ticksMax_ = 0;
    return true;
}

} // namespace core::diag::present_mode
