#include "games/emperor/options_window.h"

#include "core/diag/guard.h"
#include "core/display/tile_layout.h"
#include "core/log.h"
#include "core/options/game_options.h"
#include "core/options/hotkeys_page.h"
#include "core/options/menu.h"
#include "core/options/options_host.h"
#include "core/options/pause_menu.h"
#include "core/options/setting_rows.h"
#include "core/options/window.h"
#include "core/ui/hotkey.h"
#include "games/emperor/options_original.h"
#include "games/emperor/game_menu.h"
#include "games/emperor/options_text.h"
#include "games/emperor/site.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): the game's drawing functions and art read from the disassembly;
// calling conventions checked by disassembly.
namespace emperor::options_window {
namespace {

namespace co = core::options;
namespace pm = core::options::pause_menu;
namespace sr = core::options::setting_rows;
namespace oh = core::options::options_host;
namespace ot = emperor::options_text;
namespace oo = emperor::options_original;
namespace go = core::options::game_options;
namespace tl = core::display::tiles;
using sr::Add;
using sr::Header;
using sr::Resolve;
using sr::ShownIf;
using sr::Toggle;

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
// The game's picture (the options window keeps the one under it): screen_pitch, dd_width / dd_height, game_screen.
const Var<int> kPitch{0x10de000};
const Var<int> kWidth{0x10de008}, kHeight{0x10de004};
const Var<unsigned char> kGameScreen{0x10de045};

bool GameFrame(oh::GameFrame *out)
{
    out->pixels = static_cast<unsigned char *>(*kDrawBuffer);
    out->pitch = *kPitch;
    out->width = *kWidth;
    out->height = *kHeight;
    out->screen = *kGameScreen;
    return out->pixels != nullptr;
}

// Art (image_get_id groups; pak index << 9 | group). China_Interface is pak 1; China_Unloaded pak 0 [inferred:
// group 9 of pak 0 is drawn at dialog_offset + 0x50 by 0x501e04, and the mood orbs are groups 7 / 9 of
// China_Unloaded; checked on screen].
constexpr int kPanel = 0x20b, kInset = 0x20c, kButton = 0x202, kStrip = 0x202, kStripFirst = 0x1a;
constexpr int kFrame = 0x212, kLine = 0x21f, kCheck = 0x220, kThumb = 0x204, kSmallOrbs = 0x007;
constexpr int kArrowUp = 0x21b, kArrowDown = 0x21c, kArrowLeft = 0x21d, kArrowRight = 0x21e; // + 0 / 1 / 2
constexpr int kFontNormal = 0x1e14, kFontGold = 0x1e15, kFontTitle = 0x1e12, kFontSmall = 0x1e16,
              kFontRed = 0x1e19;

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
    case co::Font::Gold:
    case co::Font::Accent: // one set of fonts for the light and the dark ground (white and gold on brown)
    case co::Font::Value: return Id(kFontGold);
    case co::Font::Title: return Id(kFontTitle);
    case co::Font::Small: return Id(kFontSmall);
    case co::Font::Disabled: return Id(kFontRed);
    default: return Id(kFontNormal);
    }
}

class GamePainter : public co::Painter {
public:
    int ox = 0, oy = 0;

