// The game's sound through Miles (mss32.dll), on the game thread; both games use Miles 6.0 the same way, only where the
// game keeps its sound manager differs (SoundGame). The hooks stay in the game layer, which calls in here:
//   - [audio] background_sound: pause (default) -- when the game window loses the focus (WM_KILLFOCUS, or WM_ACTIVATE
//     minimized) the game calls its sound_stop_all, which closes the music and stops every sound, so another song
//     starts when one comes back; StopAll pauses them instead, and they go on as soon as the window has the focus
//     again (core::display's focus handler: before the game sees that message, so before its music update, and also
//     with our panel open). original: the game's own way.
//   - [audio] ambient_sounds: the city's random ambient sounds so many times per second (0.5 by default;
//     core/sound/ambient_rate.h) instead of once in N main-loop passes (original). OnLoop sets the game's N.
//   - The left-right position (pan) of a sound is kept within Miles's 0..127 (both games give 128 at the right edge).
//   - While one of our windows is open (SetQuiet) no new random ambient sound starts, as under the game's own dialogs.
//   - [debug] sound_probe: every 10 s in the city the log counts the ambient sounds (`sound probe:`); off by default.
// Each part is its own guarded feature: a fault switches only that part off, and the game's way goes on.
#pragma once

namespace core::sound {

// What the sound needs from the game (a constant of the game layer). No member may be nullptr; "none" is a function
// that does nothing or says no (Never, NoOwnPause, NoOwnStop, NoDelay).
struct SoundGame {
    SoundGame(const char *log, bool (*soundOff)(), void **(*streamSlot)(int i), int streams, int (*voiceCount)(),
              void **(*voices)(), bool (*stopAnyway)(), bool (*timedMusic)(), void (*delayTimedMusic)(unsigned long ms),
              void (*ownPause)(bool paused), void (*ownStop)(), volatile int *(*ambientN)(), bool (*cityScreen)())
        : log(log), soundOff(soundOff), streamSlot(streamSlot), streams(streams), voiceCount(voiceCount),
          voices(voices), stopAnyway(stopAnyway), timedMusic(timedMusic), delayTimedMusic(delayTimedMusic),
          ownPause(ownPause), ownStop(ownStop), ambientN(ambientN), cityScreen(cityScreen)
    {
    }

    const char *log;           // the prefix of the log lines, e.g. "zeus"
    bool (*soundOff)();        // the game's sound is off altogether (nothing to pause)
    void **(*streamSlot)(int i); // the game's stream handle i (HSTREAM; 0 when closed), 0 <= i < streams
    int streams;               // how many streams the game keeps (music, speech, ambient), at most kMaxStreams
    int (*voiceCount)();       // how many ordinary voices (HSAMPLE) the game has
    void **(*voices)();        // the game's HSAMPLE array (voiceCount() long), or nullptr before its sound opens
    bool (*stopAnyway)();      // the game plays sounds the pause does not hold (e.g. 3D voices): its own stop
    bool (*timedMusic)();      // the game plays music for a while (a start time and a length): the pause is added
    void (*delayTimedMusic)(unsigned long ms); // ... to its start time
    void (*ownPause)(bool paused); // our own sounds (the game layer's) paused or going on with the game's
    void (*ownStop)();         // ... stopped when the game stops everything
    volatile int *(*ambientN)();        // the game's N of the random ambient sounds (FREQUENCY_L2; 0 before it read its file)
    bool (*cityScreen)();      // the city screen (the only one the game asks for random ambient sounds on)
};

constexpr int kMaxStreams = 8;

// The "none" functions for SoundGame.
bool Never();
void NoOwnPause(bool);
void NoOwnStop();
void NoDelay(unsigned long);

// Miles's functions the core uses (all __stdcall, mss32.dll's exports), so the offline test can give fake ones. Each
// user checks the ones it needs (a missing one switches off only that feature).
struct Miles {
    // The pause.
    void(__stdcall *pauseStream)(void *stream, int onoff);
    int(__stdcall *streamStatus)(void *stream);
    int(__stdcall *sampleStatus)(void *sample);
    void(__stdcall *stopSample)(void *sample);
    void(__stdcall *resumeSample)(void *sample);
    // The layer-1 ambient sounds (core/sound/layer1), also sampleStatus, stopSample, resumeSample.
    void *(__stdcall *allocateSample)(void *driver);
    void(__stdcall *initSample)(void *sample);
    int(__stdcall *setSampleFile)(void *sample, const void *image, int block);
    void(__stdcall *setSampleLoopCount)(void *sample, int count); // 0: for ever
    void(__stdcall *setSampleVolume)(void *sample, int volume);
    void(__stdcall *setSamplePan)(void *sample, int pan);
    void(__stdcall *startSample)(void *sample);
    void(__stdcall *endSample)(void *sample);
};
constexpr int kPlaying = 4, kStopped = 8; // SMP_PLAYING, SMP_STOPPED (a stopped voice resumes where it was)

// mss32.dll's functions (nullptr where it lacks one, all nullptr before it is loaded); the fake ones in the test.
const Miles &MilesTable();

// One of our windows is open (SetQuiet).
bool Quiet();

// From the game layer's settings registration: [audio] background_sound, ambient_sounds; [debug] sound_probe.
void RegisterSettings();

// From the game layer's attach, after its hooks: pauseHooked -- sound_stop_all is hooked and calls StopAll (otherwise
// the pause is off, logged); probeHooked -- ambient_sound_random and sound_play are hooked (AmbientBegin/End,
// BeforePlay/AfterPlay). Sets core::display's focus handler when pauseHooked.
void Attach(const SoundGame &g, bool pauseHooked, bool probeHooked);

// From the game layer's sound_stop_all detour, before the game's own. switchedAway: called because the window lost the
// focus or was minimized (by its return address). True: the sounds were paused instead, do not call the game's.
// False: call the game's (our own sounds and anything paused are dropped first).
bool StopAll(bool switchedAway);

// From the game layer's ambient_sound_random detour: false while one of our windows is open (return at once, no
// sound); otherwise counts the call, then the game's, then AmbientEnd.
bool AmbientBegin();
void AmbientEnd();

// From the game layer's sound_play detour: pan is the request's left-right position (kept within 0..127); voice the
// game's result (-1: not played).
void BeforePlay(int *pan);
void AfterPlay(int voice);

// Game thread: one of our windows (panel, options) is open or closed.
void SetQuiet(bool quiet);

// Once per game pass that runs (game thread), before the game's pass: sounds paused while switched away go on (the
// safety net: normally on the focus), the ambient sounds' N follows the passes per second, the 10 s counts are
// written.
void OnLoop();

// For the offline test: Miles's functions (nullptr: mss32.dll's again) and the state back to the start.
void SetMilesForTest(const Miles *m);
void ResetForTest();
bool PausedForTest();

} // namespace core::sound
