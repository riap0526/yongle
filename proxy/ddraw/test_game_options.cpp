// Offline test of core/options/game_options (no DLL, no game): the game's own options as rows of the options window
// on a fake game -- the rows' kinds, ranges, defaults and texts; the speeds clamped to 10..100 in tens; autosave set
// only when it differs; the message kinds' "2 - value", their names, a fixed kind left out; the sound switches and
// volumes; Restore defaults and Apply writing only what changed; ReapplyVolumes on the streamed channels only; no
// rows before Attach. Then a new player's speeds for each combination of a player file and the shipped speeds, and
// PlayerFileIn on a folder made below this exe's (test_game_options_tmp\, removed at the end). Last, the difficulty
// and auto defend on a fake game whose difficulty_step is __cdecl and reads the button parameter: shown only in a
// city and single player, one level at a time, the button parameter put back, the lowest difficulty following it
// down, Restore defaults, auto defend refused while enemies attack (a notice), and a fault in the step that switches
// both rows off while the lowest still follows the level reached.
#include "core/diag/guard.h"
#include "core/options/game_options.h"
#include "core/options/menu_text.h"
#include "core/options/window.h"
#include "core/platform/files.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace co = core::options;
namespace go = core::options::game_options;

// Before core/ui/notice is installed, its texts go to our caption line: recorded here.
std::string g_caption;
namespace core::output {
void SetNotice(const char *utf8, unsigned)
{
    g_caption = utf8;
}
} // namespace core::output

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-72s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

// The fake game.
struct Fake {
    int speed[2] = {90, 60};
    bool autosave = true;
    int autosaveCalls = 0;
    int kinds = 5;
    int messages[20] = {2, 1, 0, 2, 2};
    int defaults[20] = {2, 2, 1, 2, 0};
    bool on[4] = {true, true, true, false};
    int volume[4] = {50, 100, 80, 80};
    int setVolumeCalls[4] = {};
    int writes = 0; // every set of any value
    bool player = false;
} g;

int Speed(int which) { return g.speed[which]; }
void SetSpeed(int which, int v) { g.speed[which] = v; ++g.writes; }
bool Autosave() { return g.autosave; }
void SetAutosave(bool on) { g.autosave = on; ++g.autosaveCalls; ++g.writes; }
int MessageKinds() { return g.kinds; }
std::string MessageName(int k) { return "Kind " + std::to_string(k); }
std::string ModeName(int m)
{
    const char *n[] = {"Off", "Condensed", "Full"};
    return n[m];
}
int Message(int k) { return g.messages[k]; }
void SetMessage(int k, int v) { g.messages[k] = v; ++g.writes; }
int MessageDefault(int k) { return g.defaults[k]; }
bool SoundOn(int i) { return g.on[i]; }
void SetSoundOn(int i, bool on) { g.on[i] = on; ++g.writes; }
int Volume(int i) { return g.volume[i]; }
void SetVolume(int i, int v) { g.volume[i] = v; ++g.setVolumeCalls[i]; ++g.writes; }

const go::Channel kChannels[go::kChannelCount] = {
    {"music", 50, true}, {"speech", 100, true}, {"sound_effects", 80, false}, {"ambient_volume", 80, true}};

const go::GameOptions kGame(&Speed, &SetSpeed, &Autosave, &SetAutosave, &MessageKinds, &MessageName, &ModeName,
                            &Message, &SetMessage, &MessageDefault, -1, kChannels, &SoundOn, &SetSoundOn, &Volume,
                            &SetVolume);
const go::GameOptions kGameFixed(&Speed, &SetSpeed, &Autosave, &SetAutosave, &MessageKinds, &MessageName, &ModeName,
                                 &Message, &SetMessage, &MessageDefault, 3, kChannels, &SoundOn, &SetSoundOn, &Volume,
                                 &SetVolume);

int Get(const co::Value &v) { return v.get(v.ctx); }
void Set(const co::Value &v, int x) { v.set(v.ctx, x); }

