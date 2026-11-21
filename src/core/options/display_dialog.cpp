#include "core/options/display_dialog.h"

#include "core/diag/guard.h"
#include "core/display/logical_size.h"
#include "core/display/virtual_display.h"
#include "core/game.h"
#include "core/log.h"
#include "core/settings/settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core::options::display_dialog {
namespace {

constexpr int kRowWindow = 0x88, kRowNative = 0xa0, kRowScale = 0xd4, kRowAuto = 0xf6;
constexpr int kRowScaling = 0x112, kRowEnd = 0x13e;          // the scaling line between Auto and OK
constexpr int kScaleTextWidth = kWidth - 2 * kArrowSize - 8; // the scale text, left of the arrows
constexpr unsigned long kArrowPressedMs = 120;               // the pressed image after a click

const Item kItems[kItemCount] = {
    {kWindowButton, kLeft, kRowWindow, 14 * 16, kButtonH, Arrow::None},
    {kNativeButton, kLeft, kRowNative, 14 * 16, kButtonH, Arrow::None},
    {kMinus, kLeft + kWidth - 2 * kArrowSize, kRowScale, kArrowSize, kArrowSize, Arrow::Down},
    {kPlus, kLeft + kWidth - kArrowSize, kRowScale, kArrowSize, kArrowSize, Arrow::Up},
    {k100, kLeft, kRowAuto, 6 * 16, kButtonH, Arrow::None},
    {kAuto, kLeft + 128, kRowAuto, 6 * 16, kButtonH, Arrow::None},
    {kScalingButton, kLeft, kRowScaling, 14 * 16, kButtonH, Arrow::None},
    {kOk, kLeft, kRowEnd, 6 * 16, kButtonH, Arrow::None},
    {kCancelButton, kLeft + 128, kRowEnd, 6 * 16, kButtonH, Arrow::None},
};

const Game *g_game = nullptr;
core::diag::Feature g_feature{"Display options dialog (core/options/display_dialog)"};

int g_pressed = 0; // the arrow shown pressed for a moment after a click
unsigned long g_pressedAt = 0;

// The dialog as it was opened: what Cancel (or closing it otherwise) puts back.
struct Session {
    bool open = false;
    bool changed = false; // resolution or scale changed in it (not saved yet)
    char resolution[core::settings::kValueSize] = {};
    char scale[core::settings::kValueSize] = {};
    int drawnW = 0, drawnH = 0; // the screen size the picture behind the dialog was drawn for
    bool behindDue = false;     // laid out again: the screen behind still to be drawn
};
Session g_session;

int Setting(const char *key)
{
    return core::settings::Find("video", key);
}

void Value(const char *key, char *out)
{
    core::settings::Value(Setting(key), out, core::settings::kValueSize);
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
        *w = core_game()->native_width;
        *h = core_game()->native_height;
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
// Buttons (game thread, from the game's button handling)

void SetDisplay(const char *resolution, const char *scale)
{
    if (resolution) {
        core::settings::Request(Setting("resolution"), resolution, false);
    }
    if (scale) {
        core::settings::Request(Setting("scale"), scale, false);
    }
    g_session.changed = true;
}

void PutBack(const char *why)
{
    if (!g_session.changed) {
        return;
    }
    core_log("display options: %s: resolution %s, scale %s put back", why, g_session.resolution, g_session.scale);
    core::settings::Request(Setting("resolution"), g_session.resolution, true);
    core::settings::Request(Setting("scale"), g_session.scale, true);
    g_session.changed = false;
}

void Press(int id)
{
    char v[16];
    switch (id) {
    case kWindowButton: {
        const bool to = !core::display::Windowed();
        core_log("display options: %s", to ? "windowed" : "fullscreen");
        core::settings::Request(Setting("window"), to ? "windowed" : "fullscreen", true); // as F5: saved at once
        break;
    }
    case kNativeButton:
        std::snprintf(v, sizeof(v), "%dx%d", core_game()->native_width, core_game()->native_height);
        core_log("display options: %s", v);
        SetDisplay(v, nullptr);
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
    case kScalingButton: { // nearest -> linear -> sharp_bilinear -> nearest; saved at once (as the window line)
        char now[core::settings::kValueSize];
        Value("scaling", now);
        const char *next = std::strcmp(now, "nearest") == 0  ? "linear"
                           : std::strcmp(now, "linear") == 0 ? "sharp_bilinear"
                                                             : "nearest";
        core_log("display options: scaling %s", next);
        core::settings::Request(Setting("scaling"), next, true);
        break;
    }
    case kOk:
        core_log("display options: OK%s", g_session.changed ? " (resolution and scale saved)" : "");
        if (g_session.changed) {
            core::settings::RequestSave(); // after the requests before it: the values they set are confirmed
            g_session.changed = false;
        }
        g_game->close();
        break;
    case kCancelButton:
        PutBack("Cancel");
        g_game->close();
        break;
    }
}

template <int N> void __cdecl OnButton()
{
    if (!g_game) {
        return;
    }
    int id = N;
    if (!core::diag::Guarded(g_feature, [](void *p) { Press(*static_cast<int *>(p)); }, &id)) {
        core_log("display options: button %d abandoned after an error", N);
    }
}

const Handler kHandlers[kItemCount] = {&OnButton<1>, &OnButton<2>, &OnButton<3>, &OnButton<4>, &OnButton<5>,
                                       &OnButton<6>, &OnButton<7>, &OnButton<8>, &OnButton<9>};

// ---------------------------------------------------------------------------------------------
// Drawing (game thread, from the game's dialog drawing)

void DrawDialog()
{
    const Game &g = *g_game;
    int w = 0, h = 0;
    g.screenSize(&w, &h);
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
        g_session.behindDue = true;
    }
    if (g_session.behindDue && g.drawBehind()) {
        g_session.behindDue = false;
    }
    const int hover = g.hover();
    g.panel(kPanelX, kPanelY, kPanelCols, kPanelRows);
    const bool pressedShown = g_pressed && GetTickCount() - g_pressedAt < kArrowPressedMs;
    for (const Item &it : kItems) {
        if (it.arrow != Arrow::None) {
            g.arrow(it, pressedShown && g_pressed == it.id);
        }
        else {
            g.button(it, hover == it.id);
        }
    }
    auto look = [hover](int id) { return hover == id ? TextLook::ButtonHover : TextLook::Button; };
    g.gameText(GameText::Title, kLeft, kTitleY, kWidth, TextLook::Status);
    auto gameText = [&](GameText t, const Item &it) { g.gameText(t, it.x, it.y + kTextDown, it.w, look(it.id)); };
    gameText(core::display::Windowed() ? GameText::FullScreen : GameText::Windowed, ItemOf(kWindowButton));
    gameText(GameText::NativeSize, ItemOf(kNativeButton));
    char scaling[core::settings::kValueSize], scalingText[48];
    Value("scaling", scaling);
    std::snprintf(scalingText, sizeof(scalingText), "Scaling: %s",
                  std::strcmp(scaling, "sharp_bilinear") == 0 ? "sharp bilinear" : scaling);
    const struct {
        Id id;
        const char *s;
    } kOwnText[] = {{k100, "100%"}, {kAuto, "Auto"}, {kScalingButton, scalingText}, {kOk, "OK"}};
    for (const auto &o : kOwnText) {
        const Item &it = ItemOf(o.id);
        g.text(o.s, it.x, it.y + kTextDown, it.w, look(it.id));
    }
    gameText(GameText::Cancel, ItemOf(kCancelButton));
    char line1[48], line2[48];
    const int scale = ScaleSetting();
    StatusLines(w, h, Scaled(), scale, EffectiveScale(scale), line1, sizeof(line1), line2, sizeof(line2));
    g.text(line1, kLeft, kStatusY, kWidth, TextLook::Status);
    g.text(line2, kLeft, kRowScale + kArrowTextDown, kScaleTextWidth, TextLook::Status);
    g.endDraw();
}

} // namespace

const Item (&Items())[kItemCount]
{
    return kItems;
}

const Item &ItemOf(int id)
{
    return kItems[(id >= 1 && id <= kItemCount ? id : 1) - 1];
}

void Attach(const Game &g)
{
    g_game = &g;
}

bool Working()
{
    return g_game && core::diag::Enabled(g_feature);
}

bool Draw()
{
    if (!Working()) {
        return false;
    }
    return core::diag::Guarded(g_feature, [](void *) { DrawDialog(); }, nullptr);
}

Handler HandlerOf(int id)
{
    return kHandlers[(id >= 1 && id <= kItemCount ? id : 1) - 1];
}

void OnLoop()
{
    if (!g_game || !g_session.open || g_game->dialogShown()) {
        return;
    }
    g_session.open = false;
    PutBack("closed without OK");
}

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

void DetachForTest()
{
    g_game = nullptr;
    g_session = Session{};
    g_pressed = 0;
    g_feature.off = false;
}

} // namespace core::options::display_dialog
