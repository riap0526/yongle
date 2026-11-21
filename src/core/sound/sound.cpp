#include "core/sound/sound.h"

#include "core/diag/guard.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "core/sound/ambient_rate.h"
#include "core/sound/layer1.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Moved from games/emperor/sound.cpp (Yongle's) for Zeus: the game's
// sound manager is the game layer's SoundGame; Miles's functions are mss32.dll's exports in both games (Emperor 6.0i,
// Zeus 6.0m) [confirmed: the export tables].
namespace core::sound {
namespace {

core::diag::Feature g_pauseFeature{"sound paused while switched away (core/sound)"};
core::diag::Feature g_rateFeature{"ambient sounds per second (core/sound)"};

const SoundGame *g_game = nullptr; // from Attach
bool g_attached = false;
bool g_pauseWanted = true; // [audio] background_sound = pause
bool g_probeLog = false;   // [debug] sound_probe: the 10 s counts in the log
AmbientRate g_rate;

Miles g_miles{};
const Miles *g_testMiles = nullptr;

// ---------------------------------------------------------------------------------------------
// Paused instead of stopped

struct Paused {
    bool on = false;
    void *streams[kMaxStreams] = {};
    void *samples[64] = {};
    int sampleCount = 0;
    DWORD since = 0;
    bool timedMusic = false;
} g_paused;

bool g_milesLoaded = false;

template <typename F>
void Load(HMODULE mss, F &f, const char *name)
{
    f = reinterpret_cast<F>(GetProcAddress(mss, name));
}

bool MilesFunctions()
{
    const Miles &m = MilesTable();
    return m.pauseStream && m.streamStatus && m.sampleStatus && m.stopSample && m.resumeSample;
}

int Streams()
{
    return g_game->streams < kMaxStreams ? g_game->streams : kMaxStreams;
}

// Game thread, instead of sound_stop_all: what plays is paused and noted. False: do it the game's way.
void Pause(void *result)
{
    *static_cast<bool *>(result) = false;
    if (!MilesFunctions()) {
        core::diag::Disable(g_pauseFeature, "mss32.dll lacks one of the functions needed");
        return;
    }
    if (g_game->soundOff()) {
        return; // the game's sound is off (very old Windows)
    }
    if (g_game->stopAnyway()) {
        core_log("sound: switched away: the game plays sounds that cannot wait (e.g. its 3D sounds): stopped, as "
                 "in the game");
        return;
    }
    Paused p;
    int streams = 0;
    for (int i = 0; i < Streams(); ++i) {
        void *h = *g_game->streamSlot(i);
        if (h && g_miles.streamStatus(h) == kPlaying) {
            g_miles.pauseStream(h, 1);
            p.streams[i] = h;
            ++streams;
        }
    }
    const int voices = g_game->voiceCount();
    void **samples = g_game->voices();
    for (int i = 0; i < voices && samples && p.sampleCount < 64; ++i) {
        if (samples[i] && g_miles.sampleStatus(samples[i]) == kPlaying) {
            g_miles.stopSample(samples[i]);
            p.samples[p.sampleCount++] = samples[i];
        }
    }
    g_game->ownPause(true);
    layer1::Pause(true);
    p.on = true;
    p.since = GetTickCount();
    p.timedMusic = g_game->timedMusic();
    g_paused = p;
    core_log("sound: switched away: %d stream(s) and %d sound(s) paused (the game would stop them)", streams,
             p.sampleCount);
    *static_cast<bool *>(result) = true;
}

void Resume(void *why)
{
    const Paused p = g_paused;
    g_paused.on = false;
    int streams = 0, sounds = 0;
    for (int i = 0; i < Streams(); ++i) {
        if (p.streams[i] && *g_game->streamSlot(i) == p.streams[i]) { // still the stream we paused
            g_miles.pauseStream(p.streams[i], 0);
            ++streams;
        }
    }
    for (int i = 0; i < p.sampleCount; ++i) {
        if (g_miles.sampleStatus(p.samples[i]) == kStopped) {
            g_miles.resumeSample(p.samples[i]);
            ++sounds;
        }
    }
    g_game->ownPause(false);
    layer1::Pause(false);
    const DWORD away = GetTickCount() - p.since;
    if (p.timedMusic && g_game->timedMusic()) {
        g_game->delayTimedMusic(away); // music for a while: the pause does not count
    }
    core_log("sound: back after %lu ms (%s): %d stream(s) and %d sound(s) go on where they were", away,
             static_cast<const char *>(why), streams, sounds);
}

// The game window has the focus again (game thread, in its window procedure, before the game sees the message): what
// was paused goes on now, also while our panel is open (the game's own pass, and so its music update, comes later).
void OnFocus()
{
    if (g_paused.on && !core::diag::Guarded(g_pauseFeature, &Resume, const_cast<char *>("focus back"))) {
        g_paused.on = false;
    }
}

// ---------------------------------------------------------------------------------------------
// The 10 s counts and the pan limit

struct Counts {
    long cityPasses, ambientCalls, ambientAsked, ambientPlayed, allAsked, allPlayed, panLimited;
};
Counts g_counts{};
bool g_inAmbient = false;
DWORD g_since = 0;

bool g_quiet = false; // one of our windows is open (SetQuiet)

void LogCounts(DWORD now)
{
    if (!g_probeLog) {
        g_since = 0;
        g_counts = Counts{};
        return;
    }
    if (g_since == 0) {
        g_since = now;
    }
    if (now - g_since < 10000) {
        return;
    }
    const Counts c = g_counts;
    const double s = (now - g_since) / 1000.0;
    g_counts = Counts{};
    g_since = now;
    if (c.cityPasses == 0 && c.allAsked == 0) {
        return; // nothing of interest (menus)
    }
    const int n = *g_game->ambientN();
    // rand() * N >> 15 == 1 holds for rand() in [ceil(32768 / N), ceil(65536 / N) - 1]: about 1/N of 0..32767.
    const double chance = n > 1 ? ((65536 + n - 1) / n - (32768 + n - 1) / n) / 32768.0 : 0;
    char rate[48];
    if (g_rate.Rate() > 0) {
        std::snprintf(rate, sizeof(rate), "%.2f/s wanted", g_rate.Rate());
    }
    else {
        std::snprintf(rate, sizeof(rate), "original");
    }
    core_log("sound probe: %.1f s: %ld city passes (%.0f/s); ambient_sound_random %ld calls (%.2f/s; expected %.1f "
             "at N = %d, %s), %ld asked for a sound, %ld played (%.2f/s); all sounds %ld asked, %ld played; pan "
             "limited to 127: %ld",
             s, c.cityPasses, c.cityPasses / s, c.ambientCalls, c.ambientCalls / s, c.cityPasses * chance, n, rate,
             c.ambientAsked, c.ambientPlayed, c.ambientPlayed / s, c.allAsked, c.allPlayed, c.panLimited);
}

const char *const kBackground[] = {"pause", "original"};
const char *const kBackgroundLabels[] = {"pause (music and sounds wait where they were)",
                                         "original (stopped; another song when you come back)"};
const char *const kAmbientValues[] = {"original", "0.25", "0.5", "0.75", "1.0"};
const char *const kAmbientLabels[] = {"original (1 in 300 main-loop passes, as the game)",
                                      "0.25 (one in 4 seconds)", "0.5 (one in 2 seconds, default)",
                                      "0.75 (three in 4 seconds)", "1.0 (one each second)"};

} // namespace

const Miles &MilesTable()
{
    if (g_testMiles) {
        g_miles = *g_testMiles;
        return g_miles;
    }
    if (g_milesLoaded) {
        return g_miles;
    }
    HMODULE mss = GetModuleHandleA("mss32.dll");
    if (!mss) {
        return g_miles; // all nullptr: not loaded yet
    }
    Miles &m = g_miles;
    Load(mss, m.pauseStream, "_AIL_pause_stream@8");
    Load(mss, m.streamStatus, "_AIL_stream_status@4");
    Load(mss, m.sampleStatus, "_AIL_sample_status@4");
    Load(mss, m.stopSample, "_AIL_stop_sample@4");
    Load(mss, m.resumeSample, "_AIL_resume_sample@4");
    Load(mss, m.allocateSample, "_AIL_allocate_sample_handle@4");
    Load(mss, m.initSample, "_AIL_init_sample@4");
    Load(mss, m.setSampleFile, "_AIL_set_sample_file@12");
    Load(mss, m.setSampleLoopCount, "_AIL_set_sample_loop_count@8");
    Load(mss, m.setSampleVolume, "_AIL_set_sample_volume@8");
    Load(mss, m.setSamplePan, "_AIL_set_sample_pan@8");
    Load(mss, m.startSample, "_AIL_start_sample@4");
    Load(mss, m.endSample, "_AIL_end_sample@4");
    g_milesLoaded = true;
    return g_miles;
}

bool Quiet()
{
    return g_quiet;
}

bool Never()
{
    return false;
}
void NoOwnPause(bool) {}
void NoOwnStop() {}
void NoDelay(unsigned long) {}

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "audio", "background_sound", "When switched away",
        "What happens to the music and the sounds when the game loses the focus (Alt+Tab, another window, "
        "minimized). pause (default): they wait and go on where they were when you come back. original: the game "
        "stops them all, and another song starts when you come back.",
        Type::Choice, "pause", Effect::Live, true, true, 0, 0, 0, nullptr, kBackground, kBackgroundLabels, 2});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_pauseWanted = std::strcmp(v, "pause") == 0;
        return true;
    });
    i = core::settings::Register(Item{
        "audio", "ambient_sounds", "Ambient sounds",
        "How often the city plays a random ambient sound of its buildings and land. A number: about so many per "
        "second, whatever the resolution and the frame rate (a little less when the same sound is still playing). "
        "original: once in about 300 main-loop passes, as the game does it, so it depends on the resolution, the "
        "frame rate and the computer.",
        Type::Choice, "0.5", Effect::Live, true, true, 0, 0, 0, nullptr, kAmbientValues, kAmbientLabels, 5});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_rate.SetRate(std::strcmp(v, "original") == 0 ? 0 : std::atof(v));
        return true;
    });
    i = core::settings::Register(Item{
        "debug", "sound_probe", "Sound counts in the log",
        "Every 10 s in the city the log counts the main-loop passes, the random ambient sounds asked for and "
        "played, and all sounds played (for checking Ambient sounds).",
        Type::Bool, "off", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_probeLog = std::strcmp(v, "on") == 0;
        return true;
    });
}

