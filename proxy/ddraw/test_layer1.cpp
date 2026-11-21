// Offline test of core/sound/layer1 (not the game; no mss32.dll): the layer-1 ambient sounds
// (shared by both games).
//   The file's lines: comments, FREQUENCY_L1, tabs and extra spaces, a whole-number TRSPD, a motion not played, a pitch
//   change. The flight: coming down from 60 to 0.8 in about 4 s, near for the chosen seconds, rising to 80 in about
//   5 s; C circles CW or CCW and stops at its end angle; L moves between grid points at TRSPD x 20 a second; B jumps
//   between the corners, never the same twice, at most every 50 ms. The mix: full within 1, -6 dB a doubling, nothing
//   beyond 32, the pan from where it is. The player on fake Miles functions and a fake game: a voice of our own, the
//   recording loops, silent while high, louder near, paused and going on without a jump, ends when risen, the next
//   after the gap, at once when the game would not play it or it is switched off, none started under our windows,
//   left-out recordings, not the same twice (Yongle), the game stopping all, a fault (off, the voice ended).
#include "core/diag/guard.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/sound/layer1.h"
#include "core/sound/sound.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace l1 = core::sound::layer1;
namespace snd = core::sound;

// core::display's focus handler (the real one is in virtual_display.cpp, not linked).
namespace core::display {
void SetFocusHandler(FocusHandler) {}
} // namespace core::display

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

// The test's log (core_log writes each line through to the file).
bool LogHas(const char *text)
{
    FILE *f = std::fopen("test_layer1.log", "rb");
    if (!f) {
        return false;
    }
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        all.append(buf, n);
    }
    std::fclose(f);
    return all.find(text) != std::string::npos;
}

