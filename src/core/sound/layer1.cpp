#include "core/sound/layer1.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "core/sound/master_volume.h"
#include "core/sound/sound.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Evidence: Zeus.exe 2.1.4.0 (SHA256 382bcd28...0a57), Ghidra and
// llvm-objdump.
//   ambient_layer1_start 0x5a2c30 (__thiscall, ecx = 0xf3d210, the entry's number): height 60 (0xf42b30), loops for
//     ever (loop count 0), stays (MIND + rand() * (MAXD - MIND) >> 15) s; C: the angle STRT (both STRT and END -1: both
//     at random 0..359; above 360: less 360), (cos a * X, h, sin a * Z); L: grid points STRT and END (-1: at random
//     0..7; outside 0..8: 0) of x {4,4,0,-4,-4,-4,0,4,0}, z {0,-4,-4,-4,0,4,4,4,0}; B: (0, h, 0).
//   sound_frame_update 0x5a24f0 (each pass): down 0.75 (0x5d9228) until below 0.8 (0x5e4920), then near; after the
//     time up 0.75 until above 80 (0x5e4928), then the end. C (not while coming down): the angle - TRSPD (CW, RTDIR 0)
//     or + TRSPD (CCW, 1), kept within 0..360, then the END angle once |(int)(angle - END)| <= TRSPD (0x5a2939).
//     L (not while coming down): x and z each at most TRSPD nearer the END point. B (also coming down): after TRSPD ms,
//     a corner at random (rand() * 4 >> 15, up to 21 tries not the last, 0x10e41e8): (X, Z), (X, -Z), (-X, -Z),
//     (-X, X) -- the last uses X for z, here Z (both games' files: X = Z = 2).
//   The listener at the origin facing +z, up +y (sound_3d_open 0x5c8770). The RSX provider the game opens today
//     (MSSRSX.M3D 815be073...) turns Miles's default distances (min 1, max 200) into an inner 1 and an outer 32.
//   Miles's sample volume 0..127 is taken as linear [inferred]; the master volume (master_volume.cpp) hooks
//     _AIL_set_sample_volume@8, so ours is scaled with the game's.
namespace core::sound::layer1 {

// ---------------------------------------------------------------------------------------------
// The file's lines

int ParseLine(const char *line, Entry *e, const char **why)
{
    *why = nullptr;
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    if (*line == '\0' || *line == '\r' || *line == '\n' || *line == ';' || std::strncmp(line, "FREQUENCY", 9) == 0) {
        return 0;
    }
    char file[64], fs[8], mt[8], dir[8];
    int pr = 0, minS = 0, maxS = 0;
    double x = 0, y = 0, z = 0, start = 0, end = 0, speed = 0;
    if (std::sscanf(line, "%63s %d %7s %lf %lf %lf %7s %lf %lf %7s %lf %d %d", file, &pr, fs, &x, &y, &z, mt, &start,
                    &end, dir, &speed, &minS, &maxS) != 13) {
        *why = "not 13 fields";
        return -1;
    }
    Entry r{};
    std::snprintf(r.file, sizeof(r.file), "%s", file);
    r.x = x;
    r.z = z;
    if (_stricmp(mt, "C") == 0) {
        r.motion = Motion::Circle;
    }
    else if (_stricmp(mt, "L") == 0) {
        r.motion = Motion::Grid;
    }
    else if (_stricmp(mt, "B") == 0) {
        r.motion = Motion::Corners;
    }
    else {
        *why = "a motion neither game's file uses (only C, L and B are played)";
        return -1;
    }
    if (pr != 0 || _stricmp(fs, "S") == 0) {
        *why = "a pitch change neither game's file uses (played without it)";
        // still a sound: logged by the caller, played at its own pitch
    }
    r.dir = _stricmp(dir, "CW") == 0 ? 0 : _stricmp(dir, "CCW") == 0 ? 1 : 2;
    r.start = start;
    r.end = end;
    r.speed = speed;
    r.minS = minS;
    r.maxS = maxS < minS ? minS : maxS;
    *e = r;
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Random numbers

double Rng::Next()
{
    state = state * 1664525u + 1013904223u;
    return (state >> 8) / 16777216.0;
}

int Rng::Below(int n)
{
    return n <= 0 ? 0 : static_cast<int>(Next() * n);
}

// ---------------------------------------------------------------------------------------------
// The flight

namespace {

const int kGridX[9] = {4, 4, 0, -4, -4, -4, 0, 4, 0};
const int kGridZ[9] = {0, -4, -4, -4, 0, 4, 4, 4, 0};

int GridPoint(double v, Rng &rng)
{
    const double p = v == -1 ? rng.Below(8) : v;
    return p < 0 || p > 8 ? 0 : static_cast<int>(p);
}

double Wrap360(double a)
{
    a = std::fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}

// C: the angle after so many degrees of travel from the start towards the end.
double CircleAngle(const Flight &f, double travelled)
{
    const double a0 = f.from[0], a1 = f.to[0];
    const double total = f.dir == 1 ? Wrap360(a1 - a0) : Wrap360(a0 - a1);
    if (travelled >= total) {
        return a1;
    }
    return f.dir == 1 ? a0 + travelled : a0 - travelled;
}

double Toward(double from, double to, double travelled)
{
    if (std::fabs(to - from) <= travelled) {
        return to;
    }
    return to > from ? from + travelled : from - travelled;
}

void Place(Flight &f, const Entry &e)
{
    const double perMs = e.speed * kPassesPerSecond / 1000.0; // C: degrees, L: units
    switch (e.motion) {
    case Motion::Circle: {
        const double a = CircleAngle(f, perMs * f.movedMs) * 3.14159265358979323846 / 180.0;
        f.pos = Vec{std::cos(a) * e.x, f.height, std::sin(a) * e.z};
        break;
    }
    case Motion::Grid:
        f.pos = Vec{Toward(f.from[0], f.to[0], perMs * f.movedMs), f.height,
                    Toward(f.from[1], f.to[1], perMs * f.movedMs)};
        break;
    case Motion::Corners: {
        static const int kSx[4] = {1, 1, -1, -1}, kSz[4] = {1, -1, -1, 1};
        f.pos = f.corner < 0 ? Vec{0, f.height, 0} : Vec{kSx[f.corner] * e.x, f.height, kSz[f.corner] * e.z};
        break;
    }
    }
}

} // namespace

Flight Launch(const Entry &e, Rng &rng)
{
    Flight f;
    f.durationMs = (e.minS + rng.Below(e.maxS - e.minS)) * 1000.0;
    if (e.motion == Motion::Circle) {
        f.dir = e.dir == 2 ? (rng.Below(100) < 50 ? 0 : 1) : e.dir;
        if (e.start == -1 && e.end == -1) {
            f.from[0] = rng.Below(360);
            f.to[0] = rng.Below(360);
        }
        else {
            f.from[0] = e.start > 360 ? e.start - 360 : e.start;
            f.to[0] = e.end > 360 ? e.end - 360 : e.end;
        }
    }
    else if (e.motion == Motion::Grid) {
        const int a = GridPoint(e.start, rng), b = GridPoint(e.end, rng);
        f.from[0] = kGridX[a];
        f.from[1] = kGridZ[a];
        f.to[0] = kGridX[b];
        f.to[1] = kGridZ[b];
    }
    Place(f, e);
    return f;
}

void Advance(Flight &f, const Entry &e, double dtMs, Rng &rng, int *lastCorner)
{
    if (f.phase == Phase::Done) {
        return;
    }
    switch (f.phase) {
    case Phase::Down:
        f.height -= kHeightPerSecond * dtMs / 1000.0;
        if (f.height < kNear) {
            f.height = kNear;
            f.phase = Phase::Near;
            f.nearMs = 0;
        }
        break;
    case Phase::Near:
        f.nearMs += dtMs;
        if (f.nearMs > f.durationMs) {
            f.phase = Phase::Up;
        }
        break;
    case Phase::Up:
        f.height += kHeightPerSecond * dtMs / 1000.0;
        if (f.height > kEdge) {
            f.height = kEdge; // stays at the edge while it fades
            f.phase = Phase::Fade;
            f.fadeMs = 0;
        }
        break;
    case Phase::Fade:
        f.fadeMs += dtMs;
        if (f.fadeMs >= kFadeMs) {
            f.phase = Phase::Done;
        }
        break;
    case Phase::Done:
        break;
    }
    if (e.motion == Motion::Corners) {
        f.hopMs += dtMs;
        const double every = e.speed > kMinHopMs ? e.speed : kMinHopMs;
        if (f.hopMs > every) {
            int c = 0;
            for (int tries = 0; tries <= 20; ++tries) {
                c = rng.Below(4);
                if (c != *lastCorner) {
                    break;
                }
            }
            *lastCorner = f.corner = c;
            f.hopMs = 0;
        }
    }
    else if (f.phase != Phase::Down) {
        f.movedMs += dtMs;
    }
    Place(f, e);
}

// ---------------------------------------------------------------------------------------------
// The mix

double FadeFactor(const Flight &f)
{
    if (f.phase == Phase::Done) {
        return 0;
    }
    if (f.phase != Phase::Fade) {
        return 1;
    }
    const double left = 1.0 - f.fadeMs / kFadeMs;
    return left < 0 ? 0 : left;
}

double FlightMs(const Flight &f)
{
    const double moveMs = (kEdge - kNear) / kHeightPerSecond * 1000.0;
    return moveMs + f.durationMs + moveMs + kFadeMs;
}

double WavLengthMs(const void *data, size_t size)
{
    const unsigned char *p = static_cast<const unsigned char *>(data);
    auto u32 = [p](size_t at) {
        return static_cast<uint32_t>(p[at]) | static_cast<uint32_t>(p[at + 1]) << 8 |
               static_cast<uint32_t>(p[at + 2]) << 16 | static_cast<uint32_t>(p[at + 3]) << 24;
    };
    if (size < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
        return 0;
    }
    uint32_t byteRate = 0, dataBytes = 0;
    for (size_t at = 12; at + 8 <= size;) {
        const uint32_t len = u32(at + 4);
        if (std::memcmp(p + at, "fmt ", 4) == 0 && at + 20 <= size) {
            byteRate = u32(at + 8 + 8); // after the format tag, channels, sample rate
        }
        else if (std::memcmp(p + at, "data", 4) == 0) {
            dataBytes = len;
            break;
        }
        at += 8 + len + (len & 1);
    }
    return byteRate > 0 && dataBytes > 0 ? dataBytes * 1000.0 / byteRate : 0;
}

int Passes(double lengthMs, double flightMs)
{
    if (lengthMs <= 0) {
        return 0;
    }
    const int n = static_cast<int>(flightMs / lengthMs);
    return n < 1 ? 1 : n;
}

Mix MixAt(const Vec &p, int volume, double fade)
{
    const double d = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    Mix m{0, 64};
    if (d > 0) {
        m.pan = 64 + static_cast<int>(std::lround(63.0 * p.x / d));
    }
    if (d <= kSilentBeyond) {
        const double db =
            d <= kFullDistance ? 0.0 : kEdgeDb * (d - kFullDistance) / (kSilentBeyond - kFullDistance);
        m.volume = static_cast<int>(std::lround(volume * std::pow(10.0, db / 20.0) * fade));
    }
    m.volume = m.volume < 0 ? 0 : m.volume > 127 ? 127 : m.volume;
    m.pan = m.pan < 0 ? 0 : m.pan > 127 ? 127 : m.pan;
    return m;
}

// ---------------------------------------------------------------------------------------------
// The player

namespace {

core::diag::Feature g_feature{"layer-1 ambient sounds (core/sound/layer1)"};

const Layer1Game *g_game = nullptr;
bool g_on = true;
int g_gapS = 30;
bool g_trace = false;
std::string g_testFolder;

bool g_ready = false, g_failed = false;
std::vector<Entry> g_entries;
void *g_sample = nullptr;
std::vector<char> g_image; // the wav playing (Miles reads it from here)
Rng g_rng;
bool g_seeded = false;

bool g_playing = false, g_paused = false, g_resync = false;
int g_current = -1, g_last = -1, g_lastCorner = -1;
Flight g_flight;
DWORD g_lastNow = 0, g_next = 0, g_started = 0, g_traced = 0;
int g_lastVolume = -1, g_lastPan = -1;
int g_played = 0;
int g_passes = 0; // of the one playing (0: its length unknown, looping)

const char *const kMotionNames[] = {"circle", "grid", "corners"};

std::string Folder()
{
    if (!g_testFolder.empty()) {
        return g_testFolder;
    }
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\') {
        --n;
    }
    return std::string(path, n);
}

bool LeftOut(const char *file)
{
    for (int i = 0; i < g_game->leftOutCount; ++i) {
        if (_stricmp(g_game->leftOut[i], file) == 0) {
            return true;
        }
    }
    return false;
}

// Audio\sound_fxU.txt, between LAYER1_START and LAYER2_START (only this file: Zeus first looks on the CD and for a
// file of the map, sound_fx<n>.txt; neither is there in the GOG and Steam versions).
bool ReadEntries()
{
    const std::string path = Folder() + "Audio\\sound_fxU.txt";
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        core_log("layer-1 sounds: %s not found; off", path.c_str());
        return false;
    }
    char line[512];
    bool in = false;
    int left = 0, skipped = 0, frequency = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "LAYER1_START", 12) == 0) {
            in = true;
            continue;
        }
        if (std::strncmp(line, "LAYER2_START", 12) == 0) {
            break;
        }
        if (!in) {
            continue;
        }
        if (std::strncmp(line, "FREQUENCY_L1", 12) == 0) {
            std::sscanf(line + 12, "%d", &frequency);
        }
        Entry e;
        const char *why = nullptr;
        const int r = ParseLine(line, &e, &why);
        if (r < 0) {
            line[std::strcspn(line, "\r\n")] = '\0';
            core_log("layer-1 sounds: line not played (%s): %s", why, line);
            ++skipped;
            continue;
        }
        if (r == 0) {
            continue;
        }
        if (why) {
            core_log("layer-1 sounds: %s: %s", e.file, why);
        }
        if (LeftOut(e.file)) {
            ++left;
            continue;
        }
        g_entries.push_back(e);
    }
    std::fclose(f);
    core_log("layer-1 sounds: %d from Audio\\sound_fxU.txt (%d left out, %d not played; the file's FREQUENCY_L1 %d is "
             "not used: the gap is ours)",
             static_cast<int>(g_entries.size()), left, skipped, frequency);
    return !g_entries.empty();
}

