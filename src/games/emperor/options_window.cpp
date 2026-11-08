#include "games/emperor/options_window.h"

#include "core/diag/guard.h"
#include "core/display/common_sizes.h"
#include "core/display/logical_size.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/options/window.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"
#include "core/ui/settings_view.h"
#include "games/emperor/options_original.h"
#include "games/emperor/keys.h"
#include "games/emperor/options_text.h"
#include "games/emperor/site.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/archive/resolution_ui_original_settings.md §12.3, §13.2,
// §13.5; calling conventions: docs/symbols.md.
namespace emperor::options_window {
namespace {

namespace co = core::options;
namespace cs = core::settings;
namespace ot = emperor::options_text;
namespace oo = emperor::options_original;

// ---------------------------------------------------------------------------------------------------------------
// The game's drawing functions

const Site kImageGetId{"image_get_id", 0x408170, {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x8b, 0xc7}};
// Hooked by games/emperor/resolution before we attach (it checked their bytes then): their first bytes are now a
// jump, so they are not checked here. A call goes through resolution's detour, which passes ours on unchanged.
const Site kImageDraw{"image_draw", 0x413960, {}};
const Site kFillRect{"fill_rect", 0x5296f0, {}};
const Site kTextDraw{"text_draw", 0x528430, {0x51, 0x8a, 0x44, 0x24, 0x1c, 0x84, 0xc0, 0x74}};
const Site kTextWidth{"text_get_width", 0x527a80, {0x51, 0x53, 0x55, 0x56, 0x57, 0x68, 0x1c, 0x1e}};
const Site kRectOutline{"rect_outline", 0x529440, {0x53, 0x55, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x14}};
const Site kClipReset{"screen_clip_reset", 0x5bf860, {0x8b, 0x0d, 0x1c, 0xe0, 0x0d, 0x01, 0x8b, 0x15}};
// ui_sound(n) 0x5c4500 (__cdecl; ret): the interface sound n when sound effects are on. input_update_mouse 0x52b1f2
// plays sound 0 at each press of the left button, wherever it is: the game's click.
const Site kUiSound{"ui_sound", 0x5c4500, {0xa1, 0x30, 0x1a, 0x40, 0x01, 0x85, 0xc0, 0x74}};
using UiSound_fn = void(__cdecl *)(int);
constexpr int kClickSound = 0;
// draw_buffer 0x1156df8: where the game draws. 0 while the game makes its surfaces again (a display change).
const Var<void *> kDrawBuffer{0x1156df8};
// screen_present's call in game_wndproc's WM_PAINT (0x47693a): Windows asks for it while a display change sizes the
// window, before the game has its surfaces again. Nothing of ours is drawn on that frame.
constexpr DWORD kPresentFromPaint = 0x47693f;

using ImageGetId_fn = int(__thiscall *)(void *, int);
using ImageDraw_fn = void(__cdecl *)(int id, int x, int y, int, int, int);
using TextDraw_fn = int(__cdecl *)(const char *text, int x, int y, int font, int colour, int flag);
using TextWidth_fn = int(__cdecl *)(const char *text, int font);
using Rect_fn = void(__cdecl *)(int x, int y, int w, int h, int colour);
using Void_fn = void(__cdecl *)();

const Var<int> kDialogX{0x10de014}; // dialog_offset_x: where the game's 640 x 480 dialogs start
const Var<int> kDialogY{0x10de010};
const Var<char> kRedraw{0x10de036};     // screen_redraw
const Var<int> kColourMode{0x1156ddc};  // 2: RGB565 (tooltip_draw 0x50f9f5 picks its colours by it)
const DWORD kImages = 0x1c42130;        // image_manager

// Art (image_get_id groups; pak index << 9 | group). China_Interface is pak 1; China_Unloaded pak 0 [inferred:
// group 9 of pak 0 is drawn at dialog_offset + 0x50 by 0x501e04, and the mood orbs are groups 7 / 9 of
// China_Unloaded; checked on screen in the first test].
constexpr int kPanel = 0x20b, kInset = 0x20c, kButton = 0x202, kStrip = 0x202, kStripFirst = 0x1a;
constexpr int kFrame = 0x212, kLine = 0x21f, kCheck = 0x220, kThumb = 0x204, kSmallOrbs = 0x007;
constexpr int kArrowUp = 0x21b, kArrowDown = 0x21c, kArrowLeft = 0x21d, kArrowRight = 0x21e; // + 0 / 1 / 2
constexpr int kFontNormal = 0x1e14, kFontGold = 0x1e15, kFontTitle = 0x1e12, kFontSmall = 0x1e16,
              kFontRed = 0x1e19;
constexpr int kTile = 16;

int Id(int group)
{
    return Function<ImageGetId_fn>(kImageGetId)(reinterpret_cast<void *>(kImages), group);
}

void Image(int id, int x, int y)
{
    Function<ImageDraw_fn>(kImageDraw)(id, x, y, 0, 0, 0);
}

int FontId(co::Font f)
{
    switch (f) {
    case co::Font::Gold: return Id(kFontGold);
    case co::Font::Title: return Id(kFontTitle);
    case co::Font::Small: return Id(kFontSmall);
    case co::Font::Disabled: return Id(kFontRed);
    default: return Id(kFontNormal);
    }
}

std::vector<int> Positions(int size)
{
    int count = std::max(2, (size + kTile - 1) / kTile);
    std::vector<int> out{0};
    for (int k = 1; k < count - 1; ++k) {
        out.push_back(kTile * k);
    }
    out.push_back(size - kTile);
    return out;
}

// The tile layout of panel_draw_tiles 0x5bed30 and inset_panel_draw_tiles 0x5bf080 (symbols.md), as the mockups
// draw it (tools/mockups/optmock.py nine()).
struct Nine {
    int perRow, edgeCycle, rowsCycle;
    int tl, tr, bl, br, top, bottom, left, right, inside;
};
constexpr Nine kPanelTiles{12, 10, 10, 0, 0xb, 0x84, 0x8f, 1, 0x85, 0xc, 0x17, 0xd};
constexpr Nine kInsetTiles{7, 5, 5, 0, 6, 0x2a, 0x30, 1, 0x2b, 7, 0xd, 8};

void DrawNine(int base, const Nine &n, int x0, int y0, int w, int h)
{
    std::vector<int> xs = Positions(w), ys = Positions(h);
    int mid = 0;
    for (size_t r = 0; r < ys.size(); ++r) {
        int k = 0;
        for (size_t c = 0; c < xs.size(); ++c) {
            bool lc = c == xs.size() - 1, lr = r == ys.size() - 1;
            int i;
            if (r == 0) {
                i = c == 0 ? n.tl : lc ? n.tr : n.top + k;
            }
            else if (lr) {
                i = c == 0 ? n.bl : lc ? n.br : n.bottom + k;
            }
            else if (c == 0) {
                i = n.left + mid;
            }
            else if (lc) {
                i = n.right + mid;
            }
            else {
                i = n.inside + mid + k;
            }
            Image(base + i, x0 + xs[c], y0 + ys[r]);
            if (c > 0 && !lc) {
                k = (k + 1) % n.edgeCycle;
            }
        }
        if (r > 0 && r < ys.size() - 1) {
            mid = (mid + n.perRow) % (n.perRow * n.rowsCycle);
        }
    }
}

class GamePainter : public co::Painter {
public:
    int ox = 0, oy = 0;

