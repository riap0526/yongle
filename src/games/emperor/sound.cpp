#include "games/emperor/sound.h"

#include "core/diag/guard.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "games/emperor/ambient_rate.h"
#include "games/emperor/site.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e; mss32.dll Miles 6.0i, SHA256 f09e6024...7015):
// docs/optimization.md O4, names and calling conventions in docs/symbols.md.
namespace emperor::sound {
namespace {

// game_frame 0x536f89: on the city screen, rand() * [0x1108b34] >> 15 == 1 calls it (probability 1/N per pass).
// __cdecl, no arguments, plain ret.
const Site kAmbient{"ambient_sound_random", 0x5c40c0, {0xa1, 0x30, 0x1a, 0x40, 0x01, 0x83, 0xec, 0x28}};
// sound_play(request): __thiscall (ecx = sound_manager 0x1156bc8), one stack argument (the request, sound_request
// 0x1156d78: +4 sound, +8 volume, +0xc pan, +0x10 loops), ret 4. Returns the voice used, or -1 (the same sound still
// playing on a voice, no free voice, a bad sound number).
const Site kPlay{"sound_play", 0x762884, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x18, 0x89, 0x4d}};
// sound_stop_all: __cdecl, no arguments, plain ret. Closes the four streams and ends every voice. Called by
// game_wndproc on WM_KILLFOCUS (returns to 0x476808) and on WM_ACTIVATE minimized (returns to 0x47683e), and by
// game_main when the game ends (0x475aa5: left alone).
const Site kStopAll{"sound_stop_all", 0x5c1cd0, {0x6a, 0x00, 0xb9, 0xc8, 0x6b, 0x15, 0x01, 0xe8}};
constexpr DWORD kFromKillFocus = 0x476808, kFromMinimized = 0x47683e;

const Var<int> kAmbientN{0x1108b34}; // ambient_frequency_l2: FREQUENCY_L2 of Audio\sound_fxU.txt (300); never saved
const Var<char> kGameScreen{0x10de045};
const DWORD kManager = 0x1156bc8; // sound_manager: +0x198 voices, +0x19c HSAMPLE array, +0x1dc streams, +0x208 off
const Var<int> kMusicMode{0x11550e8};     // music_state + 0: 1 = music for a while (start, length below)
const Var<DWORD> kMusicStart{0x11550e8 + 0x110};
constexpr int kStreams = 4;
constexpr int kPlaying = 4, kStopped = 8; // SMP_PLAYING, SMP_STOPPED (a stopped voice resumes where it was)

using Void_fn = void(__cdecl *)();
using Play_fn = int(__fastcall *)(void *, void *, void *); // __thiscall: ecx = the manager, ret 4
Void_fn o_Ambient, o_StopAll;
Play_fn o_Play;

// Miles, from mss32.dll's exports (all __stdcall).
using PauseStream_fn = void(__stdcall *)(void *stream, int onoff);
using Status_fn = int(__stdcall *)(void *handle);
using Sample_fn = void(__stdcall *)(void *sample);
PauseStream_fn g_pauseStream;
Status_fn g_streamStatus, g_sampleStatus;
Sample_fn g_stopSample, g_resumeSample;

core::diag::Feature g_pauseFeature{"sound paused while switched away (games/emperor/sound)"};
core::diag::Feature g_rateFeature{"ambient sounds per second (games/emperor/sound)"};

bool g_pauseWanted = true; // [sound] background_sound = pause
bool g_probeLog = false;   // [debug] sound_probe: the 10 s counts in the log
AmbientRate g_rate;

// ---------------------------------------------------------------------------------------------
// O4a: paused instead of stopped

struct Paused {
    bool on = false;
    void *streams[kStreams] = {};
    void *samples[64] = {};
    int sampleCount = 0;
    DWORD since = 0;
    int musicMode = 0;
} g_paused;

bool MilesFunctions()
{
    if (g_pauseStream) {
        return true;
    }
    HMODULE mss = GetModuleHandleA("mss32.dll");
    if (!mss) {
        return false;
    }
    g_pauseStream = reinterpret_cast<PauseStream_fn>(GetProcAddress(mss, "_AIL_pause_stream@8"));
    g_streamStatus = reinterpret_cast<Status_fn>(GetProcAddress(mss, "_AIL_stream_status@4"));
    g_sampleStatus = reinterpret_cast<Status_fn>(GetProcAddress(mss, "_AIL_sample_status@4"));
    g_stopSample = reinterpret_cast<Sample_fn>(GetProcAddress(mss, "_AIL_stop_sample@4"));
    g_resumeSample = reinterpret_cast<Sample_fn>(GetProcAddress(mss, "_AIL_resume_sample@4"));
    if (!g_pauseStream || !g_streamStatus || !g_sampleStatus || !g_stopSample || !g_resumeSample) {
        g_pauseStream = nullptr;
        return false;
    }
    return true;
}

BYTE *Manager()
{
    return reinterpret_cast<BYTE *>(kManager + g_imageShift);
}

void **StreamSlot(int i)
{
    return reinterpret_cast<void **>(Manager() + 0x1dc + 4 * i);
}

// Game thread, instead of sound_stop_all: what plays is paused and noted. False: do it the game's way.
void Pause(void *result)
{
    *static_cast<bool *>(result) = false;
    if (!MilesFunctions()) {
        core::diag::Disable(g_pauseFeature, "mss32.dll lacks one of the functions needed");
        return;
    }
    BYTE *m = Manager();
    if (*reinterpret_cast<int *>(m + 0x208) != 0) {
        return; // the game's sound is off (very old Windows)
    }
    Paused p;
    int streams = 0;
    for (int i = 0; i < kStreams; ++i) {
        void *h = *StreamSlot(i);
        if (h && g_streamStatus(h) == kPlaying) {
            g_pauseStream(h, 1);
            p.streams[i] = h;
            ++streams;
        }
    }
    const int voices = *reinterpret_cast<int *>(m + 0x198);
    void **samples = *reinterpret_cast<void ***>(m + 0x19c);
    for (int i = 0; i < voices && samples && p.sampleCount < 64; ++i) {
        if (samples[i] && g_sampleStatus(samples[i]) == kPlaying) {
            g_stopSample(samples[i]);
            p.samples[p.sampleCount++] = samples[i];
        }
    }
    p.on = true;
    p.since = GetTickCount();
    p.musicMode = *kMusicMode;
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
    for (int i = 0; i < kStreams; ++i) {
        if (p.streams[i] && *StreamSlot(i) == p.streams[i]) { // still the stream we paused
            g_pauseStream(p.streams[i], 0);
            ++streams;
        }
    }
    for (int i = 0; i < p.sampleCount; ++i) {
        if (g_sampleStatus(p.samples[i]) == kStopped) {
            g_resumeSample(p.samples[i]);
            ++sounds;
        }
    }
    const DWORD away = GetTickCount() - p.since;
    if (p.musicMode == 1 && *kMusicMode == 1) {
        *kMusicStart = *kMusicStart + away; // music for a while: the pause does not count
    }
    core_log("sound: back after %lu ms (%s): %d stream(s) and %d sound(s) go on where they were", away,
             static_cast<const char *>(why), streams, sounds);
}

void __cdecl StopAll_detour()
{
    const DWORD from = reinterpret_cast<DWORD>(__builtin_return_address(0));
    const bool focus = from == kFromKillFocus + g_imageShift || from == kFromMinimized + g_imageShift;
    if (focus && g_pauseWanted && !g_paused.on && core::diag::Enabled(g_pauseFeature)) {
        bool paused = false;
        if (core::diag::Guarded(g_pauseFeature, &Pause, &paused) && paused) {
            return;
        }
    }
    if (g_paused.on) { // the game stops everything after all (e.g. it ends): nothing to resume
        g_paused.on = false;
    }
    Original(o_StopAll, kStopAll)();
}

// ---------------------------------------------------------------------------------------------
// The 10 s counts (O4b) and the pan limit

struct Counts {
    long cityPasses, ambientCalls, ambientAsked, ambientPlayed, allAsked, allPlayed, panLimited;
};
Counts g_counts{};
bool g_inAmbient = false;
DWORD g_since = 0;

void __cdecl Ambient_detour()
{
    ++g_counts.ambientCalls;
    g_inAmbient = true;
    Original(o_Ambient, kAmbient)();
    g_inAmbient = false;
}

int __fastcall Play_detour(void *self, void *, void *request)
{
    int *pan = static_cast<int *>(request) + 3; // +0xc: x * 128 / view width is 128 at the right edge; Miles: 0..127
    if (*pan > 127) {
        *pan = 127;
        ++g_counts.panLimited;
    }
    const int voice = Original(o_Play, kPlay)(self, nullptr, request);
    ++g_counts.allAsked;
    g_counts.allPlayed += voice != -1;
    if (g_inAmbient) {
        ++g_counts.ambientAsked;
        g_counts.ambientPlayed += voice != -1;
    }
    return voice;
}

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
    const int n = *kAmbientN;
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

bool g_attached = false;

// The game window has the focus again (game thread, in its window procedure, before the game sees the message): what
// was paused goes on now, also while our panel is open (the game's own pass, and so music_update, comes later).
void OnFocus()
{
    if (g_paused.on && !core::diag::Guarded(g_pauseFeature, &Resume, const_cast<char *>("focus back"))) {
        g_paused.on = false;
    }
}

} // namespace

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "sound", "background_sound", "When switched away",
        "What happens to the music and the sounds when the game loses the focus (Alt+Tab, another window, "
        "minimized). pause (default): they wait and go on where they were when you come back. original: the game "
        "stops them all, and another song starts when you come back.",
        Type::Choice, "pause", Effect::Live, true, true, 0, 0, 0, nullptr, kBackground, kBackgroundLabels, 2});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_pauseWanted = std::strcmp(v, "pause") == 0;
        return true;
    });
    i = core::settings::Register(Item{
        "sound", "ambient_sounds", "Ambient sounds",
        "How often the city plays a random ambient sound (birds, water, workshops). A number: about so many per "
        "second whatever the resolution (a little less when the same sound is still playing); 0.5 (default) is "
        "about what the game gave on the computers of its time. original: once in about 300 main-loop passes, as "
        "the game does it -- so it depends on the resolution and the computer (about one per second at 1024x768 "
        "on a fast computer, far fewer at a large resolution).",
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

bool Attach()
{
    if (!Matches(kAmbient) || !Matches(kPlay)) {
        core_log("sound: %s does not match: no counts, no pan limit", Matches(kAmbient) ? kPlay.name : kAmbient.name);
    }
    else {
        Install(kAmbient, &Ambient_detour, o_Ambient);
        Install(kPlay, &Play_detour, o_Play);
    }
    if (!Matches(kStopAll)) {
        core_log("sound: %s does not match: sounds stop when switched away, as in the game", kStopAll.name);
        core::diag::Disable(g_pauseFeature, "sound_stop_all does not match");
    }
    else {
        Install(kStopAll, &StopAll_detour, o_StopAll);
        core::display::SetFocusHandler(&OnFocus);
    }
    g_attached = true;
    core_log("sound: installed (background_sound %s, ambient_sounds %s; pan kept within 0..127; counts in the log: "
             "%s)",
             g_pauseWanted ? "pause" : "original", g_rate.Rate() > 0 ? "per second" : "original",
             g_probeLog ? "every 10 s in the city (debug.sound_probe)" : "off");
    return true;
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
    const bool city = *kGameScreen == 1;
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
                int n = *kAmbientN;
                const int before = n;
                g_rate.Pass(a.now, a.city, &n);
                if (n != before) {
                    *kAmbientN = n;
                }
            },
            &a);
    }
    LogCounts(now);
}

} // namespace emperor::sound
