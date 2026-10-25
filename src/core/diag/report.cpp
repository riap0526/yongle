#include "core/diag/report.h"

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace core::diag {

namespace {

constexpr long kDetailed = 10; // full log lines per kind; after that only the counts in the rate line

// Game thread only.
int g_frameDepth = 0;
int g_presentsInFrame = 0;
char g_hows[96] = "";
long g_multiLogged = 0;

std::atomic<long> g_multi{0}, g_torn{0};
std::atomic<long> g_tornLogged{0};
std::atomic<long> g_windowLines{0};
constexpr long kWindowLines = 400;

std::atomic<int> g_reports{0}, g_pending{0};

void AddHow(const char *how)
{
    size_t n = std::strlen(g_hows);
    if (n + std::strlen(how) + 3 < sizeof(g_hows)) {
        std::snprintf(g_hows + n, sizeof(g_hows) - n, "%s%s", n ? ", " : "", how);
    }
}

} // namespace

void GameFrameBegin()
{
    if (g_frameDepth++ == 0) {
        g_presentsInFrame = 0;
        g_hows[0] = 0;
    }
}

void GameFrameEnd()
{
    if (g_frameDepth == 0 || --g_frameDepth != 0) {
        return;
    }
    if (g_presentsInFrame > 1) {
        g_multi.fetch_add(1);
        if (++g_multiLogged <= kDetailed) {
            core_log("frame check: %d frames handed to the output in one game frame (%s)%s", g_presentsInFrame,
                     g_hows, g_multiLogged == kDetailed ? "; further ones: counted in the rate line" : "");
        }
    }
}

void NotePresent(const char *how)
{
    if (g_frameDepth > 0) {
        ++g_presentsInFrame;
        AddHow(how ? how : "?");
    }
}

void NoteTornFrame(unsigned before, unsigned after, int w, int h)
{
    g_torn.fetch_add(1);
    long n = g_tornLogged.fetch_add(1) + 1;
    if (n <= kDetailed) {
        core_log("frame check: the output drew a copy that changed while it was drawn (%dx%d, sequence %u -> %u%s)%s",
                 w, h, before, after, before & 1 ? ", it was being written when taken" : "",
                 n == kDetailed ? "; further ones: counted in the rate line" : "");
    }
}

void TakeIntegrity(long *multiPresent, long *torn)
{
    *multiPresent = g_multi.exchange(0);
    *torn = g_torn.exchange(0);
}

void LogWindowEvent(unsigned message, std::size_t wparam, std::ptrdiff_t lparam)
{
    char what[96];
    switch (message) {
    case WM_ACTIVATEAPP:
        std::snprintf(what, sizeof(what), "WM_ACTIVATEAPP %s", wparam ? "active (switched to the game)"
                                                                      : "inactive (switched away)");
        break;
    case WM_ACTIVATE: {
        static const char *const kState[] = {"inactive", "active", "active (click)"};
        unsigned state = LOWORD(wparam);
        std::snprintf(what, sizeof(what), "WM_ACTIVATE %s%s", state < 3 ? kState[state] : "?",
                      HIWORD(wparam) ? ", minimised" : "");
        break;
    }
    case WM_SETFOCUS:
        std::snprintf(what, sizeof(what), "WM_SETFOCUS");
        break;
    case WM_KILLFOCUS:
        std::snprintf(what, sizeof(what), "WM_KILLFOCUS");
        break;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED && wparam != SIZE_RESTORED && wparam != SIZE_MAXIMIZED) {
            return;
        }
        std::snprintf(what, sizeof(what), "WM_SIZE %s %dx%d",
                      wparam == SIZE_MINIMIZED ? "minimised" : wparam == SIZE_MAXIMIZED ? "maximised" : "restored",
                      static_cast<int>(LOWORD(lparam)), static_cast<int>(HIWORD(lparam)));
        break;
    case WM_DISPLAYCHANGE:
        std::snprintf(what, sizeof(what), "WM_DISPLAYCHANGE %dx%d, %u bits", static_cast<int>(LOWORD(lparam)),
                      static_cast<int>(HIWORD(lparam)), static_cast<unsigned>(wparam));
        break;
    default:
        return;
    }
    long n = g_windowLines.fetch_add(1) + 1;
    if (n <= kWindowLines) {
        core_log("window: %s%s", what, n == kWindowLines ? " (no more window events are logged)" : "");
    }
}

int BeginReport(const char *why)
{
    int n = g_reports.fetch_add(1) + 1;
    core_log("==== REPORT #%d (%s) ==== the pictures are saved as report%d_*; the game's statistics follow", n, why,
             n);
    g_pending.store(n);
    return n;
}

int TakeReportRequest()
{
    return g_pending.exchange(0);
}

} // namespace core::diag
