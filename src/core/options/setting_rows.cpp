#include "core/options/setting_rows.h"

#include "core/display/common_sizes.h"
#include "core/display/logical_size.h"
#include "core/display/virtual_display.h"
#include "core/game.h"
#include "core/log.h"
#include "core/options/option_text.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

namespace core::options::setting_rows {
namespace {

namespace cs = core::settings;

// A dry run of the game layer's page building (at attach): Add and Toggle only collect the settings the windows list,
// for the debug panel (core::ui::SetListedElsewhere); nothing of the game is read.
bool g_dry = false;
std::vector<int> g_listedRows;

struct Setting {
    int index;
    std::vector<std::string> spell; // Choice / Resolution: the ini spellings, by choice index
    int min = 0, step = 1;
    const char *autoWord = nullptr;
    const char *shownIfValue = nullptr; // the item's own shownIfValue, or the window's (ShownIf)
    int pendingAlt = -1; // a Key row's alternative waiting for its first key to be set (a swap of the two)
};
std::deque<Setting> g_settings; // the rows' contexts (rebuilt with the pages at each opening)

int Parse(const Setting &s, const char *text)
{
    const cs::Item &it = cs::Get(s.index);
    switch (it.type) {
    case cs::Type::Bool: return std::strcmp(text, "on") == 0;
    case cs::Type::Hotkey: { // the first key (Key rows read both with KeyGet)
        core::ui::Hotkey keys[core::ui::kMaxKeys] = {};
        return core::ui::ParseHotkeyList(text, text + std::strlen(text), keys) > 0 ? KeyCode(keys[0].vk, keys[0].mods)
                                                                                  : 0;
    }
    case cs::Type::Int:
        if (s.autoWord && std::strcmp(text, s.autoWord) == 0) {
            return s.min - s.step;
        }
        return std::atoi(text);
    default:
        for (size_t k = 0; k < s.spell.size(); ++k) {
            if (s.spell[k] == text) {
                return static_cast<int>(k);
            }
        }
        return 0;
    }
}

// A key setting's keys as KeyCodes (0: none).
void KeyCodes(const char *text, int codes[2])
{
    core::ui::Hotkey keys[core::ui::kMaxKeys] = {};
    const int n = core::ui::ParseHotkeyList(text, text + std::strlen(text), keys);
    for (int k = 0; k < 2; ++k) {
        codes[k] = k < n ? KeyCode(keys[k].vk, keys[k].mods) : 0;
    }
}

int KeyGet(const void *ctx, int col)
{
    const Setting &s = *static_cast<const Setting *>(ctx);
    char v[cs::kValueSize];
    cs::Value(s.index, v, sizeof(v));
    int codes[2];
    KeyCodes(v, codes);
    return codes[col];
}
int KeyGet0(const void *ctx)
{
    return KeyGet(ctx, 0);
}
int KeyGet1(const void *ctx)
{
    return KeyGet(ctx, 1);
}

void KeyWrite(const Setting &s, int first, int second)
{
    core::ui::Hotkey keys[2];
    int n = 0;
    for (int code : {first, second}) {
        if (code != 0) {
            keys[n++] = core::ui::Hotkey{static_cast<unsigned>(code) & 0xffff, static_cast<unsigned>(code) >> 16};
        }
    }
    char v[cs::kValueSize] = ""; // no key at all: the action has none
    core::ui::FormatHotkeyList(keys, n, v, sizeof(v));
    const cs::Item &it = cs::Get(s.index);
    core_log("options window: %s.%s = %s", it.section, it.key, v);
    cs::Request(s.index, v, true);
    cs::RunPending();
}

// The window sets the alternative before the first key (Window::Apply): when the new alternative is the key that is
// still first (the two swapped), it waits for the first key's set.
void KeySet1(const void *ctx, int value)
{
    Setting &s = *static_cast<Setting *>(const_cast<void *>(ctx));
    const int first = KeyGet(ctx, 0);
    if (value != 0 && value == first) {
        s.pendingAlt = value;
        return;
    }
    KeyWrite(s, first, value);
}
void KeySet0(const void *ctx, int value)
{
    Setting &s = *static_cast<Setting *>(const_cast<void *>(ctx));
    const int second = s.pendingAlt >= 0 ? s.pendingAlt : KeyGet(ctx, 1);
    s.pendingAlt = -1;
    KeyWrite(s, value, value == second ? 0 : second);
}

int SettingGet(const void *ctx)
{
    const Setting &s = *static_cast<const Setting *>(ctx);
    char v[cs::kValueSize];
    cs::Value(s.index, v, sizeof(v));
    return Parse(s, v);
}

void SettingSet(const void *ctx, int value)
{
    const Setting &s = *static_cast<const Setting *>(ctx);
    const cs::Item &it = cs::Get(s.index);
    char v[cs::kValueSize];
    switch (it.type) {
    case cs::Type::Bool: std::snprintf(v, sizeof(v), "%s", value ? "on" : "off"); break;
    case cs::Type::Hotkey:
        if (value == 0) {
            return;
        }
        core::ui::FormatHotkey(core::ui::Hotkey{static_cast<unsigned>(value) & 0xffff, static_cast<unsigned>(value) >> 16},
                               v, sizeof(v));
        break;
    case cs::Type::Int:
        if (s.autoWord && value < s.min) {
            std::snprintf(v, sizeof(v), "%s", s.autoWord);
        }
        else {
            std::snprintf(v, sizeof(v), "%d", value);
        }
        break;
    default:
        if (value < 0 || value >= static_cast<int>(s.spell.size())) {
            return;
        }
        std::snprintf(v, sizeof(v), "%s", s.spell[value].c_str());
        break;
    }
    core_log("options window: %s.%s = %s", it.section, it.key, v);
    // A setting that lays the screen out again is saved only when "keep this setting?" is answered with Keep (the
    // window's OnKept), as the panel does: if the game does not survive it, the file still has the previous one.
    cs::Request(s.index, v, it.effect != cs::Effect::Relayout);
    // Carried out now: the window takes input on the game thread, where RunPending runs; the window reads the values
    // back right after Apply (seen in testing: a queued value was read back as the old one, so Apply stayed on and a
    // second Apply set the old value again).
    cs::RunPending();
}

// "sharp_bilinear" -> "Sharp bilinear", "1920x1080" -> "1920 x 1080".
std::string Shown(const std::string &spell)
{
    std::string out;
    for (size_t k = 0; k < spell.size(); ++k) {
        char c = spell[k];
        if (c == '_') {
            out += ' ';
        }
        else if (c == 'x' && k > 0 && k + 1 < spell.size() && std::isdigit(static_cast<unsigned char>(spell[k - 1])) &&
                 std::isdigit(static_cast<unsigned char>(spell[k + 1]))) {
            out += " x ";
        }
        else {
            out += k == 0 ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        }
    }
    return out;
}

void ResolutionChoices(Setting &s)
{
    char own[24];
    std::snprintf(own, sizeof(own), "%dx%d", core_game()->native_width, core_game()->native_height);
    s.spell = {"scaled", own};
    const SIZE n = core::display::NativeSize();
    int minW = 0, minH = 0;
    core::display::GameMinimum(&minW, &minH);
    core::display::CommonSize sizes[48];
    int count = core::display::CommonSizes(n.cx, n.cy, minW, minH, false, sizes, 48);
    for (int k = 0; k < count; ++k) {
        char v[24];
        std::snprintf(v, sizeof(v), "%dx%d", sizes[k].w, sizes[k].h);
        if (std::strcmp(v, own) != 0) {
            s.spell.push_back(v);
        }
    }
    char now[cs::kValueSize];
    cs::Value(s.index, now, sizeof(now));
    bool listed = false;
    for (const auto &v : s.spell) {
        listed = listed || v == now;
    }
    if (!listed) {
        s.spell.push_back(now); // a size typed into the file
    }
}

} // namespace

void Clear()
{
    g_settings.clear();
}

void SetDry(bool dry)
{
    g_dry = dry;
    if (dry) {
        g_listedRows.clear();
    }
}

bool Dry()
{
    return g_dry;
}

const std::vector<int> &Listed()
{
    return g_listedRows;
}

// A row for section.key, appended to rows; nothing when the setting does not exist.
void Add(std::vector<Row> &rows, const char *section, const char *key)
{
    int i = cs::Find(section, key);
    if (i < 0) {
        return;
    }
    if (g_dry) {
        g_listedRows.push_back(i);
        return;
    }
    const cs::Item &it = cs::Get(i);
    g_settings.push_back(Setting{i, {}, it.min, it.step > 0 ? it.step : 1, it.autoWord});
    Setting &s = g_settings.back();
    Row r;
    r.id = key;
    const option_text::Text *text = option_text::Find(section, key);
    // The shared texts name the game's product as {product} (the registry's own are filled in already).
    r.label = text && text->label ? cs::GameNames(text->label) : it.label;
    r.help = text && text->summary ? cs::GameNames(text->summary) : it.help ? it.help : "";
    r.restart = it.effect == cs::Effect::Restart && !(text && text->noRestartMark);
    r.relayout = it.effect == cs::Effect::Relayout;
    r.value = {&SettingGet, &SettingSet, &s};
    switch (it.type) {
    case cs::Type::Bool: r.kind = Kind::Check; break;
    case cs::Type::Int:
        r.kind = Kind::Slider;
        r.min = it.min;
        r.max = it.max;
        r.step = s.step;
        r.maxNow = it.maxNow;
        r.hasAuto = it.autoWord != nullptr;
        r.unit = it.unit ? it.unit : "";
        break;
    case cs::Type::Choice: {
        r.kind = Kind::Choice;
        char now[cs::kValueSize];
        cs::Value(i, now, sizeof(now));
        for (int k = 0; k < it.choiceCount; ++k) {
            if (cs::ChoiceListed(it, k) || std::strcmp(now, it.choices[k]) == 0) { // the current one even if not offered
                s.spell.push_back(it.choices[k]);
            }
        }
        break;
    }
    case cs::Type::Resolution:
        r.kind = Kind::Choice;
        ResolutionChoices(s);
        break;
    case cs::Type::Hotkey: { // the Hotkeys window: the first key and the alternative
        r.kind = Kind::Key;
        r.value = {&KeyGet0, &KeySet0, &s};
        r.toggle = {&KeyGet1, &KeySet1, &s};
        int codes[2];
        KeyCodes(it.defaultValue, codes);
        r.defaultToggle = codes[1];
        break;
    }
    }
    for (const auto &v : s.spell) {
        const option_text::ChoiceText *c = text ? option_text::Choice(*text, v.c_str()) : nullptr;
        const bool own = c && std::strcmp(c->spell, "*") != 0; // a name of its own only for its own entry
        r.choices.push_back(own && c->shown ? std::string(c->shown) : Shown(v));
        r.choiceHelp.push_back(c ? c->help : "");
        r.choiceRestart.push_back(own && c->restart);
    }
    r.defaultValue = Parse(s, it.defaultValue);
    if (it.shownIfKey) {
        r.shownIfId = it.shownIfKey;
        r.shownIfValue = -1; // resolved by Resolve (the other row's index of shownIfValue)
        s.shownIfValue = it.shownIfValue;
    }
    rows.push_back(std::move(r));
}

// The last row (a slider) gets the on / off setting section.key as its checkbox: a check-slider row (the master
// volume and its switch, as the game's four sound rows).
void Toggle(std::vector<Row> &rows, const char *section, const char *key)
{
    int i = cs::Find(section, key);
    if (g_dry && i >= 0) {
        g_listedRows.push_back(i);
        return;
    }
    if (i < 0 || rows.empty() || rows.back().kind != Kind::Slider || cs::Get(i).type != cs::Type::Bool) {
        return;
    }
    const cs::Item &it = cs::Get(i);
    g_settings.push_back(Setting{i, {}, 0, 1, nullptr});
    Setting &s = g_settings.back();
    Row &r = rows.back();
    r.kind = Kind::CheckSlider;
    r.toggle = {&SettingGet, &SettingSet, &s};
    r.defaultToggle = Parse(s, it.defaultValue);
}

// The last row shown only while the row `key` (same page) has `value`: for items whose condition the registry does
// not hold (the panel places display.scale beside the resolution by hand, core/ui/settings_view.cpp).
void ShownIf(std::vector<Row> &rows, const char *key, const char *value)
{
    if (rows.empty() || !rows.back().value.ctx) {
        return;
    }
    rows.back().shownIfId = key;
    rows.back().shownIfValue = -1;
    static_cast<Setting *>(const_cast<void *>(rows.back().value.ctx))->shownIfValue = value;
}

void Header(std::vector<Row> &rows, const char *title)
{
    Row r;
    r.kind = Kind::Header;
    r.label = title;
    rows.push_back(std::move(r));
}

// shownIf: the index of the item's shownIfValue among the other row's choices.
void Resolve(Page &page)
{
    for (auto &cat : page.categories) {
        for (auto &r : cat.rows) {
            if (r.shownIfId.empty()) {
                continue;
            }
            const char *want = static_cast<const Setting *>(r.value.ctx)->shownIfValue;
            for (auto &cat2 : page.categories) {
                for (auto &o : cat2.rows) {
                    if (o.id == r.shownIfId && o.value.ctx && want) {
                        r.shownIfValue = Parse(*static_cast<const Setting *>(o.value.ctx), want);
                    }
                }
            }
        }
    }
}

} // namespace core::options::setting_rows
