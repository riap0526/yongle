#include "games/emperor/display_options.h"

#include "core/diag/guard.h"
#include "core/display/logical_size.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "games/emperor/site.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/archive/resolution_ui_r3.md §1.
namespace emperor::display_options {
namespace {

// ---------------------------------------------------------------------------------------------
// Addresses (calling conventions: docs/symbols.md)

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
constexpr int kGroupText = 42;                    // EmperorText.eng: 0 title, 1 Full screen, 2 Windowed screen,
constexpr int kTextFullScreen = 1, kTextWindowed = 2, kText1024 = 4, kTextCancel = 5; // 3 800x600, 5 Cancel
constexpr int kFontTitle = 0x1e1c, kFontButton = 0x1e14;
constexpr int kColorHover = 0xfac1, kColorNormal = 0x02bf; // display_options_draw's two text colours
// The volume arrows of Sound options (its image buttons, table 0x81df40, drawn by buttons_draw 0x449b40): 24 x 24,
// the next image is the pressed one. 0x218 points down, 0x217 up (Sound options: down on the left, up on the right).
constexpr int kGroupArrowDown = 0x218, kGroupArrowUp = 0x217, kArrowSize = 24;
constexpr DWORD kArrowPressedMs = 120; // the pressed image after a click

// ---------------------------------------------------------------------------------------------
// Layout, relative to dialog_offset (the original: panel at (0x60, 0x50), 18 x 12 blocks; buttons 14 blocks wide at
// x 0x80, y 0x88 + 0x18 n; their text 4 px lower; title at y 0x5e). The scale line is laid out as a Sound options
// volume line: the text 6 px below the arrows' top.
constexpr int kPanelX = 0x60, kPanelY = 0x50, kPanelCols = 18, kPanelRows = 16;
constexpr int kLeft = 0x80, kWidth = 0xe0, kTitleY = 0x5e;
constexpr int kButtonH = 20, kTextDown = 4, kArrowTextDown = 6;
constexpr int kRowWindow = 0x88, kRow1024 = 0xa0, kStatus1 = 0xbe, kRowScale = 0xd4, kRowAuto = 0xf6, kRowEnd = 0x11a;
constexpr int kScaleTextWidth = kWidth - 2 * kArrowSize - 8; // the scale text, left of the arrows

enum Id { kWindowButton = 1, k1024Button, kMinus, kPlus, k100, kAuto, kOk, kCancelButton, kCount = kCancelButton };

struct Layout {
    int id, x, y, units; // units of 16 px (text buttons)
    int arrow;           // an arrow instead: its image group
};
const Layout kLayout[kCount] = {
    {kWindowButton, kLeft, kRowWindow, 14, 0},
    {k1024Button, kLeft, kRow1024, 14, 0},
    {kMinus, kLeft + kWidth - 2 * kArrowSize, kRowScale, 0, kGroupArrowDown},
    {kPlus, kLeft + kWidth - kArrowSize, kRowScale, 0, kGroupArrowUp},
    {k100, kLeft, kRowAuto, 6, 0},
    {kAuto, kLeft + 128, kRowAuto, 6, 0},
    {kOk, kLeft, kRowEnd, 6, 0},
    {kCancelButton, kLeft + 128, kRowEnd, 6, 0},
};

int g_pressed = 0; // the arrow shown pressed for a moment after a click
DWORD g_pressedAt = 0;

// The game's button record (display_options_buttons 0x81ba50; docs/symbols.md): the rectangle (x1, y1, x2, y2,
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

GameButton g_buttons[kCount];

core::diag::Feature g_feature{"Display options dialog (games/emperor/display_options)"};
bool g_installed = false;

// The dialog as it was opened: what Cancel (or closing it otherwise) puts back.
struct Session {
    bool open = false;
    bool changed = false; // resolution or scale changed in it (not saved yet)
    char resolution[core::settings::kValueSize] = {};
    char scale[core::settings::kValueSize] = {};
    int drawnW = 0, drawnH = 0; // the screen size the picture behind the dialog was drawn for
};
Session g_session;

int Item(const char *key)
{
    return core::settings::Find("display", key);
}

void Value(const char *key, char *out)
{
    core::settings::Value(Item(key), out, core::settings::kValueSize);
}

bool Scaled()
{
    char v[core::settings::kValueSize];
    Value("resolution", v);
    return std::strcmp(v, "scaled") == 0;
}

int ScaleSetting() // 0 = auto
{
    char v[core::settings::kValueSize];
    Value("scale", v);
    return std::strcmp(v, "auto") == 0 ? 0 : std::atoi(v);
}

void Minimum(int *w, int *h)
{
    core::display::GameMinimum(w, h);
    if (*w <= 0 || *h <= 0) {
        *w = 1024;
        *h = 768;
    }
}

int EffectiveScale(int scale)
{
    SIZE n = core::display::NativeSize();
    int mw, mh;
    Minimum(&mw, &mh);
    return core::display::ChooseLogicalSize(n.cx, n.cy, CORE_RESOLUTION_SCALED, 0, 0, scale, mw, mh).scale_percent;
}

// ---------------------------------------------------------------------------------------------
// Buttons (game thread, from screen_handle_input)

void Close()
{
    Function<Void_fn>(kCancel)(); // screen_redraw = 1; game_screen = the screen it came from
}

void SetDisplay(const char *resolution, const char *scale)
{
    if (resolution) {
        core::settings::Request(Item("resolution"), resolution, false);
    }
    if (scale) {
        core::settings::Request(Item("scale"), scale, false);
    }
    g_session.changed = true;
}

void PutBack(const char *why)
{
    if (!g_session.changed) {
        return;
    }
    core_log("display options: %s: resolution %s, scale %s put back", why, g_session.resolution, g_session.scale);
    core::settings::Request(Item("resolution"), g_session.resolution, true);
    core::settings::Request(Item("scale"), g_session.scale, true);
    g_session.changed = false;
}

void Press(int id)
{
    char v[16];
    switch (id) {
    case kWindowButton: {
        const bool to = !core::display::Windowed();
        core_log("display options: %s", to ? "windowed" : "fullscreen");
        core::settings::Request(Item("window"), to ? "windowed" : "fullscreen", true); // as F5: saved at once
        break;
    }
    case k1024Button:
        core_log("display options: 1024x768");
        SetDisplay("1024x768", nullptr);
        break;
    case kMinus:
    case kPlus: {
        g_pressed = id;
        g_pressedAt = GetTickCount();
        int mw, mh;
        Minimum(&mw, &mh);
        SIZE n = core::display::NativeSize();
        const int current = ScaleSetting();
        if (!Scaled()) { // from a fixed size: back to the scale that was set
            std::snprintf(v, sizeof(v), "%d", current);
            core_log("display options: %s -> scaled (scale %s)", id == kPlus ? "+" : "-", current ? v : "auto");
            SetDisplay("scaled", nullptr);
            break;
        }
        const int next = StepScale(current, id == kPlus ? 1 : -1, n.cx, n.cy, mw, mh);
        if (next == EffectiveScale(current) && current != 0) {
            core_log("display options: %s: the scale stays %d%% (100..%d%% here)", id == kPlus ? "+" : "-", next,
                     MaxScale(n.cx, n.cy, mw, mh));
            break;
        }
        std::snprintf(v, sizeof(v), "%d", next);
        core_log("display options: scale %s%%", v);
        SetDisplay("scaled", v);
        break;
    }
    case k100:
        core_log("display options: scale 100%%");
        SetDisplay("scaled", "100");
        break;
    case kAuto:
        core_log("display options: scale auto");
        SetDisplay("scaled", "auto");
        break;
    case kOk:
        core_log("display options: OK%s", g_session.changed ? " (resolution and scale saved)" : "");
        if (g_session.changed) {
            core::settings::RequestSave(); // after the requests before it: the values they set are confirmed
            g_session.changed = false;
        }
        Close();
        break;
    case kCancelButton:
        PutBack("Cancel");
        Close();
        break;
    }
}

template <int N> void __cdecl OnButton()
{
    int id = N;
    if (!core::diag::Guarded(g_feature, [](void *p) { Press(*static_cast<int *>(p)); }, &id)) {
        core_log("display options: button %d abandoned after an error", N);
    }
}

void __cdecl NoClick()
{
}

void (__cdecl *const kHandlers[kCount])() = {&OnButton<1>, &OnButton<2>, &OnButton<3>, &OnButton<4>,
                                              &OnButton<5>, &OnButton<6>, &OnButton<7>, &OnButton<8>};

void BuildTable()
{
    for (int i = 0; i < kCount; ++i) {
        const Layout &l = kLayout[i];
        GameButton &b = g_buttons[i];
        b = GameButton{};
        b.x1 = static_cast<short>(l.x);
        b.y1 = static_cast<short>(l.y);
        b.x2 = static_cast<short>(l.x + (l.arrow ? kArrowSize : l.units * 16));
        b.y2 = static_cast<short>(l.y + (l.arrow ? kArrowSize : kButtonH));
        b.onRelease = kHandlers[i];
        b.onClick = &NoClick;
        b.kind = 1;
        b.param = l.id;
    }
}

bool PatchTable(bool ours)
{
    BYTE bytes[7];
    std::memcpy(bytes, kTablePushBytes, sizeof(bytes));
    if (ours) {
        bytes[1] = static_cast<BYTE>(kCount);
        const DWORD table = reinterpret_cast<DWORD>(g_buttons);
        std::memcpy(bytes + 3, &table, 4);
    }
    bool ok = true;
    for (DWORD at : kTablePushes) {
        ok = PatchCode(at, bytes, sizeof(bytes)) && ok;
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Drawing (game thread, from screen_draw)

int ImageId(int group)
{
    return Function<ImageGetId_fn>(kImageGetId)(reinterpret_cast<void *>(kImages), group);
}

// After the game was laid out again (a new scale, or the window was sized) the draw buffer is new: the city behind
// the dialog is drawn again, as the screen it came from draws it (screen_draw_background, then screen_draw).
void DrawBehind()
{
    const char back = static_cast<char>(*kReturnScreen);
    if (back != 0x01 && back != 0x08 && back != 0x20 && back != 0x2d && back != 0x48 && back != 0x49) {
        return; // not a city screen: left as it is
    }
    const int hover = *kHover, hover2 = *kHover2;
    *kGameScreen = back;
    Function<ScreenBackground_fn>(kScreenBackground)(1);
    Function<Void_fn>(kScreenDraw)();
    *kGameScreen = kScreenDisplayOptions;
    *kHover = hover;
    *kHover2 = hover2;
}

void Draw()
{
    const int w = *kScreenWidth, h = *kScreenHeight;
    if (!g_session.open) {
        g_session = Session{};
        g_session.open = true;
        Value("resolution", g_session.resolution);
        Value("scale", g_session.scale);
        g_session.drawnW = w;
        g_session.drawnH = h;
        core_log("display options: opened (resolution %s, scale %s, %dx%d)", g_session.resolution, g_session.scale,
                 w, h);
    }
    else if (w != g_session.drawnW || h != g_session.drawnH) {
        g_session.drawnW = w;
        g_session.drawnH = h;
        DrawBehind();
    }
    const int ox = *kDialogX, oy = *kDialogY, hover = *kHover;
    const int font = ImageId(kFontButton);
    Function<PanelDraw_fn>(kPanelDraw)(ox + kPanelX, oy + kPanelY, kPanelCols, kPanelRows, ox, oy);
    const bool pressedShown = g_pressed && GetTickCount() - g_pressedAt < kArrowPressedMs;
    for (const Layout &l : kLayout) {
        if (l.arrow) {
            const int image = ImageId(l.arrow) + (pressedShown && g_pressed == l.id ? 1 : 0);
            Function<ImageDraw_fn>(kImageDraw)(image, ox + l.x, oy + l.y, 0, 0, 0);
        }
        else {
            Function<ButtonFrame_fn>(kButtonFrame)(2, ox + l.x, oy + l.y, l.units, 0, hover == l.id ? 1 : 2, ox, oy);
        }
    }
    Function<LangTextBox_fn>(kLangTextBox)(kGroupText, 0, ox + kLeft, oy + kTitleY, kWidth, ImageId(kFontTitle), 0, ox,
                                           oy);
    auto color = [hover](int id) { return hover == id ? kColorHover : kColorNormal; };
    auto langText = [&](int index, const Layout &l) {
        Function<LangTextBox_fn>(kLangTextBox)(kGroupText, index, ox + l.x, oy + l.y + kTextDown, l.units * 16, font,
                                               color(l.id), ox, oy);
    };
    auto text = [&](const char *s, int x, int y, int width, int c) {
        Function<TextCentered_fn>(kTextCentered)(s, ox + x, oy + y, width, font, c, 0);
    };
    langText(core::display::Windowed() ? kTextFullScreen : kTextWindowed, kLayout[0]);
    langText(kText1024, kLayout[1]);
    static const char *const kOwnText[] = {"100%", "Auto", "OK"}; // k100, kAuto, kOk
    for (int i = 0; i < 3; ++i) {
        const Layout &l = kLayout[k100 - 1 + i];
        text(kOwnText[i], l.x, l.y + kTextDown, l.units * 16, color(l.id));
    }
    langText(kTextCancel, kLayout[kCancelButton - 1]);
    char line1[48], line2[48];
    const int scale = ScaleSetting();
    StatusLines(w, h, Scaled(), scale, EffectiveScale(scale), line1, sizeof(line1), line2, sizeof(line2));
    text(line1, kLeft, kStatus1, kWidth, 0);
    text(line2, kLeft, kRowScale + kArrowTextDown, kScaleTextWidth, 0);
    Function<Void_fn>(kClipReset)();
    *kDrawDone = 0x32;
}

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
    if (g_installed && core::diag::Enabled(g_feature)) {
        if (core::diag::Guarded(g_feature, [](void *) { Draw(); }, nullptr)) {
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

int MaxScale(int nativeW, int nativeH, int minW, int minH)
{
    for (int s = 250; s > 100; s -= 5) {
        if (!core::display::ChooseLogicalSize(nativeW, nativeH, CORE_RESOLUTION_SCALED, 0, 0, s, minW, minH)
                 .limited) {
            return s;
        }
    }
    return 100;
}

int StepScale(int current, int dir, int nativeW, int nativeH, int minW, int minH)
{
    const int from =
        core::display::ChooseLogicalSize(nativeW, nativeH, CORE_RESOLUTION_SCALED, 0, 0, current, minW, minH)
            .scale_percent;
    int next = dir > 0 ? (from / 5 + 1) * 5 : ((from + 4) / 5 - 1) * 5; // the next 5 % step (auto may be 133 %)
    const int top = MaxScale(nativeW, nativeH, minW, minH);
    next = next > top ? top : next;
    next = next < 100 ? 100 : next;
    return next;
}

void StatusLines(int screenW, int screenH, bool scaled, int scale, int effectiveScale, char *line1, int size1,
                 char *line2, int size2)
{
    std::snprintf(line1, size1, "Resolution: %d x %d", screenW, screenH);
    if (!scaled) {
        std::snprintf(line2, size2, "Display scale: off");
    }
    else if (scale == 0) {
        std::snprintf(line2, size2, "Display scale: Auto (%d%%)", effectiveScale);
    }
    else {
        std::snprintf(line2, size2, "Display scale: %d%%", effectiveScale);
    }
}

void OnLoop()
{
    if (!g_session.open || *kGameScreen == kScreenDisplayOptions) {
        return;
    }
    g_session.open = false;
    PutBack("closed without OK");
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
    g_installed = true;
    if (!PatchTable(true)) {
        Fallback("the button table could not be replaced");
        return false;
    }
    core_log("display options: installed (window, 1024x768, display scale arrows, 100%% and Auto, OK, Cancel)");
    return true;
}

} // namespace emperor::display_options