void Attach(const SoundGame &g, bool pauseHooked, bool probeHooked)
{
    if (!g.soundOff || !g.streamSlot || !g.voiceCount || !g.voices || !g.stopAnyway || !g.timedMusic ||
        !g.delayTimedMusic || !g.ownPause || !g.ownStop || !g.ambientN || !g.cityScreen || g.streams < 0) {
        core_log("sound: the game layer's description is incomplete: nothing of ours installed");
        core::diag::Disable(g_pauseFeature, "incomplete game description");
        core::diag::Disable(g_rateFeature, "incomplete game description");
        return;
    }
    g_game = &g;
    if (pauseHooked) {
        core::display::SetFocusHandler(&OnFocus);
    }
    else {
        core::diag::Disable(g_pauseFeature, "sound_stop_all is not hooked");
    }
    g_attached = true;
    core_log("sound: installed (%s: background_sound %s, ambient_sounds %s; pan kept within 0..127: %s; counts in "
             "the log: %s)",
             g.log, !pauseHooked ? "not installed" : g_pauseWanted ? "pause" : "original",
             g_rate.Rate() > 0 ? "per second" : "original", probeHooked ? "yes" : "no",
             g_probeLog ? "every 10 s in the city (debug.sound_probe)" : "off");
}

bool StopAll(bool switchedAway)
{
    if (switchedAway && g_attached && g_pauseWanted && !g_paused.on && core::diag::Enabled(g_pauseFeature)) {
        bool paused = false;
        if (core::diag::Guarded(g_pauseFeature, &Pause, &paused) && paused) {
            return true;
        }
    }
    if (g_paused.on) { // the game stops everything after all (e.g. it ends): nothing to resume
        g_paused.on = false;
    }
    if (g_game) {
        g_game->ownStop();
    }
    layer1::Stop();
    return false;
}

