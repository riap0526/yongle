#include "games/emperor/sound.h"

#include "core/log.h"
#include "core/sound/layer1.h"
#include "core/sound/sound.h"
#include "games/emperor/site.h"

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e; mss32.dll Miles 6.0i, SHA256 f09e6024...7015):
// names and calling conventions checked by disassembly. What is done with them is the core's (core/sound/sound.h).
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
// game_main when the game ends (0x475aa5: left alone). It also writes -1 to 0x1401a20, which the pause leaves alone.
const Site kStopAll{"sound_stop_all", 0x5c1cd0, {0x6a, 0x00, 0xb9, 0xc8, 0x6b, 0x15, 0x01, 0xe8}};
constexpr DWORD kFromKillFocus = 0x476808, kFromMinimized = 0x47683e;

const Var<int> kAmbientN{0x1108b34}; // ambient_frequency_l2: FREQUENCY_L2 of Audio\sound_fxU.txt (300); never saved
const Var<char> kGameScreen{0x10de045};
const DWORD kManager = 0x1156bc8; // sound_manager: +0x198 voices, +0x19c HSAMPLE array, +0x1dc streams, +0x208 off
const Var<int> kMusicMode{0x11550e8};     // music_state + 0: 1 = music for a while (start, length below)
const Var<DWORD> kMusicStart{0x11550e8 + 0x110};
constexpr int kStreams = 4;

using Void_fn = void(__cdecl *)();
using Play_fn = int(__fastcall *)(void *, void *, void *); // __thiscall: ecx = the manager, ret 4
Void_fn o_Ambient, o_StopAll;
Play_fn o_Play;

BYTE *Manager()
{
    return reinterpret_cast<BYTE *>(kManager + g_imageShift);
}

const core::sound::SoundGame kGame(
    "emperor", [] { return *reinterpret_cast<int *>(Manager() + 0x208) != 0; },
    [](int i) { return reinterpret_cast<void **>(Manager() + 0x1dc + 4 * i); }, kStreams,
    [] { return *reinterpret_cast<int *>(Manager() + 0x198); },
    [] { return *reinterpret_cast<void ***>(Manager() + 0x19c); },
    &core::sound::Never, // the 3D voices: nothing plays on them (sound_3d_start 0x763a74 has no caller)
    [] { return *kMusicMode == 1; }, [](unsigned long ms) { *kMusicStart = *kMusicStart + ms; },
    &core::sound::NoOwnPause, &core::sound::NoOwnStop, // the nature sounds are the core's: it pauses and stops them
    [] { return &*kAmbientN; }, [] { return *kGameScreen == 1; });

// The nature sounds (0.2.0; the core's layer-1 player, shared with Zeus): the game lists them in
// Audio\sound_fxU.txt but never plays them (its 3D voices have no caller, above). Evidence:
//   0x1401a30: not 0 once the game's sound started; sound_manager + 0x178: the digital driver (sound_init passes its
//     address to _AIL_waveOutOpen@16, 0x761f49..0x761f50) [confirmed: capstone].
//   The ambient sounds' switch 0x10de130 (byte) and volume 0x10de140 (0..100) of the game's sound options
//     (options_original.cpp), as Zeus uses its own for them.
//   The city: screens 1, 0x48 and 0x49.
// Left out (after listening): the buzzing insects (heard as bees or mosquitoes) and the two gongs
// (synthetic, not a real instrument); and the five that are byte for byte a layer 2 sound the game already plays
// (found by comparing the files: Bamboo_Chimes2a, Bird_B, ceramic_chimes2a, crickets, crickets2); and three that
// are another cut of a layer 2 sound (bird_F1 and bird_G1 of garden.wav, cricket of crickets.wav: heard so in the
// second audition).
const Var<int> kSoundStarted{0x1401a30};
const Var<char> kAmbientOn{0x10de130};
const Var<int> kAmbientVolume{0x10de140};
const char *const kLeftOut[] = {"bugs.wav",         "insects1.wav",         "large_gong.wav", "medium_gong.wav",
                                "bamboo_chimes2a.wav", "bird_b.wav",        "ceramic_chimes2a.wav",
                                "crickets.wav",     "crickets2.wav",        "bird_f1.wav",    "bird_g1.wav",
                                "cricket.wav"};
