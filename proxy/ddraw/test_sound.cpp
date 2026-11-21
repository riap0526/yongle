// Offline test of core/sound (not the game; no mss32.dll): the master volume and the sound's pause on fake Miles
// functions and a fake game sound manager.
//   Master volume: a volume set is scaled; read back as the game asked; a change of the master volume asks the game
//   to set its own volumes again, then sets every live handle again (a released or closed one not); off is silence.
//   Pause: switched away, only what plays is paused (streams paused, voices stopped) and our own sounds with them;
//   not switched away the game stops (our own sounds stopped); back: only the same stream handle goes on, only a voice
//   still stopped is resumed, the time away is added to timed music; stopAnyway: the game stops; a fault in the game
//   description: the game stops, the pause is switched off.
//   Also: quiet, the pan limit, the ambient N set from the passes per second.
#include "core/diag/guard.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "core/sound/master_volume.h"
#include "core/sound/sound.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace mv = core::sound::master_volume;
namespace snd = core::sound;

// core::display's focus handler (the real one is in virtual_display.cpp, not linked): kept so the test calls it.
namespace core::display {
FocusHandler g_focus = nullptr;
void SetFocusHandler(FocusHandler handler)
{
    g_focus = handler;
}
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

// ---------------------------------------------------------------------------------------------
// Fake Miles: a handle is an index into these (1..15).

int g_volume[16];   // what Miles has
int g_status[16];   // kPlaying, kStopped, 2 (done)
int g_pausedStream[16];
int g_setCalls = 0;

int H(void *h)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(h));
}
void *P(int i)
{
    return reinterpret_cast<void *>(static_cast<intptr_t>(i));
}

void __stdcall FakeSet(void *h, int v)
{
    g_volume[H(h)] = v;
    ++g_setCalls;
}
int __stdcall FakeGet(void *h)
{
    return g_volume[H(h)];
}
void __stdcall FakeRelease(void *) {}

void __stdcall FakePauseStream(void *h, int onoff)
{
    g_pausedStream[H(h)] = onoff;
    g_status[H(h)] = onoff ? 16 : snd::kPlaying;
}
int __stdcall FakeStatus(void *h)
{
    return g_status[H(h)];
}
void __stdcall FakeStop(void *h)
{
    g_status[H(h)] = snd::kStopped;
}
void __stdcall FakeResume(void *h)
{
    g_status[H(h)] = snd::kPlaying;
}

// ---------------------------------------------------------------------------------------------
// Fake game

int g_reapplied = 0;
void Reapply()
{
    ++g_reapplied;
}

void *g_streams[4];
void *g_voiceArray[4];
bool g_off = false, g_stopAnyway = false, g_timed = false, g_faulty = false;
unsigned long g_delayed = 0;
int g_ownPaused = 0, g_ownResumed = 0, g_ownStopped = 0;
volatile int g_n = 0;
bool g_city = true;

bool SoundOff()
{
    if (g_faulty) {
        volatile int *bad = nullptr;
        return *bad != 0; // the game's manager read where it is not
    }
    return g_off;
}

const snd::SoundGame kGame(
    "test", &SoundOff, [](int i) { return &g_streams[i]; }, 4, [] { return 4; }, [] { return g_voiceArray; },
    [] { return g_stopAnyway; }, [] { return g_timed; }, [](unsigned long ms) { g_delayed += ms + 1; },
    [](bool paused) { paused ? ++g_ownPaused : ++g_ownResumed; }, [] { ++g_ownStopped; },
    [] { return &g_n; }, [] { return g_city; });

void SetValue(const char *section, const char *key, const char *value)
{
    core::settings::Request(core::settings::Find(section, key), value, false);
    core::settings::RunPending();
}