bool Prepare()
{
    const Miles &m = MilesTable();
    if (!m.allocateSample || !m.initSample || !m.setSampleFile || !m.setSampleLoopCount || !m.setSampleVolume ||
        !m.setSamplePan || !m.startSample || !m.endSample || !m.stopSample || !m.resumeSample || !m.sampleStatus) {
        core::diag::Disable(g_feature, "mss32.dll lacks a function needed");
        g_failed = true;
        return false;
    }
    void *driver = g_game->driver();
    if (!driver) {
        return false; // the game's sound is not open yet: the next pass
    }
    if (!ReadEntries()) {
        g_failed = true;
        return false;
    }
    g_sample = m.allocateSample(driver);
    if (!g_sample) {
        core_log("layer-1 sounds: no voice of our own could be allocated from the game's driver; off");
        g_failed = true;
        return false;
    }
    if (!g_seeded) {
        g_rng.state = GetTickCount();
        g_seeded = true;
    }
    core_log("layer-1 sounds: ready (%s; a voice of our own; on: %s, about %d s between)", g_game->log,
             g_on ? "yes" : "no", g_gapS);
    return true;
}

void Plan(DWORD now)
{
    g_next = now + static_cast<DWORD>(g_gapS * 1000.0 * (0.5 + g_rng.Next())); // 0.5 to 1.5 times the gap
    if (g_next == 0) {
        g_next = 1;
    }
}

