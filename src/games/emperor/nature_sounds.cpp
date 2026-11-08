// The unused nature sounds (0.2.0 item 10): docs/archive/ambient_layer1.md.
//
// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e; mss32.dll Miles 6.0i):
//   - sound_manager 0x1156bc8 + 0x178: the digital driver (HDIGDRIVER). sound_init passes its address to
//     _AIL_waveOutOpen@16 (0x761f49..0x761f50) and the voices are allocated from it (0x761792..0x761799,
//     _AIL_allocate_sample_handle@4) [confirmed: capstone].
//   - 0x1401a30: not 0 when the game's sound started; 0x10de0d3: the sound effects switch; 0x10de134: the sound effects
//     volume (ui_sound 0x5c4500 reads the three; sound_play passes the volume to _AIL_set_sample_volume@8 as it is,
//     0x762a0d..0x762a22) [confirmed: capstone].
//   - Miles's functions are __stdcall (mss32.dll exports, named with @<bytes>). The master volume (master_volume.cpp)
//     hooks _AIL_set_sample_volume@8, so ours is scaled with the game's.
#include "games/emperor/nature_sounds.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "games/emperor/site.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace emperor::nature_sounds {
namespace {

const DWORD kManager = 0x1156bc8;
const Var<int> kSoundStarted{0x1401a30};
const Var<char> kEffectsOn{0x10de0d3};
const Var<int> kEffectsVolume{0x10de134};

using Allocate_fn = void *(__stdcall *)(void *driver);
using Sample_fn = void(__stdcall *)(void *sample);
using SetFile_fn = int(__stdcall *)(void *sample, const void *image, int block);
using SetInt_fn = void(__stdcall *)(void *sample, int value);
using Status_fn = int(__stdcall *)(void *sample);
constexpr int kPlaying = 4, kStopped = 8; // Miles's SMP_PLAYING, SMP_STOPPED

struct Miles {
    Allocate_fn allocate = nullptr;
    Sample_fn init = nullptr, start = nullptr, end = nullptr, stop = nullptr, resume = nullptr;
    SetFile_fn setFile = nullptr;
    SetInt_fn volume = nullptr, pan = nullptr;
    Status_fn status = nullptr;
} g_miles;

// Left out (after listening, 2026-11-09): the buzzing insects (heard as bees or mosquitoes) and the two gongs
// (synthetic, not a real instrument); and the five that are byte for byte a layer 2 sound the game already plays
// (tools/recon/ambient_compare.py: Bamboo_Chimes2a, Bird_B, ceramic_chimes2a, crickets, crickets2); and three that
// are another cut of a layer 2 sound (bird_F1 and bird_G1 of garden.wav, cricket of crickets.wav: heard so in the
// second audition).
const char *const kLeftOut[] = {"bugs.wav",         "insects1.wav",         "large_gong.wav", "medium_gong.wav",
                                "bamboo_chimes2a.wav", "bird_b.wav",        "ceramic_chimes2a.wav",
                                "crickets.wav",     "crickets2.wav",        "bird_f1.wav",    "bird_g1.wav",
                                "cricket.wav"};

struct Entry {
    std::string file;
    double speed; // TRSPD: how fast it circles the listener (radians per second, as we read it)
};
std::vector<Entry> g_entries;

core::diag::Feature g_feature{"unused nature sounds (games/emperor/nature_sounds)"};
bool g_on = true;  // [audio] nature_sounds
int g_gapS = 30;   // [audio] nature_sounds_gap: the average time between two sounds
bool g_ready = false, g_failed = false;
void *g_sample = nullptr;
std::vector<char> g_image; // the wav playing (Miles reads it from here while it plays)
bool g_playing = false, g_paused = false;
DWORD g_next = 0;     // when the next sound starts (0: not planned)
DWORD g_started = 0;  // when the one playing started (moved on by pauses)
DWORD g_pausedAt = 0;
double g_angle0 = 0, g_speed = 0;
int g_lastPan = -1, g_lastVolume = -1;
int g_lastEntry = -1;
long g_played = 0;

std::string GameFolder()
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\') {
        --n;
    }
    return std::string(path, n);
}

bool LeftOut(const char *file)
{
    for (const char *f : kLeftOut) {
        if (_stricmp(f, file) == 0) {
            return true;
        }
    }
    return false;
}

