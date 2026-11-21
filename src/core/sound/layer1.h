// The layer-1 ambient sounds (both games): the recordings listed between
// LAYER1_START and LAYER2_START of Audio\sound_fxU.txt, played one at a time on a voice of our own, the way Zeus plays
// them on its 3D voices (Zeus.exe 0x5a2c30 starts one, 0x5a24f0 moves it each pass), but on an ordinary voice: where
// the sound is becomes a volume (the distance) and a left-right position (pan).
//   One sound: it starts above the listener, comes down (15 a second) to 0.8, stays there looping its recording for
//   MIND..MAXD seconds, then rises (15 a second) and ends. While near it moves by the line's motion: C circles from
//   STRT to END degrees at radius X, Z; L moves between nine grid points (STRT to END); B jumps between the four
//   corners (X, Z) every TRSPD ms (B also while it comes down). The original's amounts are per pass; here per second
//   at 20 passes a second (the project's measure of the original's pace).
//   Where the original differs (done the original's way, it sounded cut): the original starts at 60
//   and rises to 80, but nothing is heard beyond 32 (the RSX provider's outer distance), so its recording began
//   unheard and was cut off at 32; here it starts and ends at the edge of hearing (kEdge), the recording plays only
//   as many whole passes as the flight has room for (it ends by itself; Passes), and at the edge it fades out over
//   kFadeMs instead of stopping.
//   After one ends the next starts after the average gap ([audio] nature_sounds_gap) times 0.5..1.5. A new one starts
//   only when the game layer says so (the city screen, the game's ambient sounds on) and none of our windows is open
//   (core::sound::SetQuiet); the one playing goes on over the game's dialogs and when the city is left, as in the
//   original, and ends at once when the game's ambient sounds or [audio] nature_sounds are switched off.
//   Paused with the game's sounds when switched away (core::sound's pause), ended when the game stops all its sounds.
// The pure parts (the file's lines, the flight, the mix) are here for the offline test (test_layer1).
#pragma once

#include <cstddef>
#include <cstdint>

namespace core::sound::layer1 {

// ---------------------------------------------------------------------------------------------
// One line of the file

enum class Motion { Circle, Grid, Corners };

struct Entry {
    char file[64];
    double x, z;         // X, Z: the circle's radii, the corners
    Motion motion;       // MT: C, L, B
    double start, end;   // STRT, END: degrees (C) or grid points (L); -1: chosen at random
    int dir;             // RTDIR: 0 CW (the angle goes down), 1 CCW (up), 2 RCW (one of the two at random)
    double speed;        // TRSPD: degrees (C) or units (L) per pass, ms between two jumps (B)
    int minS, maxS;      // MIND, MAXD: how long it stays near (s)
};

// 1: a sound (e filled). 0: not one (empty, a comment, FREQUENCY_L1). -1: one we do not play (why says what: a
// motion or a pitch change neither game's file uses).
int ParseLine(const char *line, Entry *e, const char **why);

// ---------------------------------------------------------------------------------------------
// Random numbers (our own: the game's rand() is left alone)

struct Rng {
    uint32_t state = 1;
    double Next();          // 0 <= r < 1
    int Below(int n);       // 0..n-1 (0 when n <= 0), as the original's rand() * n >> 15
};

// ---------------------------------------------------------------------------------------------
// The flight of one sound

// Heights: the original starts at 60 (0xf42b30), comes near at 0.8 (0x5e4920) and ends at 80 (0x5e4928); ours starts
// and ends at kEdge, just inside the distance where it is heard (kSilentBeyond below; a grid corner is 5.7 to the
// side, so 31 high is 31.5 away).
constexpr double kEdge = 31.0, kNear = 0.8;
constexpr double kPassesPerSecond = 20.0;
constexpr double kHeightPerSecond = 0.75 * kPassesPerSecond; // 0x5d9228 a pass
constexpr double kMinHopMs = 50.0; // B: at most one jump a pass in the original
constexpr double kFadeMs = 1000.0;  // at the edge: faded out over this long

enum class Phase { Down, Near, Up, Fade, Done };

struct Vec {
    double x, y, z;
};

struct Flight {
    Phase phase = Phase::Down;
    double height = kEdge;
    double fadeMs = 0;                 // Fade: since it began
    double nearMs = 0, durationMs = 0; // time near so far, and how long it stays near
    double movedMs = 0;                // time moving (C, L: from the moment it is near)
    double hopMs = 0;                  // B: since the last jump
    int dir = 0;                       // C: 0 CW, 1 CCW
    double from[2] = {}, to[2] = {};   // C: angles (degrees, [0] only); L: grid points (x, z)
    int corner = -1;                   // B: the corner it is at (-1: above the listener)
    Vec pos{};
};

// Launch one (0x5a2c30).
Flight Launch(const Entry &e, Rng &rng);
// dtMs later (0x5a24f0); at kEdge again it fades (Fade, kFadeMs), then Done. lastCorner: B's last corner, kept from
// one sound to the next as the original keeps it (0x10e41e8): a jump does not land on it again.
void Advance(Flight &f, const Entry &e, double dtMs, Rng &rng, int *lastCorner);
// 1 until it fades, then down to 0 over kFadeMs.
double FadeFactor(const Flight &f);

// How long the whole flight lasts (down, near, up and the fade), in ms.
double FlightMs(const Flight &f);
// The length of a .wav image in ms (its "fmt " byte rate and "data" size), 0 when it cannot be read.
double WavLengthMs(const void *data, size_t size);
// How many whole passes of a recording lengthMs long fit in a flight flightMs long (at least 1): the voice plays
// that many and ends by itself, so no pass begins that the flight would cut. 0: the length is not
// known (it loops until it rises, then plays its last pass).
int Passes(double lengthMs, double flightMs);

// ---------------------------------------------------------------------------------------------
// Where it is -> how loud and where between the speakers

// The listener at the origin, facing +z, +y up, +x to the right (Zeus's sound_3d_open 0x5c8770).
constexpr double kFullDistance = 1.0;   // the RSX provider's inner distance: full volume within it
constexpr double kSilentBeyond = 32.0;  // its outer distance (inner x 32): nothing beyond
constexpr double kEdgeDb = -20.0;       // at the outer distance [inferred: RSX's way, its curve not read]
struct Mix {
    int volume; // 0..127
    int pan;    // 0..127 (64 the middle)
};
// volume: the game's volume for these sounds (0..100), times fade (0..1). Full within kFullDistance, then down
// evenly in dB to kEdgeDb at kSilentBeyond, nothing beyond (at first -6 dB a doubling, which fell
// too fast).
Mix MixAt(const Vec &p, int volume, double fade = 1.0);

// ---------------------------------------------------------------------------------------------
// The player

// What the player needs from the game (a constant of the game layer). No member may be nullptr.
struct Layer1Game {
    Layer1Game(const char *log, bool (*soundStarted)(), void *(*driver)(), bool (*mayStart)(), bool (*ambientOn)(),
               int (*volume)(), const char *const *leftOut, int leftOutCount, bool sameAgain)
        : log(log), soundStarted(soundStarted), driver(driver), mayStart(mayStart), ambientOn(ambientOn),
          volume(volume), leftOut(leftOut), leftOutCount(leftOutCount), sameAgain(sameAgain)
    {
    }