const core::sound::layer1::Layer1Game kLayer1(
    "emperor", [] { return *kSoundStarted != 0; },
    [] { return *reinterpret_cast<void *volatile *>(Manager() + 0x178); },
    [] {
        const char s = *kGameScreen;
        return (s == 1 || s == 0x48 || s == 0x49) && *kAmbientOn != 0;
    },
    [] { return *kAmbientOn != 0; }, [] { return *kAmbientVolume; }, kLeftOut, static_cast<int>(sizeof(kLeftOut) / sizeof(kLeftOut[0])), false);

void __cdecl StopAll_detour()
{
    const DWORD from = reinterpret_cast<DWORD>(__builtin_return_address(0));
    const bool away = from == kFromKillFocus + g_imageShift || from == kFromMinimized + g_imageShift;
    if (core::sound::StopAll(away)) {
        return;
    }
    Original(o_StopAll, kStopAll)();
}

void __cdecl Ambient_detour()
{
    if (!core::sound::AmbientBegin()) {
        return;
    }
    static bool told = false;
    if (!told) {
        // Logged once (players asked for the unused nature sounds): the state of the game's 3D channels, which nothing plays on
        // (sound_3d_start 0x763a74 has no caller): the provider the game chose at start-up (sound_3d_open 0x76307d:
        // EAX, A3D, Hardware, RSX, Software in that order; 5 = none), its handle, how many 3D samples were allocated,
        // the 3D-off flag 0x1108b40 (set when no provider opened) and sound_manager + 0x208 (sound off).
        told = true;
        const BYTE *m = Manager();
        static const char *const kKinds[] = {"EAX", "A3D", "Hardware", "RSX", "Software", "none"};
        const int kind = *reinterpret_cast<const int *>(m + 0x164);
        core_log("sound: 3D channels: provider %s (%d), handle 0x%x, %d 3D samples, 3D off flag %d, sound off %d",
                 kind >= 0 && kind <= 5 ? kKinds[kind] : "?", kind, *reinterpret_cast<const unsigned *>(m + 0x1f0),
                 *reinterpret_cast<const int *>(m + 0x1ec), *reinterpret_cast<const int *>(0x1108b40 + g_imageShift),
                 *reinterpret_cast<const int *>(m + 0x208));
    }
    Original(o_Ambient, kAmbient)();
    core::sound::AmbientEnd();
}

int __fastcall Play_detour(void *self, void *, void *request)
{
    core::sound::BeforePlay(static_cast<int *>(request) + 3); // +0xc
    const int voice = Original(o_Play, kPlay)(self, nullptr, request);
    core::sound::AfterPlay(voice);
    return voice;
}

bool g_attached = false;

} // namespace

bool Attach()
{
    bool probe = false, pause = false;
    if (!Matches(kAmbient) || !Matches(kPlay)) {
        core_log("sound: %s does not match: no counts, no pan limit", Matches(kAmbient) ? kPlay.name : kAmbient.name);
    }
    else {
        Install(kAmbient, &Ambient_detour, o_Ambient);
        Install(kPlay, &Play_detour, o_Play);
        probe = true;
    }
    if (!Matches(kStopAll)) {
        core_log("sound: %s does not match: sounds stop when switched away, as in the game", kStopAll.name);
    }
    else {
        Install(kStopAll, &StopAll_detour, o_StopAll);
        pause = true;
    }
    core::sound::Attach(kGame, pause, probe);
    core::sound::layer1::Attach(kLayer1);
    g_attached = true;
    return true;
}

void OnLoop()
{
    if (!g_attached) {
        return;
    }
    core::sound::OnLoop(); // also the nature sounds (core/sound/layer1)
}

} // namespace emperor::sound