bool Near(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------------------------------------
// The file's lines

void TestParse()
{
    l1::Entry e;
    const char *why = nullptr;
    Check(l1::ParseLine("; filename\t\tPR  F/S\n", &e, &why) == 0, "a comment: not a sound");
    Check(l1::ParseLine("\r\n", &e, &why) == 0, "an empty line: not a sound");
    Check(l1::ParseLine("FREQUENCY_L1 2000\r\n", &e, &why) == 0, "FREQUENCY_L1: not a sound");
    Check(l1::ParseLine("wind2.wav\t\t0\tF \t2 \t0 \t2 \tL \t5 \t1\tRCW\t0.025\t9 \t18\r\n", &e, &why) == 1 && !why &&
              std::strcmp(e.file, "wind2.wav") == 0 && e.motion == l1::Motion::Grid && e.start == 5 && e.end == 1 &&
              e.dir == 2 && Near(e.speed, 0.025) && e.minS == 9 && e.maxS == 18 && e.x == 2 && e.z == 2,
          "Zeus's wind2: grid, 5 to 1, at random CW or CCW, 0.025, 9..18 s");
    Check(l1::ParseLine("dark2.wav\t\t0 \tF \t2 \t0 \t2 \tC   \t0 \t180\tRCW\t1.0\t4 \t4\r\n", &e, &why) == 1 &&
              e.motion == l1::Motion::Circle && e.end == 180,
          "Zeus's dark2: 'C' with spaces after it, a circle 0 to 180");
    Check(l1::ParseLine("shimmer6.wav\t\t0\tF \t2 \t0 \t2 \tC \t0 \t270\tCCW\t1\t8  \t8\r\n", &e, &why) == 1 &&
              e.dir == 1 && Near(e.speed, 1.0),
          "Zeus's shimmer6: CCW, a whole-number TRSPD");
    Check(l1::ParseLine("shimmer7.wav\t\t0 \tF \t2 \t0 \t2 \tB \t0 \t0\tCW\t150\t3\t3\r\n", &e, &why) == 1 &&
              e.motion == l1::Motion::Corners && e.dir == 0 && Near(e.speed, 150),
          "Zeus's shimmer7: corners every 150 ms");
    Check(l1::ParseLine("Bird_A.wav\t\t0 \tF \t2 \t0 \t2 \tL \t-1 \t-1\tRCW\t0.025\t14\t14\r\n", &e, &why) == 1 &&
              e.start == -1 && e.end == -1,
          "Emperor's Bird_A: grid points at random");
    Check(l1::ParseLine("walk.wav 0 F 2 0 2 R 0 0 CW 1 3 3\r\n", &e, &why) == -1 && why,
          "MT R (a random walk): not played, a reason given");
    Check(l1::ParseLine("pitch.wav 3 S 2 0 2 C 0 180 CW 1 3 3\r\n", &e, &why) == 1 && why,
          "a pitch change: played at its own pitch, a reason given");
    Check(l1::ParseLine("short.wav 0 F 2 0\r\n", &e, &why) == -1 && why, "too few fields: not played");
}

// ---------------------------------------------------------------------------------------------
// The flight

l1::Entry Make(l1::Motion m, double start, double end, int dir, double speed, int minS, int maxS)
{
    l1::Entry e{};
    std::snprintf(e.file, sizeof(e.file), "x.wav");
    e.x = e.z = 2;
    e.motion = m;
    e.start = start;
    e.end = end;
    e.dir = dir;
    e.speed = speed;
    e.minS = minS;
    e.maxS = maxS;
    return e;
}

// Runs it in 10 ms steps until phase (or Done); returns the milliseconds taken.
double RunUntil(l1::Flight &f, const l1::Entry &e, l1::Rng &rng, l1::Phase phase, int *corner)
{
    double t = 0;
    while (f.phase != phase && f.phase != l1::Phase::Done && t < 600000) {
        l1::Advance(f, e, 10, rng, corner);
        t += 10;
    }
    return t;
}

void TestFlight()
{
    l1::Rng rng;
    rng.state = 12345;
    int corner = -1;

    // C, CW from 0 to 180 at 2.0 a pass: 40 degrees a second, so 180 degrees in 4.5 s.
    l1::Entry c = Make(l1::Motion::Circle, 0, 180, 0, 2.0, 4, 4);
    l1::Flight f = l1::Launch(c, rng);
    Check(f.phase == l1::Phase::Down && Near(f.pos.y, l1::kEdge) && Near(f.pos.x, 2) && Near(f.pos.z, 0),
          "C launched: at the edge of hearing (31 above), at the start angle (cos 0 x X, sin 0 x Z)");
    Check(l1::MixAt(f.pos, 100).volume > 0, "... heard from its start (just inside 32)");
    const double down = RunUntil(f, c, rng, l1::Phase::Near, &corner);
    Check(down >= 1990 && down <= 2030 && Near(f.height, l1::kNear), "comes down from 31 to 0.8 in about 2.01 s");
    // The step it comes near in already moves (as the original's pass): 10 ms, 0.4 degrees.
    Check(Near(f.pos.x, 2, 1e-3) && std::fabs(f.pos.z) < 0.02, "... not moving while it comes down");
    for (int i = 0; i < 225; ++i) { // 2.25 s: 90 degrees
        l1::Advance(f, c, 10, rng, &corner);
    }
    Check(std::fabs(f.pos.x) < 0.05 && Near(f.pos.z, -2, 0.01), "CW: the angle goes down (0 -> -90 after 2.25 s)");
    const double nearMs = 2250 + RunUntil(f, c, rng, l1::Phase::Up, &corner);
    Check(f.phase == l1::Phase::Up && nearMs >= 4000 && nearMs <= 4030, "near for its 4 s, then rising");
    const double up = RunUntil(f, c, rng, l1::Phase::Fade, &corner);
    Check(up >= 1990 && up <= 2030 && Near(f.height, l1::kEdge), "rises from 0.8 to the edge in about 2.01 s");
    Check(Near(l1::FadeFactor(f), 1, 0.02), "... then fades from full");
    for (int i = 0; i < 50; ++i) {
        l1::Advance(f, c, 10, rng, &corner);
    }
    Check(Near(l1::FadeFactor(f), 0.5, 0.02) && Near(f.height, l1::kEdge), "half faded after 0.5 s, at the edge");
    const double fade = RunUntil(f, c, rng, l1::Phase::Done, &corner);
    Check(fade >= 490 && fade <= 510 && l1::FadeFactor(f) == 0, "faded out after 1 s: done");
    Check(Near(f.pos.x, -2, 1e-3) && Near(f.pos.z, 0, 1e-3),
          "... moving on while it rises, stopped at its end angle (180, reached after 4.5 s)");

    // C, CCW 0 -> 270 at 2.0 a pass (40 degrees a second).
    l1::Entry ccw = Make(l1::Motion::Circle, 0, 270, 1, 2.0, 8, 8);
    f = l1::Launch(ccw, rng);
    RunUntil(f, ccw, rng, l1::Phase::Near, &corner);
    for (int i = 0; i < 225; ++i) { // 2.25 s: 90 degrees
        l1::Advance(f, ccw, 10, rng, &corner);
    }
    Check(std::fabs(f.pos.x) < 0.02 && Near(f.pos.z, 2, 0.01), "CCW: the angle goes up (0 -> 90 after 2.25 s)");
    for (int i = 0; i < 500; ++i) {
        l1::Advance(f, ccw, 10, rng, &corner);
    }
    Check(Near(f.pos.x, 0, 1e-3) && Near(f.pos.z, -2, 1e-3), "... stopped at 270");

    // RCW: one of the two, each about half the time.
    int cw = 0;
    l1::Entry rcw = Make(l1::Motion::Circle, 0, 180, 2, 1.0, 3, 3);
    for (int i = 0; i < 400; ++i) {
        cw += l1::Launch(rcw, rng).dir == 0;
    }
    Check(cw > 150 && cw < 250, "RCW: CW or CCW at random");

    // Both angles -1: at random.
    l1::Entry rnd = Make(l1::Motion::Circle, -1, -1, 0, 1.0, 3, 3);
    f = l1::Launch(rnd, rng);
    Check(f.from[0] >= 0 && f.from[0] < 360 && f.to[0] >= 0 && f.to[0] < 360, "C, both angles -1: at random");

    // L from point 1 (4, -4) to point 3 (-4, -4) at 0.025 a pass: 0.5 a second.
    l1::Entry g = Make(l1::Motion::Grid, 1, 3, 2, 0.025, 9, 18);
    f = l1::Launch(g, rng);
    Check(Near(f.pos.x, 4) && Near(f.pos.z, -4) && f.durationMs >= 9000 && f.durationMs <= 17000,
          "L launched at its start point; near for 9..17 s (the original's MIND + rand x (MAXD - MIND) >> 15)");
    RunUntil(f, g, rng, l1::Phase::Near, &corner);
    Check(Near(f.pos.x, 4, 0.01), "... not moving while it comes down");
    for (int i = 0; i < 400; ++i) { // 4 s: 2 units
        l1::Advance(f, g, 10, rng, &corner);
    }
    Check(Near(f.pos.x, 2, 0.01) && Near(f.pos.z, -4), "L: 0.5 a second along x (4 -> 2 after 4 s)");
    // 8 units take 16 s: an entry near for 17 s gets there and stays.
    l1::Entry g17 = Make(l1::Motion::Grid, 1, 3, 2, 0.025, 17, 17);
    f = l1::Launch(g17, rng);
    RunUntil(f, g17, rng, l1::Phase::Near, &corner);
    for (int i = 0; i < 1650; ++i) {
        l1::Advance(f, g17, 10, rng, &corner);
    }
    Check(f.phase == l1::Phase::Near && Near(f.pos.x, -4, 1e-3), "... and stays at its end point");
    l1::Entry bad = Make(l1::Motion::Grid, 9, -2, 2, 1.0, 3, 3);
    f = l1::Launch(bad, rng);
    Check(Near(f.from[0], 4) && Near(f.from[1], 0) && Near(f.to[0], 4) && Near(f.to[1], 0),
          "L, points outside 0..8: point 0");

    // B every 150 ms (shimmer7) and every 2 ms (shimmer3: at most every 50 ms).
    l1::Entry b = Make(l1::Motion::Corners, 0, 0, 0, 150, 3, 3);
    f = l1::Launch(b, rng);
    Check(Near(f.pos.x, 0) && Near(f.pos.z, 0), "B launched above the listener");
    int hops = 0, same = 0, last = corner, off = 0;
    for (int i = 0; i < 300; ++i) { // 3 s, also while coming down
        l1::Advance(f, b, 10, rng, &corner);
        if (f.corner != last) {
            ++hops;
            same += f.corner == last;
            last = f.corner;
        }
        off += !(Near(std::fabs(f.pos.x), 2) && Near(std::fabs(f.pos.z), 2)) && f.corner >= 0;
    }
    Check(hops >= 18 && hops <= 20 && same == 0 && off == 0,
          "B: a corner every 150 ms (also while coming down), never the same twice, at (+-X, +-Z)");
    l1::Entry b2 = Make(l1::Motion::Corners, 0, 0, 0, 2, 3, 3);
    f = l1::Launch(b2, rng);
    hops = 0;
    last = corner;
    for (int i = 0; i < 100; ++i) { // 1 s
        l1::Advance(f, b2, 10, rng, &corner);
        if (f.corner != last) {
            ++hops;
            last = f.corner;
        }
    }
    Check(hops >= 16 && hops <= 20, "B every 2 ms: at most one jump in 50 ms (once a pass in the original)");
}

// ---------------------------------------------------------------------------------------------
// The mix

// A .wav image: "fmt " with byteRate, then (optionally) a "LIST" chunk, then "data" with dataBytes (no samples).
std::string Wav(uint32_t byteRate, uint32_t dataBytes, bool list)
{
    std::string w = "RIFF....WAVE";
    auto u32 = [&w](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            w.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
        }
    };
    auto u16 = [&w](uint16_t v) {
        w.push_back(static_cast<char>(v & 0xff));
        w.push_back(static_cast<char>(v >> 8));
    };
    w += "fmt ";
    u32(16);
    u16(1);                // PCM
    u16(1);                // mono
    u32(byteRate / 2);     // samples per second (16 bit)
    u32(byteRate);
    u16(2);
    u16(16);
    if (list) {
        w += "LIST";
        u32(5); // odd: one byte of padding after it
        w += "abcde";
        w.push_back('\0');
    }
    w += "data";
    u32(dataBytes);
    return w;
}