// Audio\sound_fxU.txt, between LAYER1_START and LAYER2_START: file, PR, F/S, X, Y, Z, MT, STRT, END, RTDIR, TRSPD,
// MIND, MAXD.
bool ReadEntries()
{
    const std::string path = GameFolder() + "Audio\\sound_fxU.txt";
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        core_log("nature sounds: %s not found; off", path.c_str());
        return false;
    }
    char line[512];
    bool in = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "LAYER1_START", 12) == 0) {
            in = true;
            continue;
        }
        if (std::strncmp(line, "LAYER2_START", 12) == 0) {
            break;
        }
        if (!in || line[0] == ';' || std::strncmp(line, "FREQUENCY", 9) == 0) {
            continue;
        }
        char file[128], fs[8], mt[8], dir[8];
        int pr, x, y, z, strt, end;
        double speed, mind, maxd;
        if (std::sscanf(line, "%127s %d %7s %d %d %d %7s %d %d %7s %lf %lf %lf", file, &pr, fs, &x, &y, &z, mt, &strt,
                        &end, dir, &speed, &mind, &maxd) != 13 || LeftOut(file)) {
            continue;
        }
        g_entries.push_back(Entry{file, speed});
    }
    std::fclose(f);
    core_log("nature sounds: %d of the game's unused nature sounds from Audio\\sound_fxU.txt (%d left out)",
             static_cast<int>(g_entries.size()), static_cast<int>(sizeof(kLeftOut) / sizeof(kLeftOut[0])));
    return !g_entries.empty();
}

bool Prepare()
{
    HMODULE mss = GetModuleHandleA("mss32.dll");
    if (!mss || *kSoundStarted == 0) {
        return false;
    }
    auto get = [mss](const char *name) { return reinterpret_cast<void *>(GetProcAddress(mss, name)); };
    g_miles.allocate = reinterpret_cast<Allocate_fn>(get("_AIL_allocate_sample_handle@4"));
    g_miles.init = reinterpret_cast<Sample_fn>(get("_AIL_init_sample@4"));
    g_miles.start = reinterpret_cast<Sample_fn>(get("_AIL_start_sample@4"));
    g_miles.end = reinterpret_cast<Sample_fn>(get("_AIL_end_sample@4"));
    g_miles.stop = reinterpret_cast<Sample_fn>(get("_AIL_stop_sample@4"));
    g_miles.resume = reinterpret_cast<Sample_fn>(get("_AIL_resume_sample@4"));
    g_miles.setFile = reinterpret_cast<SetFile_fn>(get("_AIL_set_sample_file@12"));
    g_miles.volume = reinterpret_cast<SetInt_fn>(get("_AIL_set_sample_volume@8"));
    g_miles.pan = reinterpret_cast<SetInt_fn>(get("_AIL_set_sample_pan@8"));
    g_miles.status = reinterpret_cast<Status_fn>(get("_AIL_sample_status@4"));
    const Miles &m = g_miles;
    if (!m.allocate || !m.init || !m.start || !m.end || !m.stop || !m.resume || !m.setFile || !m.volume || !m.pan ||
        !m.status) {
        core_log("nature sounds: mss32.dll lacks a function needed; off");
        g_failed = true;
        return false;
    }
    void *driver = *reinterpret_cast<void **>(kManager + emperor::g_imageShift + 0x178);
    if (!driver) {
        return false; // the game's sound is not open yet
    }
    if (!ReadEntries()) {
        g_failed = true;
        return false;
    }
    g_sample = m.allocate(driver);
    if (!g_sample) {
        core_log("nature sounds: no voice could be allocated; off");
        g_failed = true;
        return false;
    }
    std::srand(GetTickCount());
    return true;
}

double Random()
{
    return std::rand() / (RAND_MAX + 1.0);
}

void Plan(DWORD now)
{
    g_next = now + static_cast<DWORD>(g_gapS * 1000.0 * (0.5 + Random())); // 0.5 to 1.5 times the average gap
}

void End()
{
    if (g_playing && g_sample) {
        g_miles.end(g_sample);
    }
    g_playing = g_paused = false;
}

