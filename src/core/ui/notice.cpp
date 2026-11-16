#include "core/ui/notice.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/output/sdl_output.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace core::ui::notice {

namespace {

core::diag::Feature g_feature{"notices in the game's boxes (core/ui/notice)"};
std::atomic<bool> g_attached{false};
Game g_game{"notice", Game::Red::kNone, nullptr, nullptr, nullptr};

// Posted texts (any thread), shown at the next frame.
constexpr int kQueue = 4;
struct Posted {
    char text[kTextSize];
    bool red;
    DWORD at; // GetTickCount when posted
};
SRWLOCK g_lock = SRWLOCK_INIT;
Posted g_queue[kQueue];
int g_queued = 0;

std::atomic<int> g_zoom{-1}; // the zoom to show (-1: nothing new)

bool g_waiting = false; // the last frame did not show the boxes (logged once per wait)

void Flush(void *)
{
    if (!g_game.showsBoxes()) {
        // The game clears its boxes on this screen: the texts and the zoom wait. Too old ones are dropped.
        const DWORD now = GetTickCount();
        AcquireSRWLockExclusive(&g_lock);
        int kept = 0, dropped = 0;
        for (int k = 0; k < g_queued; ++k) {
            if (now - g_queue[k].at > kHoldMs) {
                ++dropped;
                continue;
            }
            g_queue[kept++] = g_queue[k];
        }
        const bool wait = g_queued > 0 && !g_waiting;
        g_queued = kept;
        ReleaseSRWLockExclusive(&g_lock);
        if (wait) {
            g_waiting = true;
            core_log("%s: the game shows no notice boxes on this screen: our messages wait for the city", g_game.logPrefix);
        }
        if (dropped) {
            core_log("%s: %d message(s) not shown (waited %lu s for the city)", g_game.logPrefix, dropped,
                     kHoldMs / 1000);
        }
        return;
    }
    g_waiting = false;
    Posted posted[kQueue];
    AcquireSRWLockExclusive(&g_lock);
    const int n = g_queued;
    std::memcpy(posted, g_queue, sizeof(posted));
    g_queued = 0;
    ReleaseSRWLockExclusive(&g_lock);
    for (int k = 0; k < n && core::diag::Enabled(g_feature); ++k) { // SwitchOff from a box stops the rest
        g_game.newBox(posted[k].text, posted[k].red);
    }
    const int zoom = g_zoom.exchange(-1);
    if (zoom >= 0 && core::diag::Enabled(g_feature)) {
        g_game.zoomBox(zoom);
    }
}

} // namespace

void Install(const Game &game)
{
    g_game = game;
    g_attached.store(true);
    core_log("%s: our messages are shown in the game's notice boxes (also with the game's warnings off)%s",
             g_game.logPrefix, g_game.red == Game::Red::kNone ? "; no red font in this game: errors in the plain one" : "");
}

bool Attached()
{
    return g_attached.load();
}

void SetLogPrefix(const char *prefix)
{
    g_game.logPrefix = prefix;
}

void OnFrame()
{
    if (g_attached.load() && core::diag::Enabled(g_feature)) {
        core::diag::Guarded(g_feature, &Flush, nullptr);
    }
}

void Post(const char *text, bool red)
{
    core_log("%s%s: %s", g_game.logPrefix, red ? " (red)" : "", text);
    if (!g_attached.load() || !core::diag::Enabled(g_feature)) {
        core::output::SetNotice(text, 5000);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_queued < kQueue) {
        std::snprintf(g_queue[g_queued].text, kTextSize, "%s", text);
        g_queue[g_queued].at = GetTickCount();
        g_queue[g_queued++].red = red;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void SwitchOff(const char *why)
{
    core::diag::Disable(g_feature, why);
}

void Zoom(int percent)
{
    if (g_attached.load()) {
        g_zoom.store(percent);
    }
}

void ResetForTest()
{
    g_attached.store(false);
    g_game = Game{"notice", Game::Red::kNone, nullptr, nullptr, nullptr};
    g_waiting = false;
    AcquireSRWLockExclusive(&g_lock);
    g_queued = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_zoom.store(-1);
}

} // namespace core::ui::notice