void TestRows()
{
    std::vector<co::Row> rows;
    go::SpeedRows(rows);
    go::AutosaveRows(rows);
    go::MessageRows(rows);
    go::SoundRows(rows);
    Check(rows.empty() && !go::Attached(), "before Attach: no rows");

    go::Attach(kGame);
    Check(go::Attached(), "attached");

    // Speeds.
    rows.clear();
    go::SpeedRows(rows);
    Check(rows.size() == 2, "two speed rows");
    Check(rows[0].kind == co::Kind::Slider && rows[0].id == "game_speed" && rows[0].label == "Game speed",
          "game speed: slider, id and label");
    Check(rows[1].id == "scroll_speed" && rows[1].label == "Scroll speed" && !rows[1].help.empty(),
          "scroll speed: id, label, description");
    Check(rows[0].min == 10 && rows[0].max == 100 && rows[0].step == 10 && rows[0].unit == "%" &&
              rows[1].min == 10 && rows[1].max == 100 && rows[1].step == 10,
          "speeds: 10..100 in tens, %");
    Check(rows[0].defaultValue == 80 && rows[1].defaultValue == 50, "speeds' defaults 80 / 50");
    Check(Get(rows[0].value) == 90 && Get(rows[1].value) == 60, "speeds read the game's");
    Set(rows[0].value, 37);
    Check(g.speed[0] == 30, "a game speed of 37 is written as 30");
    Set(rows[0].value, 5);
    Check(g.speed[0] == 10, "below 10: 10");
    Set(rows[1].value, 140);
    Check(g.speed[1] == 100, "above 100: 100");
    g.speed[0] = 90;
    g.speed[1] = 60;
    rows.clear();
    go::SpeedRows(rows, false);
    Check(rows.size() == 1 && rows[0].id == "scroll_speed", "without the game speed: only the scroll speed");

    // Autosave.
    rows.clear();
    go::AutosaveRows(rows);
    Check(rows.size() == 1 && rows[0].kind == co::Kind::Check && rows[0].label == "Autosave" &&
              rows[0].defaultValue == 1,
          "autosave: one checkbox, default on");
    Set(rows[0].value, 1);
    Check(g.autosaveCalls == 0, "autosave set to what it is: the game's switch not called");
    Set(rows[0].value, 0);
    Check(g.autosaveCalls == 1 && !g.autosave && Get(rows[0].value) == 0, "autosave off: switched once");
    Set(rows[0].value, 1);
    Check(g.autosaveCalls == 2 && g.autosave, "autosave on again");

    // Messages.
    rows.clear();
    go::MessageRows(rows);
    Check(rows.size() == 5, "a row per message kind");
    Check(rows[0].kind == co::Kind::Choice && rows[0].label == "Kind 0" && rows[4].label == "Kind 4",
          "message rows: choices named by the game");
    Check(rows[0].choices.size() == 3 && rows[0].choices[0] == "Full" && rows[0].choices[2] == "Off",
          "choices Full, Condensed, Off");
    Check(rows[0].choiceHelp.size() == 3 && !rows[0].choiceHelp[0].empty() && rows[0].help.empty(),
          "each choice has a tooltip; the name has none");
    Check(Get(rows[0].value) == 0 && Get(rows[1].value) == 1 && Get(rows[2].value) == 2,
          "shown as 2 - the game's value");
    Check(rows[0].defaultValue == 0 && rows[2].defaultValue == 1 && rows[4].defaultValue == 2,
          "defaults as 2 - the game's default");
    Set(rows[0].value, 2);
    Check(g.messages[0] == 0, "Off written as the game's 0");
    Set(rows[3].value, 1);
    Check(g.messages[3] == 1, "Condensed written as the game's 1");
    g.messages[1] = 7;
    Check(Get(rows[1].value) == 0, "a value past Full is shown as Full");

    go::Attach(kGameFixed);
    rows.clear();
    go::MessageRows(rows);
    bool fixedShown = false;
    for (const co::Row &r : rows) {
        fixedShown = fixedShown || r.label == "Kind 3";
    }
    Check(rows.size() == 4 && !fixedShown && rows[3].label == "Kind 4", "the fixed kind has no row");
    Set(rows[3].value, 2);
    Check(g.messages[4] == 0, "the row after the fixed kind writes its own kind");
    go::Attach(kGame);
    g.kinds = 0;
    rows.clear();
    go::MessageRows(rows);
    Check(rows.empty(), "no kinds: no rows");
    g.kinds = 5;

    // Sound.
    rows.clear();
    go::SoundRows(rows);
    Check(rows.size() == 4, "four sound rows");
    Check(rows[0].kind == co::Kind::CheckSlider && rows[0].label == "Music" && rows[2].label == "General sounds" &&
              rows[3].label == "Ambient sounds",
          "sound rows: check + slider, named");
    Check(rows[0].min == 0 && rows[0].max == 100 && rows[0].step == 5 && rows[1].defaultValue == 100 &&
              rows[0].defaultToggle == 1,
          "volumes 0..100 in fives; defaults the game's; on by default");
    Check(Get(rows[3].toggle) == 0 && Get(rows[0].toggle) == 1 && Get(rows[1].value) == 100,
          "switches and volumes read the game's");
    Set(rows[1].value, 120);
    Check(g.volume[1] == 100, "a volume above 100: 100");
    Set(rows[1].value, -3);
    Check(g.volume[1] == 0, "a volume below 0: 0");
    Set(rows[3].toggle, 1);
    Check(g.on[3], "a switch on");

    // ReapplyVolumes: the streamed channels, each once, with their own volume.
    std::memset(g.setVolumeCalls, 0, sizeof(g.setVolumeCalls));
    g.volume[0] = 45;
    go::ReapplyVolumes();
    Check(g.setVolumeCalls[0] == 1 && g.setVolumeCalls[1] == 1 && g.setVolumeCalls[2] == 0 &&
              g.setVolumeCalls[3] == 1 && g.volume[0] == 45,
          "ReapplyVolumes: music, speech, ambient once each; general sounds not");
}

