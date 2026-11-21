#include "games/emperor/options_original.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/options/game_options.h"
#include "core/platform/files.h"
#include "games/emperor/resolution.h"
#include "games/emperor/site.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): the game's own options dialogs read from the disassembly;
// calling conventions checked by disassembly.
namespace emperor::options_original {
namespace {

namespace go = core::options::game_options;

// The sound manager's methods (__thiscall, ecx = sound_manager 0x1156bc8): channel 0 music, 1 speech, 2 ambient.
const Site kChannelVolume{"sound_channel_volume_set", 0x762eec, {0x55, 0x8b, 0xec, 0x51, 0x89, 0x4d, 0xfc, 0x8b}};
const Site kMusicVolume{"sound_music_volume_set", 0x762c0f, {0x55, 0x8b, 0xec, 0x51, 0x89, 0x4d, 0xfc, 0x8b}};
const Site kSpeechVolume{"sound_speech_volume_set", 0x762bb1, {0x55, 0x8b, 0xec, 0x51, 0x89, 0x4d, 0xfc, 0x8b}};
const Site kAmbientVolume{"sound_ambient_volume_set", 0x762c6d, {0x55, 0x8b, 0xec, 0x51, 0x89, 0x4d, 0xfc, 0x8b}};
const Site kChannelStop{"sound_channel_stop", 0x762de6, {0x55, 0x8b, 0xec, 0x51, 0x89, 0x4d, 0xfc, 0x8b}};
// ambient_layer1_stop 0x5c2f50 (__thiscall, ecx = the ambient sounds 0x1103218; the Sound options' switch 0x402264).
const Site kAmbientStop{"ambient_layer1_stop", 0x5c2f50, {0x53, 0x8b, 0xd9, 0x55, 0x57, 0x8b, 0x83, 0x10}};
// autosave_toggle 0x449460 (__cdecl, no arguments): flips 0x10de15c and the menu item's text.
const Site kAutosaveToggle{"autosave_toggle", 0x449460, {0xa1, 0x5c, 0xe1, 0x0d, 0x01, 0x85, 0xc0, 0x74}};
// lang_get_string(group, index) 0x5278b0 (__cdecl; ret).
const Site kLangString{"lang_get_string", 0x5278b0, {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x15, 0xcc, 0xd8}};
// screen_select_initial_mode 0x4761a0 (__cdecl, no arguments): writes the options' defaults, then reads Emperor.inf
// (0x52a7f0). Hooked by games/emperor/resolution when it is installed (it then calls us after the original); hooked
// here otherwise.
const Site kInitialMode{"screen_select_initial_mode", 0x4761a0, {0x53, 0x56, 0xbb, 0x01, 0x00, 0x00, 0x00, 0xbe}};

using Volume2_fn = void(__thiscall *)(void *, int, int);
using Volume1_fn = void(__thiscall *)(void *, int);
using Void_fn = void(__cdecl *)();
using This_fn = void(__thiscall *)(void *);
using Lang_fn = const char *(__cdecl *)(int group, int index);
using InitialMode_fn = int(__cdecl *)();

const DWORD kSoundManager = 0x1156bc8, kAmbientSounds = 0x1103218;

const Var<int> kGameSpeed{0x10de0dc};      // 10..100 in steps of 10 (game_speed_step 0x4024d0)
const Var<int> kScrollSpeed{0x10de0e0};    // 10..100 in steps of 10 (0x402560)
const Var<int> kRememberedSpeed{0x10de120}; // the speed the game goes back to (game_speed_step writes it too)
const Var<int> kSpeedLocked{0x88ec38};      // 1: game_speed_step and the autosave do nothing
const DWORD kGameState = 0x10bc348;         // 0x408ac0 returns it; +0 > 4 and +0x10 != 0: the speed is not kept
const Var<int> kAutosaveOff{0x10de15c};     // not 0: no autosave (0x535770 returns at 0x5357a4)
const Var<int> kMessageKinds{0x10dbe1c};    // the kinds of messages (0x5bb520 shows at most 14, less one)
const DWORD kMessages = 0x10de164;          // one byte per kind: 0 Off, 1 Condensed, 2 Full
const DWORD kMessageDefaults = 0xc7f5bc;    // the kinds' defaults from the message categories file (0x4762f6)
const Var<int> kAmbientLayer{0x10de3f0};    // not 0: the first ambient layer plays (stopped with the switch)
constexpr int kTextMessageKinds = 176, kTextMessageModes = 183; // EmperorText.eng groups

// Per channel: the switch (a byte), the volume (an int 0..100), the sound manager's stream (-1: none, general sounds
// are read when they play) and its own volume method.
struct ChannelSite {
    DWORD on, volume;
    int stream;
    const Site *setter;
};
const ChannelSite kChannelSites[go::kChannelCount] = {
    {0x10de0d4, 0x10de138, 0, &kMusicVolume},
    {0x10de0d5, 0x10de13c, 1, &kSpeechVolume},
    {0x10de0d3, 0x10de134, -1, nullptr},
    {0x10de130, 0x10de140, 2, &kAmbientVolume},
};
// Defaults: screen_select_initial_mode 0x4761a0.
const go::Channel kChannels[go::kChannelCount] = {
    {"music", 50, true},
    {"speech", 100, true},
    {"sound_effects", 80, false},
    {"ambient_volume", 80, true},
};

volatile BYTE &Byte(DWORD address)
{
    return *Var<BYTE>{address};
}
volatile int &Int(DWORD address)
{
    return *Var<int>{address};
}

std::string Lang(int group, int index)
{
    const char *s = Function<Lang_fn>(kLangString)(group, index);
    return s ? s : "";
}

int Speed(int which)
{
    return which == 0 ? *kGameSpeed : *kScrollSpeed;
}

void SetSpeed(int which, int v)
{
    if (which == 1) {
        *kScrollSpeed = v;
        return;
    }
    if (*kSpeedLocked == 1) { // as game_speed_step 0x4024d0
        core_log("options window: the game speed cannot be changed now");
        return;
    }
    *kGameSpeed = v;
    const volatile int *state = Var<int>{kGameState}.ptr();
    if (!(state[0] > 4 && state[4] != 0)) { // game_speed_step 0x402522..0x402548
        *kRememberedSpeed = v;
    }
}

bool Autosave()
{
    return *kAutosaveOff == 0;
}

void SetAutosave(bool)
{
    Function<Void_fn>(kAutosaveToggle)(); // the core calls it only when the switch differs
}

int MessageKinds()
{
    return std::min(static_cast<int>(*kMessageKinds), 14) - 1; // as 0x5bb520(0)
}

std::string MessageName(int kind)
{
    return Lang(kTextMessageKinds, kind);
}

std::string MessageModeName(int mode)
{
    return Lang(kTextMessageModes, mode);
}

int Message(int kind)
{
    return Byte(kMessages + kind);
}

void SetMessage(int kind, int value)
{
    Byte(kMessages + kind) = static_cast<BYTE>(value);
}

int MessageDefault(int kind)
{
    return Byte(kMessageDefaults + kind);
}

bool SoundOn(int i)
{
    return Byte(kChannelSites[i].on) != 0;
}

void SetSoundOn(int i, bool on)
{
    const ChannelSite &ch = kChannelSites[i];
    Byte(ch.on) = on ? 1 : 0;
    if (on || ch.stream < 0) { // the Sound options' switch 0x4021b0: switching on only sets it
        return;
    }
    Function<Volume1_fn>(kChannelStop)(reinterpret_cast<void *>(kSoundManager), ch.stream);
    if (ch.stream == 2 && *kAmbientLayer != 0) {
        Function<This_fn>(kAmbientStop)(reinterpret_cast<void *>(kAmbientSounds));
    }
}

int Volume(int i)
{
    return Int(kChannelSites[i].volume);
}

void SetVolume(int i, int v)
{
    const ChannelSite &ch = kChannelSites[i];
    Int(ch.volume) = v;
    if (ch.stream >= 0) { // as the Sound options' Cancel 0x4020c0 hands the volumes on
        Function<Volume2_fn>(kChannelVolume)(reinterpret_cast<void *>(kSoundManager), ch.stream, v);
        Function<Volume1_fn>(*ch.setter)(reinterpret_cast<void *>(kSoundManager), v);
    }
}

const go::GameOptions kGame(&Speed, &SetSpeed, &Autosave, &SetAutosave, &MessageKinds, &MessageName,
                            &MessageModeName, &Message, &SetMessage, &MessageDefault,
                            -1, // every kind Emperor's Messages dialog shows can be changed
                            kChannels, &SoundOn, &SetSoundOn, &Volume, &SetVolume);

// A new player: the GOG and Steam installs bring an Emperor.inf with 60 / 10; the game's own defaults (no Emperor.inf)
// are 90 / 60.
const int kShipped[][2] = {{90, 60}, {60, 10}};

bool HasPlayer()
{
    return go::PlayerFileIn(core_files_module_folder());
}

// Emperor.inf has been read: the game speed, the speed it goes back to, and the scroll speed.
void WriteSpeeds(int game, int scroll)
{
    *kGameSpeed = game;
    *kRememberedSpeed = game;
    *kScrollSpeed = scroll;
}

const go::NewPlayer kNewPlayer(kShipped, 2, &HasPlayer, &Speed, &WriteSpeeds);

// --- Difficulty and auto defend (core/options/game_options Difficulty). The game has the code for both but no way in:
// its difficulty dialog (screen 0x45) is never opened, and nothing writes auto_combat (the game only reads it, from
// Emperor.inf and the saved games, and sends the army out by itself when it is on).
// difficulty_step 0x401f80 (__cdecl, no arguments; ret): button_param 0 one level up, else one down, clamped to 0..4,
// then the game's data for the level read again and screen_redraw set (on the mission briefing, screen 0x35, it also
// resets the starting money: the rows are never there).
const Site kDifficultyStep{"difficulty_step", 0x401f80, {0xa1, 0x60, 0x6f, 0x0c, 0x01, 0x85, 0xc0, 0xa1}};

const Var<int> kDifficulty{0x10de2e0};       // 0..4; Emperor.inf and the saved games keep it
const Var<int> kDifficultyLowest{0x1312620}; // the lowest difficulty of this mission (its result reads it)
const Var<int> kButtonParam{0x10c6f60};      // the pressed button's parameter (difficulty_step reads it)
const Var<BYTE> kAutoCombat{0x10de2e8};      // 0 / 1 (the army panel's "Auto Combat On")
const Var<int> kInvasionA{0x1312564}, kInvasionB{0x1312780}; // not 0: enemies attacking (as Zeus's auto defend test)
const Var<char> kGameScreen{0x10de045};
const Var<int> kEditorMode{0x88ec38};   // 1: the map editor
const Var<char> kScreenRedraw{0x10de036};
constexpr int kTextDifficulty = 153;    // EmperorText.eng: 1..5 Very Easy .. Very Hard

int Difficulty()
{
    return *kDifficulty;
}
int DifficultyLowest()
{
    return *kDifficultyLowest;
}
void SetDifficultyLowest(int v)
{
    *kDifficultyLowest = v;
}
int ButtonParam()
{
    return *kButtonParam;
}
void SetButtonParam(int v)
{
    *kButtonParam = v;
}
void __cdecl DifficultyStep()
{
    Function<Void_fn>(kDifficultyStep)();
}
std::string LevelName(int level)
{
    return Lang(kTextDifficulty, level + 1);
}
bool AutoCombat()
{
    return *kAutoCombat != 0;
}
// As Zeus's auto_defend_toggle 0x5692d0 (the core calls it only with no enemies attacking); the army panel drawn
// again with or without its "Auto Combat On". Only 0 and 1, the values the game itself reads.
void ToggleAutoCombat()
{
    *kAutoCombat = *kAutoCombat != 0 ? 0 : 1;
    *kScreenRedraw = 1;
}
bool UnderAttack()
{
    return *kInvasionA != 0 || *kInvasionB != 0;
}
// The city being played: the city screen, or with a company (0x48) or a god (0x49) selected -- the screens where Esc
// opens the pause menu (games/emperor/game_menu); the Options menu puts the city screen back before calling its items.
bool InCity()
{
    const int s = static_cast<unsigned char>(*kGameScreen);
    return *kEditorMode != 1 && (s == 1 || s == 0x48 || s == 0x49);
}
// One player: menu_bar_select 0x449590 picks the single-player menu bars (their Options menu has Autosave) when the
// game state's +0x10 is 0 (0x4fae50); the other four bars are the multiplayer ones.
bool SinglePlayer()
{
    return Var<int>{kGameState}.ptr()[4] == 0;
}

const go::Difficulty kDifficultyRows(&Difficulty, &DifficultyLowest, &SetDifficultyLowest, &ButtonParam,
                                     &SetButtonParam, &DifficultyStep, &LevelName,
                                     2, // Normal: screen_select_initial_mode's default
                                     &AutoCombat, &ToggleAutoCombat, &UnderAttack, &InCity, &SinglePlayer);

core::diag::Feature g_newPlayer{"new player's speeds (games/emperor/options_original)"};
bool g_langOk = false; // lang_get_string as expected (Attach)

InitialMode_fn o_InitialMode;

int __cdecl InitialMode_detour()
{
    const int r = Original(o_InitialMode, kInitialMode)();
    AfterInfRead();
    return r;
}

} // namespace

void AfterInfRead()
{
    static bool done = false;
    if (done || !core::diag::Enabled(g_newPlayer)) {
        return;
    }
    done = true;
    core::diag::Guarded(g_newPlayer, [](void *) { go::CheckNewPlayer(kNewPlayer); }, nullptr);
}

bool Attach()
{
    // A new player's speeds, once Emperor.inf has been read (whether or not the rows can be).
    if (emperor::resolution::InitialModeHooked()) {
        emperor::resolution::SetAfterInitialMode(&AfterInfRead);
    }
    else if (Matches(kInitialMode)) {
        Install(kInitialMode, &InitialMode_detour, o_InitialMode);
    }
    else {
        core_log("options window: %s at 0x%lx does not match: a new player starts at the game's own speeds",
                 kInitialMode.name, kInitialMode.address);
    }
    // Checked before options_entry hooks it (its detour passes the game's texts on).
    g_langOk = Matches(kLangString);
    const Site *sites[] = {&kChannelVolume, &kMusicVolume, &kSpeechVolume, &kAmbientVolume,
                           &kChannelStop,   &kAmbientStop, &kAutosaveToggle, &kLangString};
    for (const Site *s : sites) {
        if (!Matches(*s)) {
            core_log("options window: %s at 0x%lx does not match; the game's own options are left out", s->name,
                     s->address);
            return false;
        }
    }
    go::Attach(kGame);
    if (Matches(kDifficultyStep)) {
        go::AttachDifficulty(kDifficultyRows);
        core_log("options window: the game's difficulty and auto combat found");
    }
    else {
        core_log("options window: %s at 0x%lx does not match; no Difficulty and Auto defend", kDifficultyStep.name,
                 kDifficultyStep.address);
    }
    return true;
}

void AttachForTest()
{
    g_langOk = true; // the test's lang_get_string is a fake at the game's address
    go::Attach(kGame);
    go::AttachDifficulty(kDifficultyRows);
}

void ReapplyVolumes()
{
    go::ReapplyVolumes();
}

std::string GameText(int group, int index)
{
    if (!g_langOk) {
        return "";
    }
    return Lang(group, index);
}

} // namespace emperor::options_original