void TestLength()
{
    std::string w = Wav(44100, 44100 * 5 + 22050, false);
    Check(Near(l1::WavLengthMs(w.data(), w.size()), 5500, 0.01), "a wav's length: 5.5 s from its byte rate and data");
    w = Wav(44100, 44100, true);
    Check(Near(l1::WavLengthMs(w.data(), w.size()), 1000, 0.01), "... past another chunk (odd size, padded)");
    Check(l1::WavLengthMs("not a wav at all", 16) == 0, "not a wav: 0 (unknown)");
    Check(l1::Passes(5310, 8026) == 1 && l1::Passes(2000, 8026) == 4 && l1::Passes(20000, 13000) == 1,
          "passes: as many whole ones as fit, at least 1");
    Check(l1::Passes(0, 8026) == 0, "length unknown: 0 (looping)");
    l1::Flight f;
    f.durationMs = 3000;
    Check(Near(l1::FlightMs(f), 2013.3 + 3000 + 2013.3 + 1000, 1), "a flight: down, near, up and the fade");
}

void TestMix()
{
    Check(l1::MixAt({0, 0.8, 0}, 100).volume == 100, "within 1: full");
    Check(l1::MixAt({0, 2, 0}, 100).volume == 93, "at 2: -0.65 dB");
    Check(l1::MixAt({0, 16.5, 0}, 100).volume == 32, "half way (16.5): -10 dB");
    Check(l1::MixAt({0, 32, 0}, 100).volume == 10, "at 32: -20 dB");
    Check(l1::MixAt({0, 32.5, 0}, 100).volume == 0, "beyond 32: nothing");
    Check(l1::MixAt({0, 2, 0}, 100, 0.5).volume == 46 && l1::MixAt({0, 2, 0}, 100, 0).volume == 0,
          "fading: times the fade");
    Check(l1::MixAt({0, 60, 0}, 100).volume == 0 && l1::MixAt({0, 60, 0}, 100).pan == 64,
          "60 above (the original's start): silent, in the middle");
    Check(l1::MixAt({2, 0.8, 0}, 100).pan > 120, "to the right (+x): right");
    Check(l1::MixAt({-2, 0.8, 0}, 100).pan < 8, "to the left: left");
    Check(l1::MixAt({0, 0.8, 2}, 100).pan == 64 && l1::MixAt({0, 0.8, -2}, 100).pan == 64,
          "in front and behind: the middle");
    Check(l1::MixAt({0, 0.5, 0}, 0).volume == 0, "the game's volume 0: silent");
    Check(l1::MixAt({0, 0.5, 0}, 80).volume == 80, "the game's volume as it is within 1");
}