    const char *log;              // the prefix of the log lines, e.g. "zeus"
    bool (*soundStarted)();       // the game's sound has started (its digital driver is open)
    void *(*driver)();            // the game's digital driver (HDIGDRIVER; nullptr while not open)
    bool (*mayStart)();           // the game would start one now (the city screen, its ambient sounds on)
    bool (*ambientOn)();          // the game's ambient sounds are on (off: the one playing ends at once)
    int (*volume)();              // the game's volume for them (0..100)
    const char *const *leftOut;   // recordings of the file not played (file names, any case); may be nullptr ...
    int leftOutCount;             // ... when 0
    bool sameAgain;               // the same recording may come twice in a row (as Zeus does)
};

// From the game layer's attach.
void Attach(const Layer1Game &g);

// From the game layer's settings registration: [audio] nature_sounds, nature_sounds_gap; [debug] nature_sounds_only
// (every other sound silent, a log line every 0.5 s while one plays). original: the description's first sentence,
// what the game itself did with these recordings (the rest of the text is the same in both games).
void RegisterSettings(const char *original);

// What the settings set (also for the offline test). The gap: seconds, the average between the end of one and the
// start of the next.
void SetOn(bool on);
void SetGapSeconds(int seconds);
// [debug]: a line every 0.5 s while one plays (where, how loud, the pan).
void SetTrace(bool on);

// Once per game pass (game thread), from core::sound::OnLoop. now: GetTickCount.
void OnLoop(unsigned long now);
// On a pass that the game does not run (paused under one of our windows, e.g. Yongle's options window, where the
// game's own volumes are changed): the one playing follows the switches and the game's volume at once; nothing moves.
void Refresh();
// From core::sound: paused with the game's sounds (switched away) or going on; the game stopped all its sounds.
void Pause(bool paused);
void Stop();

// For the offline test.
void SetFolderForTest(const char *folder); // with a trailing backslash; nullptr: the game's folder again
void SeedForTest(uint32_t seed);
void ResetForTest();
struct State {
    bool ready, playing, paused;
    int entries, played;
    const Flight *flight;
    const Entry *entry;
    unsigned long next; // when the next starts (0: not planned)
};
State StateForTest();

} // namespace core::sound::layer1