void End(DWORD now, const char *why)
{
    if (!g_playing) {
        return;
    }
    MilesTable().endSample(g_sample);
    g_playing = g_paused = false;
    core_log("layer-1 sounds: %s ended after %.1f s (%s)", g_entries[g_current].file, (now - g_started) / 1000.0,
             why);
}

int g_gameVolume = -1; // the game's volume for them last seen (logged when it changes while one plays)

void Apply(bool first)
{
    const Miles &m = MilesTable();
    const int volume = g_game->volume();
    if (!first && volume != g_gameVolume) {
        core_log("layer-1 sounds: the game's volume for them %d -> %d", g_gameVolume, volume);
    }
    g_gameVolume = volume;
    const Mix mix = MixAt(g_flight.pos, volume, FadeFactor(g_flight));
    if (mix.pan != g_lastPan) {
        m.setSamplePan(g_sample, mix.pan);
        g_lastPan = mix.pan;
    }
    if (mix.volume != g_lastVolume) {
        m.setSampleVolume(g_sample, mix.volume);
        g_lastVolume = mix.volume;
    }
    if (first) {
        m.startSample(g_sample);
    }
}

void Trace(DWORD now, bool always)
{
    if (!g_trace || (!always && now - g_traced < 500)) {
        return;
    }
    g_traced = now;
    static const char *const kPhases[] = {"down", "near", "up", "fading", "done"};
    const Vec &p = g_flight.pos;
    core_log("layer-1 sounds: %s %s at (%.2f, %.2f, %.2f), distance %.2f: volume %d, pan %d",
             g_entries[g_current].file, kPhases[static_cast<int>(g_flight.phase)], p.x, p.y, p.z,
             std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z), g_lastVolume, g_lastPan);
}