// ---------------------------------------------------------------------------------------------
// The player: fake Miles and a fake game

struct Voice {
    void *driver = nullptr;
    int inits = 0, starts = 0, ends = 0, stops = 0, resumes = 0, loopCount = -1, volume = -1, pan = -1;
    int status = 2; // SMP_DONE
    std::string file; // the first bytes of the image
} g_voice;
int g_allocs = 0;
bool g_allocFails = false;
const char *const kDriver = "driver";

void *__stdcall FakeAllocate(void *driver)
{
    ++g_allocs;
    g_voice.driver = driver;
    return g_allocFails ? nullptr : &g_voice;
}
void __stdcall FakeInit(void *)
{
    ++g_voice.inits;
    g_voice.status = 2;
    g_voice.loopCount = 1;
}
int __stdcall FakeSetFile(void *, const void *image, int)
{
    g_voice.file.assign(static_cast<const char *>(image), 8);
    return 1;
}
void __stdcall FakeLoop(void *, int n)
{
    g_voice.loopCount = n;
}
void __stdcall FakeVolume(void *, int v)
{
    g_voice.volume = v;
}
void __stdcall FakePan(void *, int p)
{
    g_voice.pan = p;
}
void __stdcall FakeStart(void *)
{
    ++g_voice.starts;
    g_voice.status = snd::kPlaying;
}
void __stdcall FakeEnd(void *)
{
    ++g_voice.ends;
    g_voice.status = 2;
}
void __stdcall FakeStop(void *)
{
    ++g_voice.stops;
    g_voice.status = snd::kStopped;
}
void __stdcall FakeResume(void *)
{
    ++g_voice.resumes;
    g_voice.status = snd::kPlaying;
}
int __stdcall FakeStatus(void *)
{
    return g_voice.status;
}

