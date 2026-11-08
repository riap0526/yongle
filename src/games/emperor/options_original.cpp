#include "games/emperor/options_original.h"

#include "core/log.h"
#include "games/emperor/options_text.h"
#include "games/emperor/site.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/resolution_ui/original_settings.md §12.1 and §14
// (S2c); calling conventions: docs/symbols.md.
namespace emperor::options_original {
namespace {

namespace co = core::options;
namespace ot = emperor::options_text;

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

using Volume2_fn = void(__thiscall *)(void *, int, int);
using Volume1_fn = void(__thiscall *)(void *, int);
using Void_fn = void(__cdecl *)();
using This_fn = void(__thiscall *)(void *);
using Lang_fn = const char *(__cdecl *)(int group, int index);

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

struct Channel {
    const char *key;   // options_text
    DWORD on;          // a byte: the switch
    DWORD volume;      // an int 0..100
    int stream;        // the sound manager's channel, -1: none (general sounds are read when they play)
    const Site *setter; // the channel's own volume method, nullptr: none
    int defaultVolume; // screen_select_initial_mode 0x4761a0
};
const Channel kChannels[] = {
    {"music", 0x10de0d4, 0x10de138, 0, &kMusicVolume, 50},
    {"speech", 0x10de0d5, 0x10de13c, 1, &kSpeechVolume, 100},
    {"sound_effects", 0x10de0d3, 0x10de134, -1, nullptr, 80},
    {"ambient_volume", 0x10de130, 0x10de140, 2, &kAmbientVolume, 80},
};

bool g_ok = false;

// Our starting speeds: 80% as Julius / Augustus (the walkers and animations at a natural pace);
// scroll 30%, near Augustus' 70% in feel, since smooth scrolling makes the game's 60% fast. The defaults of
// Restore defaults, and a new player's: screen_select_initial_mode 0x4761a0 writes its defaults before it reads
// Emperor.inf (0x52a7f0), so a player who has the file keeps their values; one who has none starts with
// ours. Each is a "mov dword ptr [variable], value" (C7 05, address, value); only the value is changed.
constexpr int kStartGameSpeed = 80, kStartScrollSpeed = 30;
struct StartValue {
    DWORD instruction;
    DWORD variable;
    int original, ours;
};
const StartValue kStartValues[] = {
    {0x476220, 0x10de0dc, 90, kStartGameSpeed},  // game_speed
    {0x47622a, 0x10de120, 90, kStartGameSpeed},  // the speed kept (game_speed_kept)
    {0x476234, 0x10de0e0, 60, kStartScrollSpeed}, // scroll_speed
    {0x47632d, 0x10de120, 90, kStartGameSpeed},  // the speed kept again, for an Emperor.inf older than 101 (or none)
};

void StartDefaults()
{
    for (const StartValue &s : kStartValues) {
        BYTE expect[10] = {0xc7, 0x05};
        std::memcpy(expect + 2, &s.variable, 4);
        std::memcpy(expect + 6, &s.original, 4);
        const void *at = reinterpret_cast<const void *>(s.instruction + g_imageShift);
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(at, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            std::memcmp(at, expect, sizeof(expect)) != 0) {
            core_log("options window: the game's start-up defaults at 0x%lx are not as expected; the game's own kept",
                     s.instruction);
            return; // all or none: every one is checked before any is changed
        }
    }
    for (const StartValue &s : kStartValues) {
        PatchCode(s.instruction + 6 + g_imageShift, reinterpret_cast<const BYTE *>(&s.ours), 4);
    }
    core_log("options window: a new player (no Emperor.inf) starts at game speed %d%%, scroll speed %d%%",
             kStartGameSpeed, kStartScrollSpeed);
}

// The rows' contexts: what << 8 | index.
enum What : intptr_t { kSpeed = 1, kAutosave, kMessage, kSoundOn, kSoundVolume };
const void *Ctx(What w, int i)
{
    return reinterpret_cast<const void *>(static_cast<intptr_t>(w) << 8 | i);
}
What WhatOf(const void *c)
{
    return static_cast<What>(reinterpret_cast<intptr_t>(c) >> 8);
}
int IndexOf(const void *c)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(c) & 0xff);
}

volatile BYTE &Byte(DWORD address)
{
    return *Var<BYTE>{address};
}
volatile int &Int(DWORD address)
{
    return *Var<int>{address};
}

int Get(const void *c)
{
    const int i = IndexOf(c);
    switch (WhatOf(c)) {
    case kSpeed: return i == 0 ? *kGameSpeed : *kScrollSpeed;
    case kAutosave: return *kAutosaveOff == 0;
    case kMessage: return 2 - std::clamp<int>(Byte(kMessages + i), 0, 2); // the row shows Full, Condensed, Off
    case kSoundOn: return Byte(kChannels[i].on) != 0;
    case kSoundVolume: return Int(kChannels[i].volume);
    }
    return 0;
}