    // The tile layouts are core/display/tile_layout's (shared with Pericles and framed_screens).
    void Nine(int group, const tl::Nine &n, int x, int y, int w, int h)
    {
        const int base = Id(group);
        tl::WalkNine(n, ox + x, oy + y, w, h, true, [&](int i, int xx, int yy) { Image(base + i, xx, yy); });
    }
    void Panel(int x, int y, int w, int h) override { Nine(kPanel, tl::kPanel, x, y, w, h); }
    void Inset(int x, int y, int w, int h) override { Nine(kInset, tl::kInset, x, y, w, h); }
    void Pieces(int base, int x, int y, int w) // left, middle..., right
    {
        tl::WalkPieces(ox + x, oy + y, w, [&](int i, int xx, int yy) { Image(base + i, xx, yy); });
    }
    void ButtonBar(int x, int y, int w, int style) override { Pieces(Id(kButton) + style * 3, x, y, w); }
    void Strip(int x, int y, int w) override { Pieces(Id(kStrip) + kStripFirst, x, y, w); }
    void Frame(int x, int y, int w, int h) override // button_frame_draw 0x5bf360, the darker set (+ 8)
    {
        const int b = Id(kFrame) + 8;
        tl::WalkFrame(ox + x, oy + y, w, h, [&](int i, int xx, int yy) { Image(b + i, xx, yy); });
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
        const int b = Id(kLine);
        tl::WalkLine(ox + x, oy + y, w, [&](int i, int xx, int yy) { Image(b + i, xx, yy); });
    }
    // y is the top of the letters (as in the mockups); text_draw puts them 3 pixels higher than that (measured on the
    // screenshots: every font, every row 3 pixels above the mockup).
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

std::vector<co::Page> BuildPages()
{
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
        if (!sr::Dry()) go::SpeedRows(c.rows);
        Add(c.rows, "gameplay", "keep_speed");
        Add(c.rows, "gameplay", "full_speed");
        Header(c.rows, "Scrolling");
        Add(c.rows, "gameplay", "simulate_while_scrolling");
        Add(c.rows, "gameplay", "smooth_scroll");
        Add(c.rows, "gameplay", "drag_scroll");
        Add(c.rows, "gameplay", "edge_scroll_width");
        general.categories.push_back(std::move(c));
    }
    {
        // Only while a city is played by one player (games/emperor/options_original's InCity, SinglePlayer): in the
        // menus the next mission's choice overrides it, the briefing would reset the starting money, and a
        // multiplayer game keeps both over the network.
        co::Category c{ot::kCategoryDifficulty, {}};
        if (!sr::Dry()) go::DifficultyRows(c.rows);
        if (!c.rows.empty()) {
            general.categories.push_back(std::move(c));
        }
    }
    {
        co::Category c{"Autosave", {}};
        if (!sr::Dry()) go::AutosaveRows(c.rows);
        if (!c.rows.empty()) {
            general.categories.push_back(std::move(c));
        }
    }
    {
        co::Category c{ot::kCategoryMessages, {}};
        if (!sr::Dry()) go::MessageRows(c.rows);
        if (!c.rows.empty()) {
            general.categories.push_back(std::move(c));
        }
    }
    co::Page display{"Video", {}};
    {
        co::Category c{"Display", {}}; // once "Screen" (Display, as modern games call it)
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
    if (!sr::Dry()) go::SoundRows(sound.categories[0].rows);
    if (!sound.categories[0].rows.empty()) {
        Header(sound.categories[0].rows, "More");
    }
    Add(sound.categories[0].rows, "audio", "ambient_sounds");
    Add(sound.categories[0].rows, "audio", "nature_sounds");
    Add(sound.categories[0].rows, "audio", "nature_sounds_gap");
    Add(sound.categories[0].rows, "audio", "background_sound");
    std::vector<co::Page> pages{std::move(general), std::move(display), std::move(sound)};
    for (auto &p : pages) {
        Resolve(p);
    }
    return pages;
}

// ---------------------------------------------------------------------------------------------------------------
// Opening, input, drawing

// The Hotkeys window (core/options/hotkeys_page): the overlays' names are the game's texts (EmperorText.eng groups 14
// and 59), "See Water" as "Water".
std::string OverlayName(int group, int index)
{
    return core::options::hotkeys_page::OverlayWord(oo::GameText(group, index));
}

const core::options::hotkeys_page::KeyPageGame kKeyPageGame(&OverlayName);

std::vector<co::Page> BuildKeyPages()
{
    return {core::options::hotkeys_page::Build(kKeyPageGame)};
}

GamePainter g_painter;

// Keys nobody may choose: Enter (the game's, not offered), the game's keys for
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

// The game's texts (options_original, lang_get_string): the pause menu shows the File menu's own Load / Save /
// Delete / Exit game.
std::string Lang(int group, int index)
{
    return oo::GameText(group, index);
}

// The pause menu's items, in order.
enum PauseItem { kContinue, kReplay, kLoad, kSave, kDelete, kOptions, kMainMenu, kExit };

void PauseAct(int item)
{
    using emperor::game_menu::Item;
    using emperor::game_menu::Request;
    switch (item) {
    case kContinue: pm::Close("Continue"); break;
    case kReplay: pm::Close("Replay map"); Request(Item::Replay); break;
    case kLoad: pm::Close("Load game"); Request(Item::Load); break;
    case kSave: pm::Close("Save game"); Request(Item::Save); break;
    case kDelete: pm::Close("Delete game"); Request(Item::Delete); break;
    case kOptions: oh::Open(kPageGeneral, 0); break; // over the pause menu; closing it shows the pause menu again
    case kMainMenu: pm::Close("Exit to main menu"); Request(Item::MainMenu); break;
    case kExit: pm::Close("Exit game"); Request(Item::Exit); break;
    default: break;
    }
}

// The pause menu's game (core/options/pause_menu.h).
void PauseOrigin(int *x, int *y)
{
    *x = *kDialogX;
    *y = *kDialogY;
}

void PauseBeginDraw()
{
    Function<Void_fn>(kClipReset)();
    g_painter.ox = *kDialogX;
    g_painter.oy = *kDialogY;
}

void Redraw()
{
    *kRedraw = 1;
}

void ClickSound()
{
    Function<UiSound_fn>(kUiSound)(kClickSound);
}

bool CanDraw(unsigned long caller)
{
    return caller != kPresentFromPaint && *kDrawBuffer != nullptr;
}

// The pause menu's look: Augustus's layout; the button under the mouse drawn with the game's darker bar (button bar
// style 1: images 208-210 of China_Interface, the pressed look) instead of the frame, which belongs to the main menu's
// buttons (as in Pericles).
co::MenuLook PauseLook()
{
    co::MenuLook l;
    l.hoverBarStyle = 1;
    l.hoverFrame = false;
    return l;
}

const pm::PauseGame kPauseGame("emperor", g_painter, PauseLook(), &PauseOrigin, &PauseBeginDraw, &Redraw, &ClickSound,
                               &CanDraw, &PauseAct, &oh::PauseOptionsKey);

// The debug panel keeps the settings to change there when something goes wrong: the screen and the panel's own key.
const char *const kEmergency[][2] = {{"video", "resolution"}, {"video", "scale"},   {"video", "window"},
                                     {"video", "renderer"},   {"video", "scaling"}, {"ui", "panel_key"}};
const oh::OptionsGame kOptionsGame(kPauseGame, ot::kOptionsTitle, &BuildPages, &BuildKeyPages, &ReservedKey, kEmergency,
                                   static_cast<int>(sizeof(kEmergency) / sizeof(kEmergency[0])), true, &GameFrame);

} // namespace

bool Available()
{
    return oh::Available();
}

bool Open(int page, int category)
{
    return oh::Open(page, category);
}

bool OpenCategory(int page, const char *name)
{
    return oh::OpenCategory(page, name);
}

bool OpenHotkeys()
{
    return oh::OpenHotkeys();
}

bool OpenPause()
{
    // The pause menu's own state: it stays after an error switches the options window off (its Options item then
    // only logs that the window is not available).
    if (!pm::Available()) {
        return false;
    }
    if (oh::IsOpen()) {
        return true;
    }
    return pm::Open(ot::kPauseTitle, {ot::kPauseContinue, ot::kMenuReplayMap, Lang(1, 3), Lang(1, 4), Lang(1, 5),
                                      ot::kPauseOptions, ot::kMenuExitToMainMenu, Lang(1, 6)});
}

bool IsOpen()
{
    return oh::IsOpen();
}

void OnPresent(unsigned long caller)
{
    oh::OnPresent(caller);
}

std::vector<co::Page> KeyPagesForTest()
{
    return BuildKeyPages();
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
    ot::Register(); // Yongle's own texts, before the shared ones
    oh::Attach(kOptionsGame);
    return true;
}

} // namespace emperor::options_window