    void Panel(int x, int y, int w, int h) override { DrawNine(Id(kPanel), kPanelTiles, ox + x, oy + y, w, h); }
    void Inset(int x, int y, int w, int h) override { DrawNine(Id(kInset), kInsetTiles, ox + x, oy + y, w, h); }
    void Pieces(int base, int x, int y, int w) // left, middle..., right
    {
        int n = std::max(2, w / kTile);
        for (int k = 0; k < n; ++k) {
            Image(base + (k == 0 ? 0 : k == n - 1 ? 2 : 1), ox + x + k * kTile, oy + y);
        }
    }
    void ButtonBar(int x, int y, int w, int style) override { Pieces(Id(kButton) + style * 3, x, y, w); }
    void Strip(int x, int y, int w) override { Pieces(Id(kStrip) + kStripFirst, x, y, w); }
    void Frame(int x, int y, int w, int h) override // button_frame_draw 0x5bf360, the darker set (+ 8)
    {
        int b = Id(kFrame) + 8;
        x += ox;
        y += oy;
        for (int xx = x + kTile; xx < x + w - kTile; xx += kTile) {
            Image(b + 1, xx, y);
            Image(b + 5, xx, y + h - kTile);
        }
        for (int yy = y + kTile; yy < y + h - kTile; yy += kTile) {
            Image(b + 7, x, yy);
            Image(b + 3, x + w - kTile, yy);
        }
        Image(b + 0, x, y);
        Image(b + 2, x + w - kTile, y);
        Image(b + 4, x + w - kTile, y + h - kTile);
        Image(b + 6, x, y + h - kTile);
    }
    void Check(int x, int y, bool on) override { Image(Id(kCheck) + (on ? 2 : 3), ox + x, oy + y); }
    void ArrowButton(int x, int y, co::Arrow a, co::ArrowState s) override
    {
        int g = a == co::Arrow::Left ? kArrowLeft : a == co::Arrow::Right ? kArrowRight
                : a == co::Arrow::Up ? kArrowUp : kArrowDown;
        Image(Id(g) + static_cast<int>(s), ox + x, oy + y);
    }
    void Knob(int x, int y) override { Image(Id(kSmallOrbs) + 4, ox + x, oy + y); }
    void Thumb(int x, int y) override { Image(Id(kThumb), ox + x, oy + y); }
    void Line(int x, int y, int w) override // hline_draw 0x5bf250
    {
        w = std::max(2 * kTile, w);
        int b = Id(kLine);
        Image(b, ox + x, oy + y);
        for (int xx = x + kTile; xx < x + w - kTile; xx += kTile) {
            Image(b + 1, ox + xx, oy + y);
        }
        Image(b + 2, ox + x + w - kTile, oy + y);
    }
    // y is the top of the letters (as in the mockups); text_draw puts them 3 pixels higher than that (measured on the
    // first test's screenshots: every font, every row 3 pixels above the mockup).
    void Text(const char *t, int x, int y, co::Font f) override
    {
        Function<TextDraw_fn>(kTextDraw)(t, ox + x, oy + y + kTextDown, FontId(f), 0, 0);
    }
    static constexpr int kTextDown = 3;
    int TextWidth(const char *t, co::Font f) override { return Function<TextWidth_fn>(kTextWidth)(t, FontId(f)); }
    void TipBox(int x, int y, int w, int h) override // tooltip_draw 0x50f9e9..0x50fa40
    {
        bool rgb565 = *kColourMode == 2;
        Function<Rect_fn>(kRectOutline)(ox + x, oy + y, w, h, rgb565 ? 0x104 : 0x84);
        Function<Rect_fn>(kFillRect)(ox + x + 1, oy + y + 1, w - 2, h - 2, rgb565 ? 0x6a24 : 0x3504);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Rows for our settings (core/settings)

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
        return core::ui::ParseHotkeyList(text, text + std::strlen(text), keys) > 0 ? co::KeyCode(keys[0].vk, keys[0].mods)
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
        codes[k] = k < n ? co::KeyCode(keys[k].vk, keys[k].mods) : 0;
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
    // back right after Apply (the first test: a queued value was read back as the old one, so Apply stayed on and a
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
    s.spell = {"scaled", "1024x768"};
    const SIZE n = core::display::NativeSize();
    int minW = 0, minH = 0;
    core::display::GameMinimum(&minW, &minH);
    core::display::CommonSize sizes[48];
    int count = core::display::CommonSizes(n.cx, n.cy, minW, minH, false, sizes, 48);
    for (int k = 0; k < count; ++k) {
        char v[24];
        std::snprintf(v, sizeof(v), "%dx%d", sizes[k].w, sizes[k].h);
        if (std::strcmp(v, "1024x768") != 0) {
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

// A dry run of BuildPages / BuildKeyPages (at Attach): Add and Toggle only collect the settings the windows list, for
// the debug panel (core::ui::SetListedElsewhere); nothing of the game is read.
bool g_dry = false;
std::vector<int> g_listedRows;

// A row for section.key, appended to rows; nothing when the setting does not exist.
void Add(std::vector<co::Row> &rows, const char *section, const char *key)
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
    co::Row r;
    r.id = key;
    const ot::Text *text = ot::Find(section, key);
    r.label = text && text->label ? text->label : it.label;
    r.help = text && text->summary ? text->summary : it.help ? it.help : "";
    r.restart = it.effect == cs::Effect::Restart && !(text && text->noRestartMark);
    r.relayout = it.effect == cs::Effect::Relayout;
    r.value = {&SettingGet, &SettingSet, &s};
    switch (it.type) {
    case cs::Type::Bool: r.kind = co::Kind::Check; break;
    case cs::Type::Int:
        r.kind = co::Kind::Slider;
        r.min = it.min;
        r.max = it.max;
        r.step = s.step;
        r.maxNow = it.maxNow;
        r.hasAuto = it.autoWord != nullptr;
        r.unit = it.unit ? it.unit : "";
        break;
    case cs::Type::Choice: {
        r.kind = co::Kind::Choice;
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
        r.kind = co::Kind::Choice;
        ResolutionChoices(s);
        break;
    case cs::Type::Hotkey: { // the Hotkeys window: the first key and the alternative
        r.kind = co::Kind::Key;
        r.value = {&KeyGet0, &KeySet0, &s};
        r.toggle = {&KeyGet1, &KeySet1, &s};
        int codes[2];
        KeyCodes(it.defaultValue, codes);
        r.defaultToggle = codes[1];
        break;
    }
    }
    for (const auto &v : s.spell) {
        const ot::ChoiceText *c = text ? ot::Choice(*text, v.c_str()) : nullptr;
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
void Toggle(std::vector<co::Row> &rows, const char *section, const char *key)
{
    int i = cs::Find(section, key);
    if (g_dry && i >= 0) {
        g_listedRows.push_back(i);
        return;
    }
    if (i < 0 || rows.empty() || rows.back().kind != co::Kind::Slider || cs::Get(i).type != cs::Type::Bool) {
        return;
    }
    const cs::Item &it = cs::Get(i);
    g_settings.push_back(Setting{i, {}, 0, 1, nullptr});
    Setting &s = g_settings.back();
    co::Row &r = rows.back();
    r.kind = co::Kind::CheckSlider;
    r.toggle = {&SettingGet, &SettingSet, &s};
    r.defaultToggle = Parse(s, it.defaultValue);
}

// The last row shown only while the row `key` (same page) has `value`: for items whose condition the registry does
// not hold (the panel places display.scale beside the resolution by hand, core/ui/settings_view.cpp).
void ShownIf(std::vector<co::Row> &rows, const char *key, const char *value)
{
    if (rows.empty() || !rows.back().value.ctx) {
        return;
    }
    rows.back().shownIfId = key;
    rows.back().shownIfValue = -1;
    static_cast<Setting *>(const_cast<void *>(rows.back().value.ctx))->shownIfValue = value;
}

void Header(std::vector<co::Row> &rows, const char *title)
{
    co::Row r;
    r.kind = co::Kind::Header;
    r.label = title;
    rows.push_back(std::move(r));
}

// shownIf: the index of the item's shownIfValue among the other row's choices.
void Resolve(co::Page &page)
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

std::vector<co::Page> BuildPages()
{
    g_settings.clear();
    co::Page general{"General", {}};
    {
        co::Category c{"General", {}};
        Add(c.rows, "ui", "intro_movie");
        Add(c.rows, "ui", "info_window");
        Add(c.rows, "ui", "minimap_picture_scale");
        general.categories.push_back(std::move(c));
    }
    {
        co::Category c{"Speed", {}};
        if (!g_dry) oo::SpeedRows(c.rows);
        Add(c.rows, "gameplay", "keep_speed");
        Add(c.rows, "gameplay", "full_speed");
        Header(c.rows, "Scrolling");
        Add(c.rows, "gameplay", "simulate_while_scrolling");
        Add(c.rows, "gameplay", "smooth_scroll");
        general.categories.push_back(std::move(c));
    }
    {
        co::Category c{"Autosave", {}};
        if (!g_dry) oo::AutosaveRows(c.rows);
        if (!c.rows.empty()) {
            general.categories.push_back(std::move(c));
        }
    }
    {
        co::Category c{ot::kCategoryMessages, {}};
        if (!g_dry) oo::MessageRows(c.rows);
        if (!c.rows.empty()) {
            general.categories.push_back(std::move(c));
        }
    }
    co::Page display{"Video", {}};
    {
        co::Category c{"Screen", {}};
        Add(c.rows, "video", "window");
        Add(c.rows, "video", "resolution");
        Add(c.rows, "video", "scale");
        ShownIf(c.rows, "resolution", "scaled");
        Add(c.rows, "video", "scaling");
        Add(c.rows, "video", "present");
        Add(c.rows, "video", "renderer");
        Add(c.rows, "video", "window_lock_cursor");
        display.categories.push_back(std::move(c));
    }
    {
        co::Category c{"City", {}};
        Add(c.rows, "video", "city_zoom");
        Add(c.rows, "video", "city_zoom_shrink");
        display.categories.push_back(std::move(c));
    }
    {
        co::Category c{"Menus and loading", {}};
        Add(c.rows, "video", "loading_screen");
        Add(c.rows, "video", "menu_screens");
        Header(c.rows, "City background");
        Add(c.rows, "video", "background_blur");
        Add(c.rows, "video", "background_brightness");
        display.categories.push_back(std::move(c));
    }
    {
        co::Category c{"Empire map", {}};
        Add(c.rows, "video", "empire_map");
        Add(c.rows, "video", "empire_map_scale");
        Add(c.rows, "video", "empire_map_background");
        display.categories.push_back(std::move(c));
    }
    co::Page sound{"Audio", {{"", {}}}};
    Add(sound.categories[0].rows, "audio", "master_volume"); // ours, over the game's four
    Toggle(sound.categories[0].rows, "audio", "master_sound");
    if (!g_dry) oo::SoundRows(sound.categories[0].rows);
    if (!sound.categories[0].rows.empty()) {
        Header(sound.categories[0].rows, "More");
    }
    Add(sound.categories[0].rows, "audio", "ambient_sounds");
    Add(sound.categories[0].rows, "audio", "background_sound");
    std::vector<co::Page> pages{std::move(general), std::move(display), std::move(sound)};
    for (auto &p : pages) {
        Resolve(p);
    }
    return pages;
}

// ---------------------------------------------------------------------------------------------------------------
// Opening, input, drawing

// The Hotkeys window: our keys, one page (the same size as the options window; the game's own keys join it when
// they can be changed).
std::vector<co::Page> BuildKeyPages()
{
    co::Category c{"", {}};
    co::Row columns;
    columns.kind = co::Kind::Columns;
    columns.label = ""; // no "Action" (the titles are above the list)
    columns.rightLabel = ot::kColumnKey;
    columns.rightLabel2 = ot::kColumnAlternative;
    c.rows.push_back(std::move(columns));
    // Augustus's sections, with Emperor's actions (docs/key_bindings.md section 3.2).
    for (int s = 0; s < emperor::keys::kSectionCount; ++s) {
        Header(c.rows, ot::KeySection(s));
        if (s == emperor::keys::kGlobal) { // ours first
            Add(c.rows, "ui", "panel_key");
            Add(c.rows, "debug", "screenshot_key");
            Add(c.rows, "debug", "report_key");
            Add(c.rows, "ui", "city_picture_key");
            Add(c.rows, "ui", "minimap_picture_key");
        }
        for (int a = 0; a < emperor::keys::ActionCount(); ++a) {
            const emperor::keys::ActionInfo &info = emperor::keys::Action(a);
            if (info.section != s) {
                continue;
            }
            const size_t before = c.rows.size();
            Add(c.rows, "keys", info.key);
            if (c.rows.size() == before || g_dry) {
                continue;
            }
            if (info.textGroup > 0) { // the overlays: the game's own names, "See Water" as "Water overlay" (Augustus's)
                std::string name = oo::GameText(info.textGroup, info.textIndex);
                if (name.compare(0, 4, "See ") == 0) {
                    name.erase(0, 4);
                }
                if (!name.empty()) {
                    char label[96];
                    std::snprintf(label, sizeof(label), ot::kOverlayName, name.c_str());
                    c.rows.back().label = label;
                }
            }
        }
    }
    return {co::Page{ot::kHotkeysTitle, {std::move(c)}}};
}

co::Window g_window; // the options window
co::Window g_keys;   // the Hotkeys window, over it or alone (from the game's Options menu)
GamePainter g_painter;
bool g_open = false;     // either is open
bool g_keysOpen = false; // the Hotkeys window is the one shown
bool g_keysOnly = false; // ... opened alone: closing it closes everything
bool g_waiting = false;  // a Key row waits for a key (core/ui SetOtherCapture)
core::diag::Feature g_feature{"options window (games/emperor)"};

co::Window &Front()
{
    return g_keysOpen ? g_keys : g_window;
}

void SetWaiting(bool waiting, unsigned endedVk)
{
    if (waiting != g_waiting) {
        g_waiting = waiting;
        core::ui::SetOtherCapture(waiting, waiting ? 0 : endedVk);
        if (waiting) {
            core_log("options window: waiting for a key (our keys and the game's do nothing meanwhile)");
        }
        else {
            core_log("options window: the wait for a key ended (key 0x%x)", endedVk);
        }
    }
}

void Close(const char *why)
{
    if (!g_open) {
        return;
    }
    g_open = g_keysOpen = g_keysOnly = false;
    SetWaiting(false, 0);
    *kRedraw = 1; // the screen under the window is drawn again
    core_log("options window: closed (%s)", why);
}

void CloseKeys(const char *why)
{
    SetWaiting(false, g_keys.EndedWaitVk());
    if (g_keysOnly) {
        Close(why);
        return;
    }
    g_keysOpen = false;
    core_log("options window: Hotkeys closed (%s)", why);
}

void Kept()
{
    cs::RequestSave(); // the setting that laid the screen out again is kept: now it goes into the file
    cs::RunPending();
    core_log("options window: the new setting kept");
}

// Keys nobody may choose (docs/key_bindings.md section 3.2): Enter (the game's, not offered), the game's keys for
// development and multiplayer (Ctrl+A, Alt+D, Alt+Z, Alt+T, Ctrl+Alt+C, Numpad2; comma and period are not hotkey keys),
// and ours that stay fixed (F5 and Alt+Enter: window or full screen; Shift+F7: the resolution, for testing).
bool ReservedKey(unsigned vk, unsigned mods)
{
    using core::ui::kModAlt;
    using core::ui::kModCtrl;
    using core::ui::kModShift;
    return vk == VK_RETURN || (vk == 'A' && mods == kModCtrl) || (mods == kModAlt && (vk == 'D' || vk == 'Z' || vk == 'T')) ||
           (vk == 'C' && mods == (kModCtrl | kModAlt)) || (vk == VK_NUMPAD2 && mods == 0) || (vk == VK_F5 && mods == 0) ||
           (vk == VK_F7 && mods == kModShift);
}

void Prepare(co::Window &w, bool keys)
{
    co::Look look;
    if (keys) {
        look.title = ot::kHotkeysTitle;
        look.hotkeysButton = look.applyButton = false;
        look.plainHeaders = true; // Augustus's hotkey list: the section titles in gold at the left
        look.columnsAbove = true; // ... and "Hotkey" / "Alternative" above the list
    }
    w.SetLook(look);
    co::Texts t;
    t.pressKey = ot::kPressKey;
    t.notChanged = ot::kKeyNotChanged;
    t.notUsable = ot::kKeyNotUsable;
    t.inUse = ot::kKeyInUse;
    t.reserved = ot::kKeyReserved;
    t.clearHint = ot::kKeyClearHint;
    t.keyTip = ot::kKeyTip;
    t.keepTitle = ot::kKeepTitle;
    t.keepCountdown = ot::kKeepCountdown;
    t.keep = ot::kKeep;
    t.revert = ot::kRevert;
    w.SetTexts(t);
    // The game's keys are in the Hotkeys window's list now: a key in use says which action has it (no game key note).
    w.SetReservedKey(&ReservedKey);
    w.SetOnKept(&Kept);
    w.SetRestartTip(ot::kRestartTip);
}

void OpenKeys(bool alone)
{
    Prepare(g_keys, true);
    g_keys.SetPages(BuildKeyPages());
    g_keys.Open(0, 0);
    g_keysOpen = true;
    g_keysOnly = alone;
    core_log("options window: Hotkeys opened%s", alone ? " (from the Options menu)" : "");
}

struct Where {
    int page, category;
    bool keys;            // the Hotkeys window alone
    const char *name;     // the category by its name (nullptr: by number)
};

void OpenNow(void *p)
{
    const Where &w = *static_cast<const Where *>(p);
    if (w.keys) {
        OpenKeys(true);
        g_open = true;
        return;
    }
    Prepare(g_window, false);
    g_window.SetPages(BuildPages());
    int category = w.category;
    if (w.name && w.page >= 0 && w.page < static_cast<int>(g_window.PageCount())) {
        const auto &cats = g_window.Categories(w.page);
        for (size_t k = 0; k < cats.size(); ++k) {
            if (cats[k].name == w.name) {
                category = static_cast<int>(k);
            }
        }
    }
    g_window.Open(w.page, category);
    g_open = true;
    g_keysOpen = g_keysOnly = false;
    core_log("options window: opened (page %d, category %d)", w.page, w.category);
}

bool g_attached = false;

struct Message {
    UINT m;
    WPARAM w;
    int x, y;
};

void HandleNow(void *p)
{
    const Message &msg = *static_cast<const Message *>(p);
    const int x = msg.x - *kDialogX, y = msg.y - *kDialogY;
    co::Window &w = Front();
    co::Action a = co::Action::None;
    switch (msg.m) {
    case WM_MOUSEMOVE: w.MouseMove(x, y); break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        Function<UiSound_fn>(kUiSound)(kClickSound); // as the game: a click sound at every press, even on the panel
        a = w.MouseDown(x, y, GetTickCount());
        break;
    case WM_LBUTTONUP: a = w.MouseUp(x, y); break;
    case WM_RBUTTONUP: // the game closes its dialogs with a right click (not while a key is awaited); on a key box it
                       // removes that key
        if (w.RightClick(x, y)) {
            Function<UiSound_fn>(kUiSound)(kClickSound);
            break;
        }
        if (!w.WaitingForKey()) {
            w.Closing();
            w.Cancel();
            a = co::Action::Close;
        }
        break;
    case WM_MOUSEWHEEL: w.Wheel(GET_WHEEL_DELTA_WPARAM(msg.w) / WHEEL_DELTA); break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: { // Alt combinations and F10 come as system keys
        auto held = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };
        const unsigned mods = (held(VK_CONTROL) ? core::ui::kModCtrl : 0u) |
                              (held(VK_SHIFT) ? core::ui::kModShift : 0u) | (held(VK_MENU) ? core::ui::kModAlt : 0u);
        if (!w.WaitingForKey() && !w.Confirming() && emperor::keys::IsOptionsKey(static_cast<unsigned>(msg.w), mods)) {
            a = w.Key(VK_ESCAPE, 0); // the key that opened the window closes it (as Esc: nothing kept)
        }
        else {
            a = w.Key(static_cast<int>(msg.w), mods);
        }
        break;
    }
    default: break;
    }
    SetWaiting(w.WaitingForKey(), w.EndedWaitVk());
    if (a == co::Action::Close) {
        if (g_keysOpen) {
            CloseKeys("OK or Cancel");
        }
        else {
            Close("OK or Cancel");
        }
    }
    else if (a == co::Action::OpenHotkeys) {
        OpenKeys(false);
    }
}

bool Active()
{
    return g_open;
}

void OnMessage(UINT m, WPARAM w, LPARAM, int x, int y)
{
    Message msg{m, w, x, y};
    if (!core::diag::Guarded(g_feature, &HandleNow, &msg)) {
        g_open = g_keysOpen = g_keysOnly = false;
        SetWaiting(false, 0); // a wait for a key ends with the window (our keys work again)
        *kRedraw = 1;
        core_log("options window: closed after an error; the game's own options stay");
    }
}

const core::display::ModalInput kInput{&Active, &OnMessage};

void DrawNow(void *)
{
    Function<Void_fn>(kClipReset)();
    g_painter.ox = *kDialogX;
    g_painter.oy = *kDialogY;
    co::Window &w = Front();
    w.Tick(GetTickCount());
    if (w.TakeCloseRequest()) { // OK, then "keep this setting?" answered or timed out
        if (g_keysOpen) {
            CloseKeys("OK");
        }
        else {
            Close("OK");
        }
        return;
    }
    Front().Draw(g_painter); // the Hotkeys window covers the options window (the same size)
    *kRedraw = 1; // the next frame draws the screen under the window again (tooltips, hover frames)
}

} // namespace

bool Available()
{
    return g_attached && core::diag::Enabled(g_feature);
}

bool Open(int page, int category)
{
    if (!Available()) {
        return false;
    }
    if (g_open) {
        return true;
    }
    Where w{page, category, false, nullptr};
    if (!core::diag::Guarded(g_feature, &OpenNow, &w)) {
        g_open = g_keysOpen = g_keysOnly = false;
        SetWaiting(false, 0); // a wait for a key ends with the window (our keys work again)
        core_log("options window: not available after an error (the game's own options stay)");
        return false;
    }
    return true;
}

bool OpenCategory(int page, const char *name)
{
    if (!Available()) {
        return false;
    }
    if (g_open) {
        return true;
    }
    Where w{page, 0, false, name};
    if (!core::diag::Guarded(g_feature, &OpenNow, &w)) {
        g_open = g_keysOpen = g_keysOnly = false;
        SetWaiting(false, 0); // a wait for a key ends with the window (our keys work again)
        core_log("options window: not available after an error (the game's own options stay)");
        return false;
    }
    return true;
}

bool OpenHotkeys()
{
    if (!Available()) {
        return false;
    }
    if (g_open) {
        return true;
    }
    Where w{0, 0, true, nullptr};
    if (!core::diag::Guarded(g_feature, &OpenNow, &w)) {
        g_open = g_keysOpen = g_keysOnly = false;
        SetWaiting(false, 0); // a wait for a key ends with the window (our keys work again)
        core_log("options window: not available after an error");
        return false;
    }
    return true;
}

bool IsOpen()
{
    return g_open;
}

void OnPresent(unsigned long caller)
{
    if (!g_open || caller == kPresentFromPaint || *kDrawBuffer == nullptr) {
        return;
    }
    if (!core::diag::Guarded(g_feature, &DrawNow, nullptr)) {
        g_open = g_keysOpen = g_keysOnly = false;
        SetWaiting(false, 0); // a wait for a key ends with the window (our keys work again)
        *kRedraw = 1;
        core_log("options window: closed after an error while drawing; the game's own options stay");
    }
}

bool Attach()
{
    // image_draw and fill_rect: checked by resolution, which is attached before us (emperor_attach).
    const Site *sites[] = {&kImageGetId, &kTextDraw, &kTextWidth, &kRectOutline, &kClipReset, &kUiSound};
    for (const Site *s : sites) {
        if (!Matches(*s)) {
            core_log("options window: %s at 0x%lx does not match; not available", s->name, s->address);
            return false;
        }
    }

    core::display::SetModalInput(&kInput);
    g_attached = true;
    core_log("options window: available (the game's Options menu and the main menu open it)");

    // The debug panel leaves out what these windows list, except the settings to change there when something goes
    // wrong (the screen; the panel's own key) (2026-11-08).
    static const char *const kEmergency[][2] = {{"video", "resolution"}, {"video", "scale"},   {"video", "window"},
                                                 {"video", "renderer"},   {"video", "scaling"}, {"ui", "panel_key"}};
    g_dry = true;
    g_listedRows.clear();
    BuildPages();
    BuildKeyPages();
    g_dry = false;
    g_settings.clear();
    std::vector<int> hide;
    for (int i : g_listedRows) {
        bool keep = false;
        for (const auto &e : kEmergency) {
            keep = keep || i == cs::Find(e[0], e[1]);
        }
        if (!keep) {
            hide.push_back(i);
        }
    }
    core::ui::SetListedElsewhere(hide.data(), static_cast<int>(hide.size()));
    core_log("options window: the debug panel leaves out %d setting(s) listed here (\"Show all settings\" shows them)",
             static_cast<int>(hide.size()));
    return true;
}

} // namespace emperor::options_window