void TestMasterVolume()
{
    static const mv::VolumeGame volume(&Reapply);
    mv::MilesVolume fake{&FakeSet, &FakeSet, &FakeSet, &FakeGet, &FakeGet, &FakeRelease, &FakeRelease, &FakeRelease};
    mv::MilesVolume game{};
    mv::AttachForTest(volume, fake, &game);
    game.setSample(P(1), 100);
    game.setStream(P(2), 80);
    game.set3D(P(3), 60);
    Check(g_volume[1] == 100 && g_volume[2] == 80, "master volume 100%: the volumes as the game set them");
    SetValue("audio", "master_volume", "50");
    Check(g_reapplied == 1, "a change of the master volume: the game sets its own volumes again");
    Check(g_volume[1] == 50 && g_volume[2] == 40 && g_volume[3] == 30, "... and every live handle is set at 50%");
    Check(game.getStream(P(2)) == 80 && game.get3D(P(3)) == 60, "read back: what the game asked for, not what Miles has");
    game.setStream(P(2), 75); // a fade steps by what it reads
    Check(g_volume[2] == 38 && game.getStream(P(2)) == 75, "a stream volume stepped: scaled, read back as asked");
    game.releaseSample(P(1));
    game.closeStream(P(2));
    g_volume[1] = g_volume[2] = -1;
    SetValue("audio", "master_volume", "100");
    Check(g_volume[1] == -1 && g_volume[2] == -1 && g_volume[3] == 60, "released and closed handles are not set again");
    SetValue("audio", "master_sound", "off");
    Check(g_volume[3] == 0 && g_reapplied == 3, "master sound off: silence");
    SetValue("audio", "master_sound", "on");
    Check(g_volume[3] == 60, "master sound on again: the volume the game asked for");
    // Only one heard (the layer-1 sounds' [debug] nature_sounds_only).
    game.setSample(P(4), 90);
    mv::SetSolo(P(4));
    Check(g_volume[3] == 0 && g_volume[4] == 90, "solo: every other handle silent at once, the one kept");
    game.set3D(P(3), 70);
    Check(g_volume[3] == 0 && game.get3D(P(3)) == 70, "... a volume set meanwhile: silent, read back as asked");
    mv::SetSolo(nullptr);
    Check(g_volume[3] == 70 && g_volume[4] == 90, "solo off: every handle at the volume the game asked for");
}

void Reset()
{
    snd::ResetForTest();
    std::memset(g_status, 0, sizeof(g_status));
    std::memset(g_pausedStream, 0, sizeof(g_pausedStream));
    g_streams[0] = P(1);       // music, playing
    g_streams[1] = nullptr;    // speech: closed
    g_streams[2] = P(2);       // ambient: open, done
    g_streams[3] = P(3);       // ambient: playing
    g_status[1] = g_status[3] = snd::kPlaying;
    g_status[2] = 2;
    g_voiceArray[0] = P(10); // playing
    g_voiceArray[1] = P(11); // done
    g_voiceArray[2] = nullptr;
    g_voiceArray[3] = P(12); // playing
    g_status[10] = g_status[12] = snd::kPlaying;
    g_status[11] = 2;
    g_ownPaused = g_ownResumed = g_ownStopped = 0;
    g_delayed = 0;
    g_off = g_stopAnyway = g_timed = g_faulty = false;
}