void StartOne(DWORD now)
{
    const int n = static_cast<int>(g_entries.size());
    int k = g_rng.Below(n);
    if (!g_game->sameAgain && k == g_last && n > 1) {
        k = (k + 1) % n;
    }
    const Entry &e = g_entries[k];
    const std::string path = Folder() + "Audio\\Ambient\\Layer1\\" + e.file;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        core_log("layer-1 sounds: %s not found", path.c_str());
        Plan(now);
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    g_image.assign(size > 0 ? static_cast<size_t>(size) : 0, 0);
    const bool read = size > 0 && std::fread(g_image.data(), 1, g_image.size(), f) == g_image.size();
    std::fclose(f);
    const Miles &m = MilesTable();
    m.initSample(g_sample);
    if (!read || !m.setSampleFile(g_sample, g_image.data(), -1)) {
        core_log("layer-1 sounds: %s could not be played", e.file);
        Plan(now);
        return;
    }
    g_current = g_last = k;
    g_flight = Launch(e, g_rng);
    // As many whole passes as the flight has room for, so none is cut (the game loops it until it is gone: loop count
    // 0). Length unknown: looping, and the pass playing when it rises is the last.
    const double lengthMs = WavLengthMs(g_image.data(), g_image.size());
    g_passes = Passes(lengthMs, FlightMs(g_flight));
    m.setSampleLoopCount(g_sample, g_passes);
    g_lastVolume = g_lastPan = -1;
    g_started = g_lastNow = g_traced = now;
    g_playing = true;
    g_paused = false;
    ++g_played;
    char way[96];
    if (e.motion == Motion::Corners) {
        std::snprintf(way, sizeof(way), "a jump every %.0f ms", e.speed > kMinHopMs ? e.speed : kMinHopMs);
    }
    else if (e.motion == Motion::Circle) {
        std::snprintf(way, sizeof(way), "%.0f to %.0f degrees %s, %.0f degrees/s", g_flight.from[0], g_flight.to[0],
                      g_flight.dir == 1 ? "CCW" : "CW", e.speed * kPassesPerSecond);
    }
    else {
        std::snprintf(way, sizeof(way), "(%.0f, %.0f) to (%.0f, %.0f), %.2f/s", g_flight.from[0], g_flight.from[1],
                      g_flight.to[0], g_flight.to[1], e.speed * kPassesPerSecond);
    }
    char passes[64];
    if (g_passes > 0) {
        std::snprintf(passes, sizeof(passes), "%d pass(es) of %.1f s in %.1f s", g_passes, lengthMs / 1000.0,
                      FlightMs(g_flight) / 1000.0);
    }
    else {
        std::snprintf(passes, sizeof(passes), "length unknown: looping until it rises");
    }
    core_log("layer-1 sounds: %s (%s, %s) near for %.0f s, %s; %d played", e.file,
             kMotionNames[static_cast<int>(e.motion)], way, g_flight.durationMs / 1000.0, passes, g_played);
    Apply(true);
    Trace(now, true);
}