void StartOne(DWORD now)
{
    int k = static_cast<int>(Random() * g_entries.size());
    if (k == g_lastEntry && g_entries.size() > 1) {
        k = (k + 1) % static_cast<int>(g_entries.size());
    }
    const Entry &e = g_entries[k];
    const std::string path = GameFolder() + "Audio\\Ambient\\Layer1\\" + e.file;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        core_log("nature sounds: %s not found", path.c_str());
        Plan(now);
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    g_image.assign(size > 0 ? static_cast<size_t>(size) : 0, 0);
    const bool read = size > 0 && std::fread(g_image.data(), 1, g_image.size(), f) == g_image.size();
    std::fclose(f);
    g_miles.init(g_sample);
    if (!read || !g_miles.setFile(g_sample, g_image.data(), -1)) {
        core_log("nature sounds: %s could not be played", e.file.c_str());
        Plan(now);
        return;
    }
    g_lastEntry = k;
    g_angle0 = Random() * 6.283185307179586;
    g_speed = e.speed;
    g_lastPan = g_lastVolume = -1;
    g_started = now;
    g_playing = true;
    g_paused = false;
    if (++g_played <= 30) {
        core_log("nature sounds: %s (circling at %.3f; next about %d s after it ends)", e.file.c_str(), e.speed, g_gapS);
    }
}

// Where it is on its circle: left and right from the angle's sine, louder in front (cosine).
void Move(DWORD now, bool first)
{
    const double t = (now - g_started) / 1000.0;
    const double a = g_angle0 - g_speed * t; // clockwise
    int pan = 64 + static_cast<int>(std::lround(63 * std::sin(a)));
    pan = pan < 0 ? 0 : pan > 127 ? 127 : pan;
    const int effects = *kEffectsVolume;
    int volume = static_cast<int>(std::lround(effects * 0.8 * (0.7 + 0.3 * std::cos(a))));
    volume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
    if (pan != g_lastPan) {
        g_miles.pan(g_sample, pan);
        g_lastPan = pan;
    }
    if (volume != g_lastVolume) {
        g_miles.volume(g_sample, volume);
        g_lastVolume = volume;
    }
    if (first) {
        g_miles.start(g_sample);
    }
}

struct Args {
    DWORD now;
    bool city;
};

void LoopNow(void *p)
{
    const Args &a = *static_cast<Args *>(p);
    const bool want = g_on && a.city && *kEffectsOn != 0 && *kSoundStarted != 0;
    if (!g_ready) {
        if (!want || g_failed || !(g_ready = Prepare())) {
            return;
        }
    }
    if (!want) {
        End();
        g_next = 0;
        return;
    }
    if (g_paused) {
        return;
    }
    if (g_playing) {
        if (g_miles.status(g_sample) == kPlaying) {
            Move(a.now, false);
            return;
        }
        g_playing = false; // it ended
        Plan(a.now);
        return;
    }
    if (g_next == 0) {
        Plan(a.now);
        return;
    }
    if (static_cast<LONG>(a.now - g_next) >= 0) {
        StartOne(a.now);
        if (g_playing) {
            Move(a.now, true);
        }
    }
}

void Off()
{
    if (g_sample && g_miles.end) {
        g_miles.end(g_sample);
    }
    g_playing = g_paused = false;
    core_log("nature sounds: off after an error");
}

} // namespace

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "audio", "nature_sounds", "Unused nature sounds",
        "Birds, wind, crickets, frogs and wind chimes that come with the game but that it never played originally. In the city, "
        "now and then one of them circles around you. Uses the sound effects volume.",
        Type::Bool, "on", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_on = std::strcmp(v, "on") == 0;
        return true;
    });
    i = core::settings::Register(Item{
        "audio", "nature_sounds_gap", "Unused nature sounds: time between (s)",
        "About how many seconds pass between two of them (each time a little more or less).",
        Type::Int, "30", Effect::Live, true, true, 15, 90, 15, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_gapS = std::atoi(v);
        g_gapS = g_gapS < 15 ? 15 : g_gapS;
        g_next = 0; // planned again with the new gap
        return true;
    });
}

void OnLoop(unsigned long nowMs, bool city)
{
    if (!core::diag::Enabled(g_feature)) {
        return;
    }
    Args a{nowMs, city};
    if (!core::diag::Guarded(g_feature, &LoopNow, &a)) {
        Off();
    }
}

void Pause(bool paused)
{
    if (!g_ready || !core::diag::Enabled(g_feature) || paused == g_paused || !g_playing) {
        return;
    }
    if (paused) {
        g_miles.stop(g_sample);
        g_pausedAt = GetTickCount();
        g_paused = true;
    }
    else {
        g_started += GetTickCount() - g_pausedAt; // the circle goes on where it was
        if (g_miles.status(g_sample) == kStopped) {
            g_miles.resume(g_sample);
        }
        g_paused = false;
    }
}

void Stop()
{
    if (g_ready && g_playing) {
        End();
        g_next = 0;
    }
}

} // namespace emperor::nature_sounds