void SetSpeed(int i, int v)
{
    v = std::clamp(v / 10 * 10, 10, 100);
    if (i == 1) {
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

void SetSoundOn(int i, bool on)
{
    const Channel &ch = kChannels[i];
    Byte(ch.on) = on ? 1 : 0;
    if (on || ch.stream < 0) { // the Sound options' switch 0x4021b0: switching on only sets it
        return;
    }
    Function<Volume1_fn>(kChannelStop)(reinterpret_cast<void *>(kSoundManager), ch.stream);
    if (ch.stream == 2 && *kAmbientLayer != 0) {
        Function<This_fn>(kAmbientStop)(reinterpret_cast<void *>(kAmbientSounds));
    }
}

void SetSoundVolume(int i, int v)
{
    const Channel &ch = kChannels[i];
    v = std::clamp(v, 0, 100);
    Int(ch.volume) = v;
    if (ch.stream >= 0) { // as the Sound options' Cancel 0x4020c0 hands the volumes on
        Function<Volume2_fn>(kChannelVolume)(reinterpret_cast<void *>(kSoundManager), ch.stream, v);
        Function<Volume1_fn>(*ch.setter)(reinterpret_cast<void *>(kSoundManager), v);
    }
}

void Set(const void *c, int v)
{
    const int i = IndexOf(c);
    switch (WhatOf(c)) {
    case kSpeed: SetSpeed(i, v); break;
    case kAutosave:
        if ((*kAutosaveOff == 0) != (v != 0)) {
            Function<Void_fn>(kAutosaveToggle)();
        }
        break;
    case kMessage: Byte(kMessages + i) = static_cast<BYTE>(2 - std::clamp(v, 0, 2)); break;
    case kSoundOn: SetSoundOn(i, v != 0); break;
    case kSoundVolume: SetSoundVolume(i, v); break;
    }
    static const char *const kNames[] = {"", "speed", "autosave", "message kind", "sound switch", "sound volume"};
    const int w = static_cast<int>(WhatOf(c));
    core_log("options window: the game's %s %d = %d", w > 0 && w < 6 ? kNames[w] : "?", i,
             w == kMessage ? 2 - std::clamp(v, 0, 2) : v); // messages: the game's value (0 Off, 1 Condensed, 2 Full)
}

co::Value V(What w, int i)
{
    return {&Get, &Set, Ctx(w, i)};
}

std::string Lang(int group, int index)
{
    const char *s = Function<Lang_fn>(kLangString)(group, index);
    return s ? s : "";
}

// Label and description from options_text ("game", key).
void Texts(co::Row &r, const char *key)
{
    const ot::Text *t = ot::Find("game", key);
    if (t) {
        r.label = t->label ? t->label : key;
        r.help = t->summary ? t->summary : "";
    }
}

} // namespace

bool Attach()
{
    const Site *sites[] = {&kChannelVolume, &kMusicVolume, &kSpeechVolume, &kAmbientVolume,
                           &kChannelStop,   &kAmbientStop, &kAutosaveToggle, &kLangString};
    for (const Site *s : sites) {
        if (!Matches(*s)) {
            core_log("options window: %s at 0x%lx does not match; the game's own options are left out", s->name,
                     s->address);
            return false;
        }
    }
    g_ok = true;
    StartDefaults();
    return true;
}

void ReapplyVolumes()
{
    if (!g_ok) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        if (kChannels[i].stream >= 0) {
            SetSoundVolume(i, Int(kChannels[i].volume));
        }
    }
}

void SpeedRows(std::vector<co::Row> &rows)
{
    if (!g_ok) {
        return;
    }
    const char *keys[] = {"game_speed", "scroll_speed"};
    const int defaults[] = {kStartGameSpeed, kStartScrollSpeed}; // ours, also a new player's (StartDefaults)
    for (int i = 0; i < 2; ++i) {
        co::Row r;
        r.kind = co::Kind::Slider;
        r.id = keys[i];
        Texts(r, keys[i]);
        r.value = V(kSpeed, i);
        r.min = 10;
        r.max = 100;
        r.step = 10;
        r.unit = "%";
        r.defaultValue = defaults[i];
        rows.push_back(std::move(r));
    }
}

void AutosaveRows(std::vector<co::Row> &rows)
{
    if (!g_ok) {
        return;
    }
    co::Row r;
    r.kind = co::Kind::Check;
    r.id = "autosave";
    Texts(r, "autosave");
    r.value = V(kAutosave, 0);
    r.defaultValue = 1; // 0x10de15c starts at 0 (on) when the game has no Emperor.inf
    rows.push_back(std::move(r));
}

void MessageRows(std::vector<co::Row> &rows)
{
    if (!g_ok) {
        return;
    }
    const int kinds = std::min(static_cast<int>(*kMessageKinds), 14) - 1; // as 0x5bb520(0)
    // The names say enough (no tooltip on them); the values have one each. Shown Full, Condensed, Off (the game's
    // order reversed, so the arrows go from the most to the least): choice k is the game's value 2 - k.
    const ot::Text *t = ot::Find("game", "messages");
    const char *spells[] = {"full", "condensed", "off"};
    for (int i = 0; i < kinds; ++i) {
        co::Row r;
        r.kind = co::Kind::Choice;
        r.label = Lang(kTextMessageKinds, i);
        for (int k = 0; k < 3; ++k) {
            r.choices.push_back(Lang(kTextMessageModes, 2 - k));
            r.choiceHelp.push_back(t ? ot::ChoiceHelp(*t, spells[k]) : "");
        }
        r.value = V(kMessage, i);
        r.defaultValue = 2 - std::clamp<int>(Byte(kMessageDefaults + i), 0, 2);
        rows.push_back(std::move(r));
    }
}

void SoundRows(std::vector<co::Row> &rows)
{
    if (!g_ok) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        co::Row r;
        r.kind = co::Kind::CheckSlider;
        r.id = kChannels[i].key;
        Texts(r, kChannels[i].key);
        r.toggle = V(kSoundOn, i);
        r.value = V(kSoundVolume, i);
        r.min = 0;
        r.max = 100;
        r.step = 5; // the Sound options' arrows
        r.unit = "%";
        r.defaultValue = kChannels[i].defaultVolume;
        r.defaultToggle = 1;
        rows.push_back(std::move(r));
    }
}

} // namespace emperor::options_original