void LoopNow(void *p)
{
    const DWORD now = *static_cast<DWORD *>(p);
    // Playing on: the switches on (also over the game's dialogs and after the city is left, as in the original).
    const bool on = g_on && g_game->soundStarted() && g_game->ambientOn();
    if (!g_ready) {
        if (!on || !g_game->mayStart() || g_failed || !(g_ready = Prepare())) {
            return;
        }
    }
    master_volume::SetSolo(g_trace ? g_sample : nullptr); // [debug] nature_sounds_only (nothing when unchanged)
    if (!on) {
        End(now, !g_on ? "switched off" : "the game's ambient sounds switched off");
        g_next = 0;
        return;
    }
    if (g_paused) {
        return;
    }
    if (g_playing) {
        if (g_resync) {
            g_lastNow = now; // back from a pause: the time away does not count
            g_resync = false;
        }
        double dt = static_cast<double>(now - g_lastNow);
        dt = dt > 500 ? 500 : dt; // a long stall: not a jump across the sky
        g_lastNow = now;
        const Phase before = g_flight.phase;
        Advance(g_flight, g_entries[g_current], dt, g_rng, &g_lastCorner);
        if (g_passes == 0 && before == Phase::Near && g_flight.phase != Phase::Near) {
            MilesTable().setSampleLoopCount(g_sample, 1); // rising: this pass of the recording is its last
        }
        if (g_flight.phase == Phase::Done) {
            End(now, "faded out at the edge");
            Plan(now);
            return;
        }
        if (MilesTable().sampleStatus(g_sample) != kPlaying) {
            End(now, "played to its end");
            Plan(now);
            return;
        }
        Apply(false);
        Trace(now, false);
        return;
    }
    if (!g_game->mayStart()) {
        g_next = 0; // not the city: planned again when back
        return;
    }
    if (g_next == 0) {
        Plan(now);
        return;
    }
    if (static_cast<LONG>(now - g_next) >= 0 && !Quiet()) { // under one of our windows: when it closes
        StartOne(now);
    }
}

