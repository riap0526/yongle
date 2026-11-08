#include "games/emperor/options_window.h"

#include "core/diag/guard.h"
#include "core/display/common_sizes.h"
#include "core/display/logical_size.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/options/window.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"
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

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/resolution_ui/original_settings.md §12.3, §13.2,
// §13.5; calling conventions: docs/symbols.md.
namespace emperor::options_window {
namespace {

namespace co = core::options;
namespace cs = core::settings;
namespace ot = emperor::options_text;

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
};
std::deque<Setting> g_settings; // the rows' contexts (rebuilt with the pages at each opening)

int Parse(const Setting &s, const char *text)
{
    const cs::Item &it = cs::Get(s.index);
    switch (it.type) {
    case cs::Type::Bool: return std::strcmp(text, "on") == 0;
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
    cs::Request(s.index, v, true);
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

// A row for section.key, appended to rows; nothing when the setting does not exist.
void Add(std::vector<co::Row> &rows, const char *section, const char *key)
{
    int i = cs::Find(section, key);
    if (i < 0) {
        return;
    }
    const cs::Item &it = cs::Get(i);
    g_settings.push_back(Setting{i, {}, it.min, it.step > 0 ? it.step : 1, it.autoWord});
    Setting &s = g_settings.back();
    co::Row r;
    r.id = key;
    r.label = it.label;
    const ot::Text *text = ot::Find(section, key);
    r.help = text && text->summary ? text->summary : it.help ? it.help : "";
    r.restart = it.effect == cs::Effect::Restart && !(text && text->noRestartMark);
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
    case cs::Type::Choice:
        r.kind = co::Kind::Choice;
        for (int k = 0; k < it.choiceCount; ++k) {
            s.spell.push_back(it.choices[k]);
        }
        break;
    case cs::Type::Resolution:
        r.kind = co::Kind::Choice;
        ResolutionChoices(s);
        break;
    case cs::Type::Hotkey: return; // the Hotkeys window
    }
    for (const auto &v : s.spell) {
        r.choices.push_back(Shown(v));
        r.choiceHelp.push_back(text ? ot::ChoiceHelp(*text, v.c_str()) : "");
    }
    r.defaultValue = Parse(s, it.defaultValue);
    if (it.shownIfKey) {
        r.shownIfId = it.shownIfKey;
        r.shownIfValue = -1; // resolved by Resolve (the other row's index of shownIfValue)
        s.shownIfValue = it.shownIfValue;
    }
    rows.push_back(std::move(r));
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
        Add(c.rows, "display", "intro_movie");
        Add(c.rows, "ui", "info_window");
        Add(c.rows, "ui", "minimap_picture_scale");
        Add(c.rows, "timing", "high_resolution_timer");
        general.categories.push_back(std::move(c));
    }
    {
        co::Category c{"Speed", {}};
        Add(c.rows, "gameplay", "keep_speed");
        Add(c.rows, "gameplay", "full_speed");
        Header(c.rows, "Scrolling");
        Add(c.rows, "gameplay", "simulate_while_scrolling");
        Add(c.rows, "gameplay", "smooth_scroll");
        general.categories.push_back(std::move(c));
    }
    co::Page display{"Display", {}};
    {
        co::Category c{"Screen", {}};
        Add(c.rows, "display", "window");
        Add(c.rows, "display", "resolution");
        Add(c.rows, "display", "scale");
        ShownIf(c.rows, "resolution", "scaled");
        Add(c.rows, "display", "scaling");
        Add(c.rows, "display", "present");
        Add(c.rows, "display", "renderer");
        Add(c.rows, "display", "window_lock_cursor");
        display.categories.push_back(std::move(c));
    }
    {
        co::Category c{"City", {}};
        Add(c.rows, "display", "city_zoom");
        Add(c.rows, "display", "city_zoom_shrink");
        display.categories.push_back(std::move(c));
    }
    {
        co::Category c{"Menus and loading", {}};
        Add(c.rows, "display", "loading_screen");
        Add(c.rows, "display", "menu_screens");
        Header(c.rows, "City background");
        Add(c.rows, "display", "background_blur");
        Add(c.rows, "display", "background_brightness");
        display.categories.push_back(std::move(c));
    }
    {
        co::Category c{"China map", {}};
        Add(c.rows, "display", "china_map");
        Add(c.rows, "display", "china_map_scale");
        Add(c.rows, "display", "china_map_background");
        display.categories.push_back(std::move(c));
    }
    co::Page sound{"Sound", {{"", {}}}};
    Add(sound.categories[0].rows, "sound", "background_sound");
    Add(sound.categories[0].rows, "sound", "ambient_sounds");
    std::vector<co::Page> pages{std::move(general), std::move(display), std::move(sound)};
    for (auto &p : pages) {
        Resolve(p);
    }
    return pages;
}

// ---------------------------------------------------------------------------------------------------------------
// Opening, input, drawing

co::Window g_window;
GamePainter g_painter;
bool g_open = false;
core::diag::Feature g_feature{"options window (games/emperor)"};

void Close(const char *why)
{
    if (!g_open) {
        return;
    }
    g_open = false;
    *kRedraw = 1; // the screen under the window is drawn again
    core_log("options window: closed (%s)", why);
}

void OpenNow(void *)
{
    g_window.SetPages(BuildPages());
    g_window.SetRestartTip(ot::kRestartTip);
    g_window.Open(0, 0);
    g_open = true;
    core_log("options window: opened");
}

void Open()
{
    if (g_open || !core::diag::Enabled(g_feature)) {
        return;
    }
    if (!core::diag::Guarded(g_feature, &OpenNow, nullptr)) {
        g_open = false;
        core_log("options window: not available after an error (the game's own options stay)");
    }
}

struct Message {
    UINT m;
    WPARAM w;
    int x, y;
};

void HandleNow(void *p)
{
    const Message &msg = *static_cast<const Message *>(p);
    const int x = msg.x - *kDialogX, y = msg.y - *kDialogY;
    co::Action a = co::Action::None;
    switch (msg.m) {
    case WM_MOUSEMOVE: g_window.MouseMove(x, y); break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        Function<UiSound_fn>(kUiSound)(kClickSound); // as the game: a click sound at every press, even on the panel
        a = g_window.MouseDown(x, y, GetTickCount());
        break;
    case WM_LBUTTONUP: a = g_window.MouseUp(x, y); break;
    case WM_RBUTTONUP: // the game closes its dialogs with a right click
        g_window.Cancel();
        a = co::Action::Close;
        break;
    case WM_MOUSEWHEEL: g_window.Wheel(GET_WHEEL_DELTA_WPARAM(msg.w) / WHEEL_DELTA); break;
    case WM_KEYDOWN: a = g_window.Key(static_cast<int>(msg.w)); break;
    default: break;
    }
    if (a == co::Action::Close) {
        Close("OK or Cancel");
    }
    else if (a == co::Action::OpenHotkeys) {
        core_log("options window: Hotkeys (not yet: S2e)");
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
        g_open = false;
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
    g_window.Tick(GetTickCount());
    g_window.Draw(g_painter);
    *kRedraw = 1; // the next frame draws the screen under the window again (tooltips, hover frames)
}

// Temporary entry for the first test build (S2b): Ctrl+O. S2d puts the window in the game's Options menu and the
// main menu instead.
core::display::ActionKey g_openKey{'O', core::ui::kModCtrl, [] { Open(); }};

} // namespace

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
        g_open = false;
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
    core::display::AddActionKey(&g_openKey);
    core_log("options window: available (Ctrl+O while testing)");
    return true;
}

} // namespace emperor::options_window
