#include "core/options/game_options.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/options/menu_text.h"
#include "core/options/option_text.h"
#include "core/ui/notice.h"

#include <algorithm>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace core::options::game_options {
namespace {

namespace ot = core::options::option_text;

const GameOptions *g_game = nullptr;

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

int Get(const void *c)
{
    const GameOptions &g = *g_game;
    const int i = IndexOf(c);
    switch (WhatOf(c)) {
    case kSpeed: return g.speed(i);
    case kAutosave: return g.autosave() ? 1 : 0;
    case kMessage: return 2 - std::clamp(g.message(i), 0, 2); // the row shows Full, Condensed, Off
    case kSoundOn: return g.soundOn(i) ? 1 : 0;
    case kSoundVolume: return g.volume(i);
    }
    return 0;
}

void Set(const void *c, int v)
{
    const GameOptions &g = *g_game;
    const int i = IndexOf(c);
    switch (WhatOf(c)) {
    case kSpeed: g.setSpeed(i, std::clamp(v / 10 * 10, 10, 100)); break;
    case kAutosave:
        if (g.autosave() != (v != 0)) {
            g.setAutosave(v != 0);
        }
        break;
    case kMessage: g.setMessage(i, 2 - std::clamp(v, 0, 2)); break;
    case kSoundOn: g.setSoundOn(i, v != 0); break;
    case kSoundVolume: g.setVolume(i, std::clamp(v, 0, 100)); break;
    }
    static const char *const kNames[] = {"", "speed", "autosave", "message kind", "sound switch", "sound volume"};
    const int w = static_cast<int>(WhatOf(c));
    core_log("options window: the game's %s %d = %d", w > 0 && w < 6 ? kNames[w] : "?", i,
             w == kMessage ? 2 - std::clamp(v, 0, 2) : v); // messages: the game's value (0 Off, 1 Condensed, 2 Full)
}

Value V(What w, int i)
{
    return {&Get, &Set, Ctx(w, i)};
}

// Label and description from option_text ("game", key).
void Texts(Row &r, const char *key)
{
    const ot::Text *t = ot::Find("game", key);
    if (t) {
        r.label = t->label ? t->label : key;
        r.help = t->summary ? t->summary : "";
    }
}

// --- Difficulty and auto defend.

const Difficulty *g_diff = nullptr;
core::diag::Feature g_difficulty{"difficulty and auto defend (core/options/game_options)"};
constexpr int kLevels = 5;

int Level()
{
    return std::clamp(g_diff->level(), 0, kLevels - 1);
}

// The lowest difficulty of the mission follows the difficulty down, as the game's own dialog does when it closes.
void FollowLowest()
{
    const int now = g_diff->level();
    if (g_diff->lowest() > now) {
        g_diff->setLowest(now);
        core_log("options window: the lowest difficulty of this mission is now %d", now);
    }
}

struct Walk {
    int to;
};

// One level at a time, as the game's own arrows: the game reloads its data for each level.
void WalkNow(void *p)
{
    const Walk &w = *static_cast<Walk *>(p);
    const Difficulty &d = *g_diff;
    for (int k = 0; k < kLevels && Level() != w.to; ++k) {
        d.setButtonParam(Level() < w.to ? 0 : 1);
        d.step();
    }
}

int DifficultyGet(const void *c)
{
    if (!g_diff) {
        return 0;
    }
    return IndexOf(c) == 0 ? Level() : (g_diff->autoDefend() ? 1 : 0);
}

void DifficultySet(const void *c, int v)
{
    if (!g_diff || !core::diag::Enabled(g_difficulty)) {
        core_log("options window: the difficulty and auto defend are switched off; not changed");
        return;
    }
    if (IndexOf(c) == 1) {
        if ((v != 0) != g_diff->autoDefend()) {
            ToggleAutoDefend();
        }
        return;
    }
    const int from = Level();
    Walk w{std::clamp(v, 0, kLevels - 1)};
    const int param = g_diff->buttonParam();
    const bool ok = core::diag::Guarded(g_difficulty, &WalkNow, &w);
    // Outside the guard: after a fault the difficulty stays at the level reached (each step is complete in the game),
    // and the lowest still follows it, so a fault never keeps a lower difficulty out of the mission's result.
    g_diff->setButtonParam(param);
    FollowLowest();
    if (ok) {
        core_log("options window: the game's difficulty %d -> %d (lowest of this mission %d)", from, Level(),
                 g_diff->lowest());
    }
    else {
        core_log("options window: the difficulty failed at level %d; the difficulty and auto defend rows are off",
                 Level());
    }
}

void ToggleNow(void *p)
{
    bool &flipped = *static_cast<bool *>(p);
    if (g_diff->underAttack()) {
        core_log("options window: enemies are attacking: auto defend stays %s", g_diff->autoDefend() ? "on" : "off");
        core::ui::notice::Post(core::options::menu_text::kAutoDefendLocked);
        return;
    }
    g_diff->toggleAutoDefend();
    flipped = true;
    core_log("options window: the game's auto defend is now %s", g_diff->autoDefend() ? "on" : "off");
}

} // namespace

void AttachDifficulty(const Difficulty &d)
{
    g_diff = &d;
}

bool DifficultyAttached()
{
    return g_diff != nullptr;
}

bool DifficultySwitchedOff()
{
    return g_diff && !core::diag::Enabled(g_difficulty);
}

bool DifficultyShown()
{
    return g_diff && core::diag::Enabled(g_difficulty) && g_diff->inCity() && g_diff->singlePlayer();
}