bool AmbientBegin()
{
    if (g_quiet) {
        return false; // as under the game's own dialogs: no new ambient sound (the one playing ends by itself)
    }
    ++g_counts.ambientCalls;
    g_inAmbient = true;
    return true;
}

void AmbientEnd()
{
    g_inAmbient = false;
}

void BeforePlay(int *pan)
{
    if (*pan > 127) { // x * 128 / view width is 128 at the right edge; Miles: 0..127
        *pan = 127;
        ++g_counts.panLimited;
    }
}

void AfterPlay(int voice)
{
    ++g_counts.allAsked;
    g_counts.allPlayed += voice != -1;
    if (g_inAmbient) {
        ++g_counts.ambientAsked;
        g_counts.ambientPlayed += voice != -1;
    }
}

void SetQuiet(bool quiet)
{
    g_quiet = quiet;
}

void OnLoop()
{
    if (!g_attached) {
        return;
    }
    // Normally resumed when the focus came back (OnFocus); this is the safety net for a pass that runs without that
    // (e.g. the focus went to another window of this process and the game goes on).
    if (g_paused.on && !core::diag::Guarded(g_pauseFeature, &Resume, const_cast<char *>("game pass"))) {
        g_paused.on = false;
    }
    const DWORD now = GetTickCount();
    const bool city = g_game->cityScreen();
    if (city) {
        ++g_counts.cityPasses;
    }
    if (core::diag::Enabled(g_rateFeature)) {
        struct Args {
            DWORD now;
            bool city;
        } a{now, city};
        core::diag::Guarded(
            g_rateFeature,
            [](void *p) {
                const Args &a = *static_cast<Args *>(p);
                volatile int *slot = g_game->ambientN();
                int n = *slot;
                const int before = n;
                g_rate.Pass(a.now, a.city, &n);
                if (n != before) {
                    *slot = n;
                }
            },
            &a);
    }
    LogCounts(now);
    layer1::OnLoop(now);
}

void SetMilesForTest(const Miles *m)
{
    g_testMiles = m;
    if (!m) {
        g_miles = Miles{};
        g_milesLoaded = false;
    }
}

void ResetForTest()
{
    g_paused = Paused{};
    g_counts = Counts{};
    g_quiet = false;
    g_inAmbient = false;
    g_since = 0;
}

bool PausedForTest()
{
    return g_paused.on;
}

} // namespace core::sound
