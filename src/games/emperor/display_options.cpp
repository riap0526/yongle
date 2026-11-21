#include "games/emperor/display_options.h"

#include "core/log.h"
#include "core/options/display_dialog.h"
#include "games/emperor/site.h"

#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): the dialog's table and drawing read from the disassembly.
namespace emperor::display_options {
namespace {

namespace dd = core::options::display_dialog;

// ---------------------------------------------------------------------------------------------
// Addresses (calling conventions checked by disassembly)

const Site kDraw{"display_options_draw", 0x5ba9c0, {0x56, 0x68, 0x14, 0x1e, 0x00, 0x00, 0xb9, 0x30}};
const Site kPanelDraw{"panel_draw", 0x5363f0, {0x51, 0xa1, 0x1c, 0xf5, 0x15, 0x01, 0x85, 0xc0}};
const Site kButtonFrame{"menu_button_frame_draw", 0x536180, {0xa1, 0x1c, 0xf5, 0x15, 0x01, 0x85, 0xc0, 0x75}};
const Site kLangTextBox{"lang_text_draw_in_box", 0x535f50, {0xa1, 0x1c, 0xf5, 0x15, 0x01, 0x85, 0xc0, 0x75}};
const Site kTextCentered{"text_draw_centered", 0x5282a0, {0x8a, 0x44, 0x24, 0x1c, 0x84, 0xc0, 0x74, 0x12}};
const Site kImageGetId{"image_get_id", 0x408170, {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x8b, 0xc7}};
// Hooked by games/emperor/resolution (which checked these bytes before hooking; we are only attached after it), so
// its first bytes are a jump by now and are not checked here; calls go through that detour, as the game's own do.
const Site kImageDraw{"image_draw", 0x413960, {0x8b, 0x44, 0x24, 0x18, 0x8b, 0x4c, 0x24, 0x14}};
const Site kClipReset{"screen_clip_reset", 0x5bf860, {0x8b, 0x0d, 0x1c, 0xe0, 0x0d, 0x01, 0x8b, 0x15}};
const Site kScreenDraw{"screen_draw", 0x537690, {0x33, 0xc0, 0x53, 0xa0, 0x45, 0xe0, 0x0d, 0x01}};
const Site kScreenBackground{"screen_draw_background", 0x5371f0, {0x8b, 0x44, 0x24, 0x04, 0xb1, 0x01, 0x85, 0xc0}};
const Site kCancel{"display_options_cancel", 0x402010, {0xa0, 0x54, 0xf5, 0x15, 0x01, 0xc6, 0x05, 0x36}};

using Void_fn = void(__cdecl *)();
using PanelDraw_fn = void(__cdecl *)(int x, int y, int cols, int rows, int ox, int oy);
using ButtonFrame_fn = void(__cdecl *)(int kind, int x, int y, int units, int heightUnits, int state, int ox, int oy);
using LangTextBox_fn = void(__cdecl *)(int group, int index, int x, int y, int width, int font, int color, int ox,
                                       int oy);
using TextCentered_fn = void(__cdecl *)(const char *text, int x, int y, int width, int font, int color, int flag);
using ImageGetId_fn = int(__thiscall *)(void *, int);
using ImageDraw_fn = void(__cdecl *)(int id, int x, int y, int, int, int);
using ScreenBackground_fn = void(__cdecl *)(int force);

// The three places in screen_handle_input 0x538160 (case 0x0a) that pass the button table: push 4; push 0x81ba50
// before 0x44a830 (which button the mouse is over), 0x536580 (its index -> 0xa599fc) and 0x44a710 (clicks).
constexpr DWORD kTablePushes[] = {0x538acf, 0x538aec, 0x538b0a};
const BYTE kTablePushBytes[7] = {0x6a, 0x04, 0x68, 0x50, 0xba, 0x81, 0x00};

const Var<int> kDialogX{0x10de014}; // dialog_offset_x
const Var<int> kDialogY{0x10de010};
const Var<int> kHover{0xa599fc};    // the dialog's button under the mouse (1-based, 0 none)
const Var<int> kHover2{0xa599f8};   // screen_draw keeps both around its drawing
const Var<char> kGameScreen{0x10de045};
const Var<int> kReturnScreen{0x115f554}; // the screen the dialog goes back to (display_options_cancel)
const Var<int> kScreenWidth{0x10de01c};
const Var<int> kScreenHeight{0x10de018};
const Var<int> kDrawDone{0x13f8dc0}; // display_options_draw sets it to 0x32 (opening sets 0); nothing reads it
const DWORD kImages = 0x1c42130;     // image_manager
constexpr char kScreenDisplayOptions = 0x0a;
constexpr int kFontTitle = 0x1e1c, kFontButton = 0x1e14;
constexpr int kColorHover = 0xfac1, kColorNormal = 0x02bf; // display_options_draw's two text colours
// The volume arrows of Sound options (its image buttons, table 0x81df40, drawn by image_buttons_draw 0x449b40): 24 x 24,
// the next image is the pressed one. 0x218 points down, 0x217 up (Sound options: down on the left, up on the right).
constexpr int kGroupArrowDown = 0x218, kGroupArrowUp = 0x217;

// The game's button record (display_options_buttons 0x81ba50): the rectangle (x1, y1, x2, y2,
// exclusive) relative to dialog_offset; 0x44a710 calls onRelease when the mouse is let go on the button it went
// down on, onClick on a click event (the original's is the empty 0x40fc20); kind 1 = left button; +0x11 is set
// when pressed.
struct GameButton {
    short x1, y1, x2, y2;
    void(__cdecl *onRelease)();
    void(__cdecl *onClick)();
    BYTE unknown10, pressed, unknown12, kind;
    int param;
    int param2;
};
static_assert(sizeof(GameButton) == 28, "the game's button record");

GameButton g_buttons[dd::kItemCount];
bool g_installed = false;

void __cdecl NoClick()
{
}

void BuildTable()
{
    for (int i = 0; i < dd::kItemCount; ++i) {
        const dd::Item &it = dd::Items()[i];
        GameButton &b = g_buttons[i];
        b = GameButton{};
        b.x1 = static_cast<short>(it.x);
        b.y1 = static_cast<short>(it.y);
        b.x2 = static_cast<short>(it.x + it.w);
        b.y2 = static_cast<short>(it.y + it.h);
        b.onRelease = dd::HandlerOf(it.id);
        b.onClick = &NoClick;
        b.kind = 1;
        b.param = it.id;
    }
}

bool PatchTable(bool ours)
{
    BYTE bytes[7];
    std::memcpy(bytes, kTablePushBytes, sizeof(bytes));
    if (ours) {
        bytes[1] = static_cast<BYTE>(dd::kItemCount);
        const DWORD table = reinterpret_cast<DWORD>(g_buttons);
        std::memcpy(bytes + 3, &table, 4);
    }
    bool ok = true;
    for (DWORD at : kTablePushes) {
        ok = core::site::PatchCode(at, bytes, sizeof(bytes)) && ok;
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------
// The core's dialog with the game's own drawing functions (game thread, from screen_draw)

int ImageId(int group)
{
    return Function<ImageGetId_fn>(kImageGetId)(reinterpret_cast<void *>(kImages), group);
}

void Close()
{
    Function<Void_fn>(kCancel)(); // screen_redraw = 1; game_screen = the screen it came from
}

bool DialogShown()
{
    return *kGameScreen == kScreenDisplayOptions;
}

void ScreenSize(int *w, int *h)
{
    *w = *kScreenWidth;
    *h = *kScreenHeight;
}

int Hover()
{
    return *kHover;
}

// The city behind the dialog drawn again, as the screen it came from draws it (screen_draw_background, then
// screen_draw).
bool DrawBehind()
{
    const char back = static_cast<char>(*kReturnScreen);
    if (back != 0x01 && back != 0x08 && back != 0x20 && back != 0x2d && back != 0x48 && back != 0x49) {
        return true; // not a city screen: left as it is
    }
    const int hover = *kHover, hover2 = *kHover2;
    *kGameScreen = back;
    Function<ScreenBackground_fn>(kScreenBackground)(1);
    Function<Void_fn>(kScreenDraw)();
    *kGameScreen = kScreenDisplayOptions;
    *kHover = hover;
    *kHover2 = hover2;
    return true;
}

void Panel(int x, int y, int cols, int rows)
{
    const int ox = *kDialogX, oy = *kDialogY;
    Function<PanelDraw_fn>(kPanelDraw)(ox + x, oy + y, cols, rows, ox, oy);
}

void Button(const dd::Item &it, bool hovered)
{
    const int ox = *kDialogX, oy = *kDialogY;
    Function<ButtonFrame_fn>(kButtonFrame)(2, ox + it.x, oy + it.y, it.w / 16, 0, hovered ? 1 : 2, ox, oy);
}

void ArrowImage(const dd::Item &it, bool pressed)
{
    const int image = ImageId(it.arrow == dd::Arrow::Down ? kGroupArrowDown : kGroupArrowUp) + (pressed ? 1 : 0);
    Function<ImageDraw_fn>(kImageDraw)(image, *kDialogX + it.x, *kDialogY + it.y, 0, 0, 0);
}

int Color(dd::TextLook look)
{
    return look == dd::TextLook::ButtonHover ? kColorHover : look == dd::TextLook::Button ? kColorNormal : 0;
}

void GameTextBox(dd::GameText t, int x, int y, int width, dd::TextLook look)
{
    const int ox = *kDialogX, oy = *kDialogY;
    const bool title = t == dd::GameText::Title;
    Function<LangTextBox_fn>(kLangTextBox)(dd::kTextGroup, dd::kTextIndex[static_cast<int>(t)], ox + x, oy + y, width,
                                           ImageId(title ? kFontTitle : kFontButton), Color(look), ox, oy);
}

void Text(const char *s, int x, int y, int width, dd::TextLook look)
{
    Function<TextCentered_fn>(kTextCentered)(s, *kDialogX + x, *kDialogY + y, width, ImageId(kFontButton),
                                             Color(look), 0);
}

void EndDraw()
{
    Function<Void_fn>(kClipReset)();
    *kDrawDone = 0x32;
}

const dd::Game kGame(&Close, &DialogShown, &ScreenSize, &Hover, &DrawBehind, &Panel, &Button, &ArrowImage,
                     &GameTextBox, &Text, &EndDraw);

using Draw_fn = void(__cdecl *)();
Draw_fn o_Draw;

void Fallback(const char *why)
{
    if (g_installed) {
        g_installed = false;
        PatchTable(false);
        core_log("display options: the game's own dialog from now on (%s)", why);
    }
}

void __cdecl Draw_detour()
{
    if (g_installed && dd::Working()) {
        if (dd::Draw()) {
            return;
        }
        Fallback("drawing failed");
    }
    else if (g_installed) {
        Fallback("switched off after an error");
    }
    Original(o_Draw, kDraw)();
}

} // namespace

void OnLoop()
{
    dd::OnLoop();
}

bool Attach()
{
    const Site *sites[] = {&kDraw, &kPanelDraw, &kButtonFrame, &kLangTextBox, &kTextCentered,
                           &kImageGetId, &kClipReset, &kScreenDraw, &kScreenBackground, &kCancel};
    for (const Site *s : sites) {
        if (!Matches(*s)) {
            core_log("display options: %s at 0x%lx does not match; the game's own dialog stays", s->name, s->address);
            return false;
        }
    }
    for (DWORD at : kTablePushes) {
        if (std::memcmp(reinterpret_cast<const void *>(at), kTablePushBytes, sizeof(kTablePushBytes)) != 0) {
            core_log("display options: the button table at 0x%lx is not passed as expected; the game's own dialog "
                     "stays",
                     at);
            return false;
        }
    }
    BuildTable();
    if (!Install(kDraw, &Draw_detour, o_Draw)) {
        core_log("display options: display_options_draw could not be hooked; the game's own dialog stays");
        return false;
    }
    dd::Attach(kGame);
    g_installed = true;
    if (!PatchTable(true)) {
        Fallback("the button table could not be replaced");
        return false;
    }
    core_log("display options: installed (window, 1024x768, display scale arrows, 100%% and Auto, scaling, OK, "
             "Cancel)");
    return true;
}

} // namespace emperor::display_options