void RefreshNow(void *)
{
    if (!g_ready || !g_playing || g_paused) {
        return;
    }
    if (!(g_on && g_game->soundStarted() && g_game->ambientOn())) {
        End(GetTickCount(), !g_on ? "switched off" : "the game's ambient sounds switched off");
        g_next = 0;
        return;
    }
    Apply(false);
}

void Off()
{
    if (g_sample && g_playing && MilesTable().endSample) {
        MilesTable().endSample(g_sample);
    }
    g_playing = g_paused = false;
    master_volume::SetSolo(nullptr);
    core_log("layer-1 sounds: off after an error");
}

} // namespace

void RegisterSettings(const char *original)
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    static std::string help;
    help = std::string(original) +
           " Played the way the game designed them as 3D sounds, recreated in stereo: now and then in the city, one "
           "comes down from above, moves around you for a while and rises away again. Uses the ambient sounds volume.";
    int i = core::settings::Register(Item{"audio", "nature_sounds", "Nature sounds", help.c_str(), Type::Bool, "on",
                                          Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        SetOn(std::strcmp(v, "on") == 0);
        return true;
    });
    i = core::settings::Register(Item{
        "audio", "nature_sounds_gap", "Nature sounds: time between (s)",
        "About how many seconds pass from the end of one to the start of the next, each time a little more or less. "
        "30: about one every 30 seconds after the last one ends.",
        Type::Int, "30", Effect::Live, true, true, 15, 90, 15, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        const int s = std::atoi(v);
        SetGapSeconds(s < 15 ? 15 : s);
        return true;
    });
    i = core::settings::Register(Item{
        "debug", "nature_sounds_only", "Only nature sounds",
        "For testing: all other sounds and the music are silent, and while a nature sound plays the log says every "
        "half second where it is and how loud. The game's own volumes keep their values.",
        Type::Bool, "off", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        SetTrace(std::strcmp(v, "on") == 0);
        return true;
    });
}