// Restore defaults and Apply in the window: only the rows whose value changes are written.
void TestWindow()
{
    g = Fake{};
    g.speed[0] = 80; // already the default
    g.speed[1] = 60;
    co::Page page{"General", {{"Speed", {}}}};
    go::SpeedRows(page.categories[0].rows);
    go::AutosaveRows(page.categories[0].rows);
    co::Window w;
    w.SetPages({page});
    w.Open(0, 0);
    w.RestoreDefaults();
    g.writes = 0;
    w.Apply();
    Check(g.speed[0] == 80 && g.speed[1] == 50, "Restore defaults + Apply: 80 / 50");
    Check(g.writes == 1 && g.autosaveCalls == 0, "Apply wrote only the scroll speed (the rest was at its default)");
}

std::vector<std::pair<int, int>> g_written;
int NpSpeed(int which) { return g.speed[which]; }
void NpWrite(int game, int scroll)
{
    g_written.push_back({game, scroll});
    g.speed[0] = game;
    g.speed[1] = scroll;
}
bool NpPlayer() { return g.player; }

void TestNewPlayer()
{
    const int shipped[][2] = {{90, 60}, {60, 10}};
    const go::NewPlayer n(shipped, 2, &NpPlayer, &NpSpeed, &NpWrite);
    struct Case {
        bool player;
        int game, scroll;
        bool written;
        const char *what;
    } cases[] = {
        {false, 90, 60, true, "new player, the game's defaults 90 / 60: 80 / 50"},
        {false, 60, 10, true, "new player, the install's 60 / 10: 80 / 50"},
        {false, 70, 60, false, "no player file but other speeds: kept"},
        {false, 90, 10, false, "no player file, one speed shipped, the other not: kept"},
        {true, 90, 60, false, "a player file and the defaults: kept"},
        {true, 30, 20, false, "a player file and other speeds: kept"},
    };
    for (const Case &c : cases) {
        g.player = c.player;
        g.speed[0] = c.game;
        g.speed[1] = c.scroll;
        g_written.clear();
        const bool r = go::CheckNewPlayer(n);
        const bool ok = r == c.written &&
                        (c.written ? g_written.size() == 1 && g.speed[0] == 80 && g.speed[1] == 50
                                   : g_written.empty() && g.speed[0] == c.game && g.speed[1] == c.scroll);
        Check(ok, c.what);
    }
}