bool g_started = true, g_city = true, g_ambient = true, g_faulty = false;
int g_volume = 100;

bool AmbientOn()
{
    if (g_faulty) {
        volatile int *bad = nullptr;
        return *bad != 0;
    }
    return g_ambient;
}

void *Driver()
{
    return const_cast<void *>(static_cast<const void *>(kDriver));
}

const char *const kLeftOut[] = {"LEFT.wav"};
const l1::Layer1Game kZeus("test", [] { return g_started; }, &Driver, [] { return g_city && g_ambient; }, &AmbientOn,
                           [] { return g_volume; }, nullptr, 0, true);
const l1::Layer1Game kYongle("test", [] { return g_started; }, &Driver, [] { return g_city && g_ambient; },
                             &AmbientOn, [] { return g_volume; }, kLeftOut, 1, false);

const char *const kFolder = "test_layer1_tmp\\";

void WriteFile(const std::string &path, const char *text)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fputs(text, f);
        std::fclose(f);
    }
}

void MakeFolder()
{
    CreateDirectoryA("test_layer1_tmp", nullptr);
    CreateDirectoryA("test_layer1_tmp\\Audio", nullptr);
    CreateDirectoryA("test_layer1_tmp\\Audio\\Ambient", nullptr);
    CreateDirectoryA("test_layer1_tmp\\Audio\\Ambient\\Layer1", nullptr);
    WriteFile(std::string(kFolder) + "Audio\\sound_fxU.txt",
              "LAYER0_START\r\nnot.wav 0 F 2 0 2 L 1 3 CW 1 3 3\r\n"
              "LAYER1_START\r\n\r\nFREQUENCY_L1 2000\r\n"
              "; filename PR F/S X Y Z MT STRT END RTDIR TRSPD MIND MAXD\r\n"
              "wind1.wav\t\t0 \tF \t2 \t0 \t2 \tL \t1 \t3\tCCW\t0.025\t4\t4\r\n"
              "bells.wav\t\t0 \tF \t2 \t0 \t2 \tC \t0 \t180\tRCW\t1.0\t3  \t3\r\n"
              "left.wav\t\t0 \tF \t2 \t0 \t2 \tC \t0 \t180\tRCW\t1.0\t3  \t3\r\n"
              "walk.wav 0 F 2 0 2 R 0 0 CW 1 3 3\r\n"
              "LAYER2_START\r\nlayer2.wav 0 F 2 0 2 L 1 3 CW 1 3 3\r\n");
    WriteFile(std::string(kFolder) + "Audio\\Ambient\\Layer1\\wind1.wav", "WIND1...wav data");
    WriteFile(std::string(kFolder) + "Audio\\Ambient\\Layer1\\bells.wav", "BELLS...wav data");
    WriteFile(std::string(kFolder) + "Audio\\Ambient\\Layer1\\left.wav", "LEFT....wav data");
}

void RemoveFolder()
{
    DeleteFileA("test_layer1_tmp\\Audio\\Ambient\\Layer1\\wind1.wav");
    DeleteFileA("test_layer1_tmp\\Audio\\Ambient\\Layer1\\bells.wav");
    DeleteFileA("test_layer1_tmp\\Audio\\Ambient\\Layer1\\left.wav");
    DeleteFileA("test_layer1_tmp\\Audio\\sound_fxU.txt");
    RemoveDirectoryA("test_layer1_tmp\\Audio\\Ambient\\Layer1");
    RemoveDirectoryA("test_layer1_tmp\\Audio\\Ambient");
    RemoveDirectoryA("test_layer1_tmp\\Audio");
    RemoveDirectoryA("test_layer1_tmp");
}