void DifficultyRows(std::vector<Row> &rows)
{
    if (!DifficultyShown()) {
        return;
    }
    Row level;
    level.kind = Kind::Choice;
    level.id = "difficulty";
    Texts(level, "difficulty");
    for (int k = 0; k < kLevels; ++k) {
        level.choices.push_back(g_diff->levelName(k));
    }
    level.value = {&DifficultyGet, &DifficultySet, reinterpret_cast<const void *>(intptr_t{0})};
    level.defaultValue = std::clamp(g_diff->defaultLevel, 0, kLevels - 1);
    rows.push_back(std::move(level));
    Row defend;
    defend.kind = Kind::Check;
    defend.id = "auto_defend";
    Texts(defend, "auto_defend");
    defend.value = {&DifficultyGet, &DifficultySet, reinterpret_cast<const void *>(intptr_t{1})};
    defend.defaultValue = 0; // both games ship with it off
    rows.push_back(std::move(defend));
}

bool ToggleAutoDefend()
{
    if (!g_diff || !core::diag::Enabled(g_difficulty)) {
        core_log("options window: the difficulty and auto defend are switched off; auto defend not changed");
        return false;
    }
    bool flipped = false;
    if (!core::diag::Guarded(g_difficulty, &ToggleNow, &flipped)) {
        core_log("options window: auto defend failed; the difficulty and auto defend rows are off");
        return false;
    }
    return flipped;
}

void Attach(const GameOptions &g)
{
    g_game = &g;
}

bool Attached()
{
    return g_game != nullptr;
}

void DetachForTest()
{
    g_game = nullptr;
    g_diff = nullptr;
}

void SpeedRows(std::vector<Row> &rows, bool gameSpeed)
{
    if (!g_game) {
        return;
    }
    const char *keys[] = {"game_speed", "scroll_speed"};
    const int defaults[] = {kStartGameSpeed, kStartScrollSpeed}; // ours, also a new player's (CheckNewPlayer)
    for (int i = gameSpeed ? 0 : 1; i < 2; ++i) {
        Row r;
        r.kind = Kind::Slider;
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

void AutosaveRows(std::vector<Row> &rows)
{
    if (!g_game) {
        return;
    }
    Row r;
    r.kind = Kind::Check;
    r.id = "autosave";
    Texts(r, "autosave");
    r.value = V(kAutosave, 0);
    r.defaultValue = 1; // both games start with autosave on when they have no .inf
    rows.push_back(std::move(r));
}

void MessageRows(std::vector<Row> &rows)
{
    if (!g_game) {
        return;
    }
    const GameOptions &g = *g_game;
    const int kinds = g.messageKinds();
    // The names say enough (no tooltip on them); the values have one each. Shown Full, Condensed, Off (the game's
    // order reversed, so the arrows go from the most to the least): choice k is the game's value 2 - k.
    const ot::Text *t = ot::Find("game", "messages");
    const char *spells[] = {"full", "condensed", "off"};
    for (int i = 0; i < kinds; ++i) {
        if (i == g.fixedMessageKind) {
            continue;
        }
        Row r;
        r.kind = Kind::Choice;
        r.label = g.messageName(i);
        for (int k = 0; k < 3; ++k) {
            r.choices.push_back(g.messageModeName(2 - k));
            const ot::ChoiceText *c = t ? ot::Choice(*t, spells[k]) : nullptr;
            r.choiceHelp.push_back(c ? c->help : "");
        }
        r.value = V(kMessage, i);
        r.defaultValue = 2 - std::clamp(g.messageDefault(i), 0, 2);
        rows.push_back(std::move(r));
    }
}

void SoundRows(std::vector<Row> &rows)
{
    if (!g_game) {
        return;
    }
    for (int i = 0; i < kChannelCount; ++i) {
        const Channel &ch = g_game->channels[i];
        Row r;
        r.kind = Kind::CheckSlider;
        r.id = ch.key;
        Texts(r, ch.key);
        r.toggle = V(kSoundOn, i);
        r.value = V(kSoundVolume, i);
        r.min = 0;
        r.max = 100;
        r.step = 5; // the Sound options' arrows
        r.unit = "%";
        r.defaultValue = ch.defaultVolume;
        r.defaultToggle = 1;
        rows.push_back(std::move(r));
    }
}

void ReapplyVolumes()
{
    if (!g_game) {
        return;
    }
    for (int i = 0; i < kChannelCount; ++i) {
        if (g_game->channels[i].stream) {
            g_game->setVolume(i, std::clamp(g_game->volume(i), 0, 100));
        }
    }
}

bool CheckNewPlayer(const NewPlayer &n)
{
    const int game = n.speed(0), scroll = n.speed(1);
    if (n.hasPlayer()) {
        core_log("options window: a player file exists: the speeds stay as the game read them (%d%%, %d%%)", game,
                 scroll);
        return false;
    }
    bool shipped = false;
    for (int k = 0; k < n.shippedCount; ++k) {
        shipped = shipped || (n.shipped[k][0] == game && n.shipped[k][1] == scroll);
    }
    if (!shipped) {
        core_log("options window: no player file, but the speeds (%d%%, %d%%) are not the shipped ones: they stay",
                 game, scroll);
        return false;
    }
    n.writeSpeeds(kStartGameSpeed, kStartScrollSpeed);
    core_log("options window: a new player (no player file, shipped speeds %d%%, %d%%) starts at game speed %d%%, "
             "scroll speed %d%%",
             game, scroll, kStartGameSpeed, kStartScrollSpeed);
    return true;
}

bool PlayerFileIn(const std::wstring &folder)
{
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((folder + L"Save\\*.dat").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    do {
        found = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    } while (!found && FindNextFileW(h, &f));
    FindClose(h);
    return found;
}

} // namespace core::options::game_options