void TestPlayerFile()
{
    const std::wstring root = std::wstring(core_files_module_folder()) + L"test_game_options_tmp\\";
    const std::wstring save = root + L"Save\\";
    CreateDirectoryW(root.c_str(), nullptr);
    Check(!go::PlayerFileIn(root), "no Save folder: no player");
    CreateDirectoryW(save.c_str(), nullptr);
    Check(!go::PlayerFileIn(root), "an empty Save folder: no player");
    CreateDirectoryW((save + L"Folder.dat").c_str(), nullptr);
    HANDLE f = CreateFileW((save + L"dummy.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CloseHandle(f);
    Check(!go::PlayerFileIn(root), "a folder named *.dat and another file: no player");
    f = CreateFileW((save + L"Someone.dat").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CloseHandle(f);
    Check(go::PlayerFileIn(root), "Save\\Someone.dat: a player");
    DeleteFileW((save + L"Someone.dat").c_str());
    DeleteFileW((save + L"dummy.txt").c_str());
    RemoveDirectoryW((save + L"Folder.dat").c_str());
    RemoveDirectoryW(save.c_str());
    RemoveDirectoryW(root.c_str());
    Check(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES, "the test folder removed");
}

// --- Difficulty and auto defend.
struct FakeDifficulty {
    int level = 1, lowest = 1, param = 77;
    std::vector<int> steps; // the button parameter seen by each call of the step
    int faultAt = -1;       // the step faults on this call (0-based); -1 never
    bool defend = false, attack = false, city = true, single = true;
    int toggles = 0;
} d;

void __cdecl FakeStep() // as the games' difficulty_step: reads the button parameter, clamps 0..4
{
    if (static_cast<int>(d.steps.size()) == d.faultAt) {
        *static_cast<volatile int *>(nullptr) = 1;
    }
    d.steps.push_back(d.param);
    d.level = std::max(0, std::min(4, d.level + (d.param == 0 ? 1 : -1)));
}
int DLevel() { return d.level; }
int DLowest() { return d.lowest; }
void DSetLowest(int v) { d.lowest = v; }
int DParam() { return d.param; }
void DSetParam(int v) { d.param = v; }
std::string DName(int k) { return "Level " + std::to_string(k); }
bool DDefend() { return d.defend; }
void DToggle() { d.defend = !d.defend; ++d.toggles; }
bool DAttack() { return d.attack; }
bool DCity() { return d.city; }
bool DSingle() { return d.single; }

const go::Difficulty kDiff(&DLevel, &DLowest, &DSetLowest, &DParam, &DSetParam, &FakeStep, &DName, 2, &DDefend,
                           &DToggle, &DAttack, &DCity, &DSingle);

void TestDifficulty()
{
    std::vector<co::Row> rows;
    go::DifficultyRows(rows);
    Check(rows.empty() && !go::DifficultyAttached() && !go::DifficultyShown() && !go::ToggleAutoDefend(),
          "difficulty: before AttachDifficulty: no rows, no toggle");
    go::AttachDifficulty(kDiff);
    Check(go::DifficultyAttached() && go::DifficultyShown(), "difficulty: attached, shown in a city");
    d.city = false;
    go::DifficultyRows(rows);
    Check(rows.empty() && !go::DifficultyShown(), "difficulty: not in a city: no rows");
    d.city = true;
    d.single = false;
    go::DifficultyRows(rows);
    Check(rows.empty(), "difficulty: not single player: no rows");
    d.single = true;
    go::DifficultyRows(rows);
    Check(rows.size() == 2, "difficulty: two rows in a city, single player");
    const co::Row level = rows[0], defend = rows[1];
    Check(level.kind == co::Kind::Choice && level.id == "difficulty" && level.label == "Difficulty" &&
              level.choices.size() == 5 && level.choices[0] == "Level 0" && level.choices[4] == "Level 4" &&
              level.defaultValue == 2,
          "difficulty: a choice of the game's five levels, the game's default");
    Check(level.help.find("result") != std::string::npos && level.help.find("lowest") != std::string::npos,
          "difficulty: its description says the mission's result counts the lowest");
    Check(defend.kind == co::Kind::Check && defend.id == "auto_defend" && defend.label == "Auto defend" &&
              defend.defaultValue == 0 && !defend.help.empty(),
          "auto defend: a checkbox, off by default, described");
    Check(Get(level.value) == 1 && Get(defend.value) == 0, "difficulty: the game's values read");

    // Up three levels: three steps with parameter 0, the parameter put back, the lowest stays.
    Set(level.value, 4);
    Check(d.level == 4 && d.steps.size() == 3 && d.steps[0] == 0 && d.steps[2] == 0 && d.param == 77 &&
              d.lowest == 1,
          "difficulty: 1 -> 4: three steps up, button parameter back, lowest stays 1");
    // Down four: four steps with parameter 1, the lowest follows down.
    d.steps.clear();
    Set(level.value, 0);
    Check(d.level == 0 && d.steps.size() == 4 && d.steps[3] == 1 && d.param == 77 && d.lowest == 0,
          "difficulty: 4 -> 0: four steps down, the lowest now 0");
    d.steps.clear();
    Set(level.value, 3);
    Check(d.level == 3 && d.lowest == 0 && d.steps.size() == 3, "difficulty: up again: the lowest stays 0");
    d.steps.clear();
    Set(level.value, 3);
    Check(d.steps.empty(), "difficulty: the same level: no step");
    d.steps.clear();
    Set(level.value, 9);
    Check(d.level == 4 && d.steps.size() == 1, "difficulty: past the last level: to the last");

    // Restore defaults in the window: the game's default (2), Apply walks there.
    {
        co::Page page{"General", {{"Difficulty", rows}}};
        co::Window w;
        w.SetPages({page});
        w.Open(0, 0);
        w.RestoreDefaults();
        d.steps.clear();
        w.Apply();
        Check(d.level == 2 && d.steps.size() == 2 && !d.defend && d.toggles == 0,
              "difficulty: Restore defaults + Apply: the game's default, auto defend not touched");
    }

    // Auto defend: switched when no enemies attack; refused while they do (a notice, nothing changes).
    Set(defend.value, 1);
    Check(d.defend && d.toggles == 1, "auto defend: on");
    Set(defend.value, 1);
    Check(d.toggles == 1, "auto defend: on again: not toggled");
    d.attack = true;
    g_caption.clear();
    Set(defend.value, 0);
    Check(d.defend && d.toggles == 1 && g_caption == co::menu_text::kAutoDefendLocked,
          "auto defend: while enemies attack: not switched off, the notice shown");
    Check(!go::ToggleAutoDefend() && d.defend, "auto defend: ... the menu's toggle refused too");
    d.defend = false;
    Check(!go::ToggleAutoDefend() && !d.defend, "auto defend: ... and not switched on");
    d.attack = false;
    Check(go::ToggleAutoDefend() && d.defend, "auto defend: the menu's toggle after the attack");

    // A fault in the game's step (the second of three): off, the button parameter back, the level reached kept and
    // the lowest following it.
    d.level = 4;
    d.lowest = 4;
    d.steps.clear();
    d.faultAt = 1;
    Set(level.value, 1);
    Check(d.level == 3 && d.lowest == 3 && d.param == 77, "difficulty: a fault: stays at 3, lowest 3, parameter back");
    rows.clear();
    go::DifficultyRows(rows);
    Check(rows.empty() && !go::DifficultyShown(), "difficulty: ... both rows off from then on");
    const int toggles = d.toggles;
    Check(!go::ToggleAutoDefend() && d.toggles == toggles, "difficulty: ... auto defend not changed either");
}

} // namespace

int main()
{
    core::diag::InstallExceptionLog(); // the guard's exception handling (the difficulty's fault)
    TestRows();
    TestWindow();
    TestNewPlayer();
    TestPlayerFile();
    TestDifficulty(); // last: its fault switches the rows off for the rest of the process

    go::DetachForTest();
    std::vector<co::Row> rows;
    go::SoundRows(rows);
    Check(rows.empty(), "after DetachForTest: no rows");
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