snd::Miles g_miles{};

void Fresh(const l1::Layer1Game &game)
{
    l1::ResetForTest();
    l1::SeedForTest(777);
    g_voice = Voice{};
    g_allocs = 0;
    g_allocFails = false;
    g_started = g_city = g_ambient = true;
    g_faulty = false;
    g_volume = 100;
    snd::SetQuiet(false);
    l1::SetOn(true);
    l1::SetGapSeconds(2);
    l1::Attach(game);
}

// Passes every 10 ms from *now for ms.
void Passes(unsigned long *now, unsigned long ms)
{
    for (unsigned long t = 0; t < ms; t += 10) {
        *now += 10;
        l1::OnLoop(*now);
    }
}

// Until one starts (at most 10 s).
bool UntilStarted(unsigned long *now)
{
    for (int i = 0; i < 1000 && !l1::StateForTest().playing; ++i) {
        *now += 10;
        l1::OnLoop(*now);
    }
    return l1::StateForTest().playing;
}

void TestPlayer()
{
    g_miles = snd::Miles{};
    g_miles.sampleStatus = &FakeStatus;
    g_miles.stopSample = &FakeStop;
    g_miles.resumeSample = &FakeResume;
    g_miles.allocateSample = &FakeAllocate;
    g_miles.initSample = &FakeInit;
    g_miles.setSampleFile = &FakeSetFile;
    g_miles.setSampleLoopCount = &FakeLoop;
    g_miles.setSampleVolume = &FakeVolume;
    g_miles.setSamplePan = &FakePan;
    g_miles.startSample = &FakeStart;
    g_miles.endSample = &FakeEnd;
    snd::SetMilesForTest(&g_miles);
    MakeFolder();
    l1::SetFolderForTest(kFolder);

    // Not before the game's sound has started.
    Fresh(kZeus);
    g_started = false;
    unsigned long now = 100000;
    Passes(&now, 100);
    Check(!l1::StateForTest().ready && g_allocs == 0, "the game's sound not started: nothing prepared");
    g_started = true;
    l1::OnLoop(now += 10);
    l1::State s = l1::StateForTest();
    Check(s.ready && g_allocs == 1 && g_voice.driver == kDriver, "started: a voice of our own from the game's driver");
    Check(s.entries == 3, "the file: 3 sounds in layer 1 (not layer 0 or 2, not the R line)");
    Check(s.next != 0 && s.next - now >= 1000 && s.next - now <= 3000, "the first planned after 2 s x 0.5..1.5");

    // One plays.
    Check(UntilStarted(&now), "it starts when the time comes");
    s = l1::StateForTest();
    Check(g_voice.starts == 1 && g_voice.loopCount == 0, "... looping (loop count 0)");
    Check(g_voice.volume >= 8 && g_voice.volume <= 12 && std::abs(g_voice.pan - 64) <= 12,
          "... heard from its start, quietly (about -20 dB at the edge), near the middle");
    const std::string first = s.entry->file;
    const int quiet = g_voice.volume;
    Passes(&now, 1500);
    Check(g_voice.volume > quiet, "louder as it comes near");
    Check(l1::StateForTest().flight->phase == l1::Phase::Down, "... still coming down after 1.5 s");

    // Paused with the game's sounds and going on.
    l1::Pause(true);
    Check(g_voice.stops == 1 && l1::StateForTest().paused, "switched away: our voice stopped too");
    const double height = l1::StateForTest().flight->height, moved = l1::StateForTest().flight->movedMs;
    Passes(&now, 5000);
    Check(l1::StateForTest().flight->height == height && l1::StateForTest().flight->movedMs == moved,
          "... nothing moves while paused");
    l1::Pause(false);
    Check(g_voice.resumes == 1 && !l1::StateForTest().paused, "back: it goes on");
    const double before = l1::StateForTest().flight->movedMs + (60 - l1::StateForTest().flight->height);
    l1::OnLoop(now += 10);
    const double after = l1::StateForTest().flight->movedMs + (60 - l1::StateForTest().flight->height);
    Check(after - before < 1, "... where it was: the time away does not count");

    // Rising: the recording plays to its end (no new pass); at the edge it fades out, then ends; the next is planned.
    for (int i = 0; i < 2000 && l1::StateForTest().flight->phase != l1::Phase::Up; ++i) {
        l1::OnLoop(now += 10);
    }
    Check(g_voice.loopCount == 1, "rising: this pass of the recording is its last (loop count 1)");
    for (int i = 0; i < 2000 && l1::StateForTest().flight->phase != l1::Phase::Fade; ++i) {
        l1::OnLoop(now += 10);
    }
    const int edge = g_voice.volume;
    Passes(&now, 500);
    Check(edge >= 8 && g_voice.volume > 0 && g_voice.volume <= edge / 2 + 1, "at the edge: fading (half after 0.5 s)");
    for (int i = 0; i < 2000 && l1::StateForTest().playing; ++i) {
        l1::OnLoop(now += 10);
    }
    s = l1::StateForTest();
    Check(!s.playing && g_voice.ends == 1 && s.next != 0, "faded out: ended, the next planned");
    Check(LogHas("ended after") && LogHas("(faded out at the edge)"), "log: faded out");

    // The recording ends by itself while rising: ended there, no cut.
    Check(UntilStarted(&now), "the next one");
    for (int i = 0; i < 2000 && l1::StateForTest().flight->phase != l1::Phase::Up; ++i) {
        l1::OnLoop(now += 10);
    }
    g_voice.status = 2; // the last pass ended (SMP_DONE)
    l1::OnLoop(now += 10);
    Check(!l1::StateForTest().playing && l1::StateForTest().next != 0, "the recording ended while rising: done");
    Check(LogHas("(played to its end)"), "log: played to its end");

    // Zeus: the same may come again; Yongle: not twice in a row, the left-out one never.
    int again = 0, rounds = 0;
    std::string last = first;
    for (int i = 0; i < 30; ++i) {
        if (!UntilStarted(&now)) {
            break;
        }
        const std::string f = l1::StateForTest().entry->file;
        again += f == last;
        last = f;
        ++rounds;
        l1::Stop();
    }
    Check(rounds == 30 && again > 0, "Zeus: the same recording may come twice in a row");

    Fresh(kYongle);
    l1::OnLoop(now += 10);
    Check(l1::StateForTest().entries == 2, "Yongle: the left-out recording is not read (any case)");
    again = rounds = 0;
    last.clear();
    for (int i = 0; i < 30; ++i) {
        if (!UntilStarted(&now)) {
            break;
        }
        const std::string f = l1::StateForTest().entry->file;
        again += f == last;
        last = f;
        ++rounds;
        l1::Stop();
    }
    Check(rounds == 30 && again == 0, "Yongle: never the same twice in a row");

    // A dialog over the city, or the city left: the one playing goes on (the original's update does not look at the
    // screen) and ends by itself; none new starts. The game's ambient sounds or ours switched off: ended at once.
    Fresh(kZeus);
    Check(UntilStarted(&now), "playing again");
    g_city = false;
    Passes(&now, 1000);
    Check(l1::StateForTest().playing && g_voice.ends == 0, "a dialog (not the city screen): it goes on");
    for (int i = 0; i < 3000 && l1::StateForTest().playing; ++i) {
        l1::OnLoop(now += 10);
    }
    Check(!l1::StateForTest().playing && g_voice.ends == 1, "... and ends by itself");
    Passes(&now, 10000);
    Check(!l1::StateForTest().playing, "... none new while not the city");
    g_city = true;
    Check(UntilStarted(&now), "back in the city: one starts after the gap");
    g_ambient = false;
    l1::OnLoop(now += 10);
    Check(!l1::StateForTest().playing && g_voice.ends == 2, "the game's ambient sounds switched off: ended at once");
    g_ambient = true;
    Check(UntilStarted(&now), "on again: one starts after the gap");
    l1::SetOn(false);
    l1::OnLoop(now += 10);
    Check(!l1::StateForTest().playing && g_voice.ends == 3, "ours switched off: ended at once");
    l1::SetOn(true);

    // Under one of our windows: none starts; the one playing goes on.
    Fresh(kZeus);
    snd::SetQuiet(true);
    Passes(&now, 10000);
    Check(l1::StateForTest().ready && !l1::StateForTest().playing, "one of our windows open: none starts");
    snd::SetQuiet(false);
    l1::OnLoop(now += 10);
    Check(l1::StateForTest().playing, "closed: the one due starts");
    snd::SetQuiet(true);
    Passes(&now, 200);
    Check(l1::StateForTest().playing, "a window opened while one plays: it goes on");
    snd::SetQuiet(false);

    // The game stops all its sounds.
    l1::Stop();
    Check(!l1::StateForTest().playing && g_voice.ends == 1, "the game stopped all its sounds: ended");

    // The game's volume follows.
    Fresh(kZeus);
    UntilStarted(&now);
    while (l1::StateForTest().flight && l1::StateForTest().flight->phase == l1::Phase::Down) {
        l1::OnLoop(now += 10);
    }
    const int full = g_voice.volume;
    g_volume = 50;
    l1::OnLoop(now += 10);
    Check(full > 0 && std::abs(g_voice.volume * 2 - full) <= 2, "the game's volume halved: ours halved");
    Check(LogHas("the game's volume for them 100 -> 50"), "log: the game's volume changed");

    // Paused under one of our windows (no game pass): Refresh follows the volume and the switch, nothing moves.
    {
        const l1::Flight before = *l1::StateForTest().flight;
        g_volume = 20;
        l1::Refresh();
        const l1::Flight &after = *l1::StateForTest().flight;
        Check(g_voice.volume > 0 && g_voice.volume <= full / 5 + 1, "paused: the game's volume lowered: ours at once");
        Check(after.height == before.height && after.movedMs == before.movedMs && after.nearMs == before.nearMs,
              "... nothing moves");
        g_ambient = false;
        l1::Refresh();
        Check(!l1::StateForTest().playing && g_voice.ends == 1, "paused: the game's ambient sounds off: ended at once");
        g_ambient = true;
        g_volume = 100;
        l1::Refresh();
        Check(!l1::StateForTest().playing, "... nothing starts on a paused pass");
    }

    // No voice: off, logged.
    Fresh(kZeus);
    g_allocFails = true;
    Passes(&now, 100);
    Check(!l1::StateForTest().ready && g_allocs == 1, "no voice to be had: off (asked once)");

    // A fault: off, the voice ended, the game goes on.
    Fresh(kZeus);
    UntilStarted(&now);
    g_faulty = true;
    l1::OnLoop(now += 10);
    Check(!l1::StateForTest().playing && g_voice.ends == 1, "a fault: switched off, our voice ended");
    g_faulty = false;
    Passes(&now, 5000);
    Check(!l1::StateForTest().playing, "... and stays off");

    // A real wav header: whole passes only (bells: 2 s, near 3 s: the flight of 8 s has room for 4).
    {
        const std::string w = Wav(1000, 2000, false);
        FILE *bf = std::fopen((std::string(kFolder) + "Audio\\Ambient\\Layer1\\bells.wav").c_str(), "wb");
        if (bf) {
            std::fwrite(w.data(), 1, w.size(), bf);
            std::fclose(bf);
        }
    }
    Fresh(kZeus);
    bool bells = false;
    for (int i = 0; i < 40 && !bells; ++i) {
        if (!UntilStarted(&now)) {
            break;
        }
        bells = std::strcmp(l1::StateForTest().entry->file, "bells.wav") == 0;
        if (!bells) {
            l1::Stop();
        }
    }
    Check(bells && g_voice.loopCount == 4, "a recording of known length: as many whole passes as fit (4)");
    Check(LogHas("4 pass(es) of 2.0 s in 8.0 s"), "log: the passes");
    for (int i = 0; i < 2000 && l1::StateForTest().playing && l1::StateForTest().flight->phase != l1::Phase::Up; ++i) {
        l1::OnLoop(now += 10);
    }
    Check(g_voice.loopCount == 4, "... not changed when it rises");
    l1::Stop();

    // A Miles function missing: off.
    Fresh(kZeus);
    g_miles.setSampleLoopCount = nullptr;
    Passes(&now, 100);
    Check(!l1::StateForTest().ready && g_allocs == 0, "mss32.dll lacks a function: off");
    g_miles.setSampleLoopCount = &FakeLoop;

    l1::ResetForTest();
    l1::SetFolderForTest(nullptr);
    snd::SetMilesForTest(nullptr);
    RemoveFolder();
}

} // namespace

int main()
{
    DeleteFileA("test_layer1.log");
    core_log_open(GetModuleHandleW(nullptr), "test_layer1.log");
    core::diag::InstallExceptionLog();
    TestParse();
    TestFlight();
    TestLength();
    TestMix();
    TestPlayer();
    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}