void Attach(const Layer1Game &g)
{
    if (!g.log || !g.soundStarted || !g.driver || !g.mayStart || !g.ambientOn || !g.volume ||
        (g.leftOutCount > 0 && !g.leftOut)) {
        core::diag::Disable(g_feature, "incomplete game description");
        return;
    }
    g_game = &g;
}

void SetOn(bool on)
{
    g_on = on;
}

void SetGapSeconds(int seconds)
{
    g_gapS = seconds < 1 ? 1 : seconds;
    if (!g_playing) {
        g_next = 0; // planned again with the new gap
    }
}

void SetTrace(bool on)
{
    g_trace = on;
}

void OnLoop(unsigned long now)
{
    if (!g_game || !core::diag::Enabled(g_feature)) {
        return;
    }
    DWORD n = now;
    if (!core::diag::Guarded(g_feature, &LoopNow, &n)) {
        Off();
    }
}

void Refresh()
{
    if (!g_game || !core::diag::Enabled(g_feature)) {
        return;
    }
    if (!core::diag::Guarded(g_feature, &RefreshNow, nullptr)) {
        Off();
    }
}

void Pause(bool paused)
{
    if (!g_ready || !core::diag::Enabled(g_feature) || !g_playing || paused == g_paused) {
        return;
    }
    const Miles &m = MilesTable();
    if (paused) {
        m.stopSample(g_sample);
        g_paused = true;
    }
    else {
        if (m.sampleStatus(g_sample) == kStopped) {
            m.resumeSample(g_sample);
        }
        g_paused = false;
        g_resync = true;
    }
}

void Stop()
{
    if (g_ready && g_playing && core::diag::Enabled(g_feature)) {
        End(GetTickCount(), "the game stopped all its sounds");
        g_next = 0;
    }
}

void SetFolderForTest(const char *folder)
{
    g_testFolder = folder ? folder : "";
}

void SeedForTest(uint32_t seed)
{
    g_rng.state = seed;
    g_seeded = true;
}

void ResetForTest()
{
    g_ready = g_failed = g_playing = g_paused = g_resync = false;
    g_entries.clear();
    g_sample = nullptr;
    g_current = g_last = g_lastCorner = -1;
    g_next = g_lastNow = g_started = g_traced = 0;
    g_lastVolume = g_lastPan = g_gameVolume = -1;
    g_played = 0;
    g_feature.off.store(false);
}

State StateForTest()
{
    return State{g_ready,
                 g_playing,
                 g_paused,
                 static_cast<int>(g_entries.size()),
                 g_played,
                 g_playing ? &g_flight : nullptr,
                 g_playing ? &g_entries[g_current] : nullptr,
                 g_next};
}

} // namespace core::sound::layer1