void TestPause()
{
    static const snd::Miles miles{&FakePauseStream, &FakeStatus, &FakeStatus, &FakeStop, &FakeResume};
    snd::SetMilesForTest(&miles);
    snd::Attach(kGame, true, true);
    Check(core::display::g_focus != nullptr, "the pause hooked: the focus handler is ours");

    Reset();
    Check(!snd::StopAll(false), "not switched away (e.g. the game ends): the game's own stop");
    Check(g_ownStopped == 1 && !snd::PausedForTest(), "... our own sounds stopped too, nothing paused");

    Reset();
    g_timed = true;
    Check(snd::StopAll(true), "switched away: paused instead of the game's stop");
    Check(g_pausedStream[1] == 1 && g_pausedStream[3] == 1 && g_pausedStream[2] == 0,
          "... only the playing streams are paused");
    Check(g_status[10] == snd::kStopped && g_status[12] == snd::kStopped && g_status[11] == 2,
          "... only the playing voices are stopped");
    Check(g_ownPaused == 1, "... our own sounds wait too");
    g_streams[3] = P(4);     // the game opened another ambient stream meanwhile
    g_status[12] = 2;        // a voice the game ended meanwhile
    Sleep(20);
    core::display::g_focus();
    Check(!snd::PausedForTest(), "focus back: going on");
    Check(g_pausedStream[1] == 0 && g_pausedStream[3] == 1, "... only the same stream handle goes on");
    Check(g_status[10] == snd::kPlaying && g_status[12] == 2, "... only a voice still stopped is resumed");
    Check(g_ownResumed == 1, "... our own sounds go on");
    Check(g_delayed >= 1, "... the time away is added to the timed music");

    Reset();
    Check(snd::StopAll(true) && snd::PausedForTest(), "paused again");
    snd::OnLoop();
    Check(!snd::PausedForTest() && g_status[10] == snd::kPlaying, "a game pass without the focus message: going on");
    Check(g_delayed == 0, "... no timed music: nothing added");

    // As Yongle has always done it: the game stopping everything again while paused is the game's own stop (nothing
    // is left to resume).
    Reset();
    Check(snd::StopAll(true), "paused");
    Check(!snd::StopAll(true) && !snd::PausedForTest() && g_ownStopped == 1,
          "the game's stop again while paused: the game's own stop, nothing to resume");

    Reset();
    g_stopAnyway = true;
    Check(!snd::StopAll(true) && !snd::PausedForTest() && g_status[1] == snd::kPlaying,
          "the game plays sounds that cannot wait: the game's own stop");

    Reset();
    SetValue("audio", "background_sound", "original");
    Check(!snd::StopAll(true) && !snd::PausedForTest(), "background_sound original: the game's own stop");
    SetValue("audio", "background_sound", "pause");

    Reset();
    g_off = true;
    Check(!snd::StopAll(true), "the game's sound off: the game's own stop");

    // Quiet, pan, the ambient N.
    Reset();
    snd::SetQuiet(true);
    Check(!snd::AmbientBegin(), "one of our windows open: no new ambient sound");
    snd::SetQuiet(false);
    Check(snd::AmbientBegin(), "closed: the ambient sound as the game");
    snd::AmbientEnd();
    int pan = 128;
    snd::BeforePlay(&pan);
    Check(pan == 127, "pan 128 (the right edge): 127");
    pan = 64;
    snd::BeforePlay(&pan);
    Check(pan == 64, "pan 64: as it was");
    SetValue("audio", "ambient_sounds", "1.0"); // the settings file is not loaded here: the value set by hand
    g_n = 300; // the game read its file
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 700) {
        snd::OnLoop();
        Sleep(5);
    }
    Check(g_n != 300 && g_n >= 2 && g_n < 300, "the ambient N follows the passes per second (1.0 a second)");
    char line[64];
    std::snprintf(line, sizeof(line), "    (N = %d)", g_n);
    std::puts(line);
    SetValue("audio", "ambient_sounds", "original");
    snd::OnLoop();
    Check(g_n == 300, "original: the file's N back");
    g_city = false;
    g_n = 300;
    SetValue("audio", "ambient_sounds", "1.0");
    start = GetTickCount();
    while (GetTickCount() - start < 600) {
        snd::OnLoop();
        Sleep(5);
    }
    Check(g_n == 300, "not the city: N left alone");
    g_city = true;

    // A fault in the game's description (last: the pause stays off).
    Reset();
    g_faulty = true;
    Check(!snd::StopAll(true) && !snd::PausedForTest(), "a fault while pausing: the game's own stop");
    g_faulty = false;
    Reset();
    Check(!snd::StopAll(true), "... and the pause is switched off");
    snd::SetMilesForTest(nullptr);
}

} // namespace

int main()
{
    DeleteFileA("test_sound.log");
    core_log_open(GetModuleHandleW(nullptr), "test_sound.log");
    core::diag::InstallExceptionLog();
    mv::RegisterSettings();
    snd::RegisterSettings();
    TestMasterVolume();
    TestPause();
    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}
