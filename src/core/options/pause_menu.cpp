// The pause menu: see pause_menu.h. Moved from Yongle's games/emperor/options_window.cpp (HandlePause, ClosePause,
// OpenPause, DrawNow's pause branch) and game_menu.cpp (the pending item, Replay / MainMenu / Exit's "answered No"
// branch, Exit's padding) for Pericles.
#include "core/options/pause_menu.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/options/menu.h"
#include "core/ui/hotkey.h"

#include <algorithm>
#include <cmath>

namespace core::options::pause_menu {
namespace {

const PauseGame *g_game = nullptr;
core::diag::Feature g_feature{"pause menu (core/options/pause_menu)"};
Menu g_menu;
bool g_open = false;
std::string g_title;
std::vector<std::string> g_items;
int g_pending = 0;
bool g_hasPending = false;

struct Message {
    UINT m;
    WPARAM w;
    int x, y;
};

void HandleNow(void *p)
{
    const Message &msg = *static_cast<const Message *>(p);
    int ox = 0, oy = 0;
    g_game->origin(&ox, &oy);
    const int x = msg.x - ox, y = msg.y - oy;
    switch (msg.m) {
    case WM_MOUSEMOVE: g_menu.MouseMove(x, y); break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        g_game->click(); // as the game: a click sound at every press, even on the panel
        g_menu.MouseDown(x, y);
        break;
    case WM_LBUTTONUP: {
        const int item = g_menu.MouseUp(x, y);
        if (item >= 0) {
            g_game->act(item);
        }
        break;
    }
    case WM_RBUTTONUP: Close("right click"); break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: { // Alt combinations and F10 come as system keys
        auto held = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };
        const unsigned mods = (held(VK_CONTROL) ? core::ui::kModCtrl : 0u) |
                              (held(VK_SHIFT) ? core::ui::kModShift : 0u) | (held(VK_MENU) ? core::ui::kModAlt : 0u);
        if (g_menu.Key(static_cast<int>(msg.w)) == Menu::kClose) {
            Close("Esc");
        }
        else {
            g_game->optionsKey(static_cast<unsigned>(msg.w), mods);
        }
        break;
    }
    default: break;
    }
}

void DrawNow(void *)
{
    g_game->beginDraw();
    g_menu.Draw(g_game->painter);
    g_game->redraw(); // the hover frame moves with the mouse: the screen under the menu is drawn again
}

void Failed(const char *where)
{
    g_open = false;
    g_hasPending = false;
    if (g_game) {
        g_game->redraw();
    }
    core_log("pause menu: closed after an error %s; Esc and the File menu stay the game's", where);
}

} // namespace

void Attach(const PauseGame &game)
{
    g_game = &game;
    core_log("pause menu: available (%s)", game.logName);
}

bool Available()
{
    return g_game != nullptr && core::diag::Enabled(g_feature);
}

bool Open(std::string title, std::vector<std::string> items)
{
    if (!Available()) {
        return false;
    }
    if (g_open) {
        return true;
    }
    g_title = std::move(title);
    g_items = std::move(items);
    g_menu.SetLook(g_game->look);
    g_menu.Set(g_title, g_items);
    g_open = true;
    g_game->redraw();
    core_log("pause menu: opened (the game is paused)");
    return true;
}

bool Reopen()
{
    return Open(g_title, g_items);
}

bool IsOpen()
{
    return g_open;
}

void Close(const char *why)
{
    if (!g_open) {
        return;
    }
    g_open = false;
    g_game->redraw();
    core_log("pause menu: closed (%s)", why);
}

bool Active()
{
    return g_open;
}

void HandleMessage(UINT m, WPARAM w, LPARAM, int x, int y)
{
    if (!g_open || !Available()) {
        return;
    }
    Message msg{m, w, x, y};
    if (!core::diag::Guarded(g_feature, &HandleNow, &msg)) {
        Failed("in its input");
    }
}

void OnPresent(unsigned long caller)
{
    if (!g_open || !Available() || !g_game->canDraw(caller)) {
        return;
    }
    if (!core::diag::Guarded(g_feature, &DrawNow, nullptr)) {
        Failed("while drawing");
    }
}

void Request(int item)
{
    g_pending = item;
    g_hasPending = true;
}

bool TakePending(int *item)
{
    if (!g_hasPending) {
        return false;
    }
    g_hasPending = false;
    *item = g_pending;
    return true;
}

void AfterConfirm(bool yes, bool fromPause, void (*action)(), const char *logPrefix, const char *what)
{
    if (yes) {
        core_log("%s: %s", logPrefix, what);
        action();
    }
    else if (fromPause) {
        Reopen();
    }
}

namespace {

constexpr int kMargin = 20; // the text's glyphs at least this far inside the panel's sides (its frame is ~10 px)

// Where the visible letters of s (spaces around them) have their middle, in the font measured by width, when s is
// drawn from 0: the leading spaces' width plus half the letters'.
double GlyphMiddle(const std::string &s, ConfirmBox::Width_fn width, double *left, double *right)
{
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) {
        *left = *right = 0;
        return 0;
    }
    const size_t b = s.find_last_not_of(' ');
    const double lead = a > 0 ? width(s.substr(0, a).c_str()) : 0;
    const double core = width(s.substr(a, b - a + 1).c_str());
    *left = lead;
    *right = lead + core;
    return lead + core / 2;
}

} // namespace

void LayOutConfirm(const ConfirmBox &box, std::string *title, std::string *text, int minWidth)
{
    const std::string t0 = *title, s0 = *text;
    double best = 1e18;
    std::string bestT = t0, bestS = s0;
    for (int c = 0; c <= 2; ++c) {
        for (int d = 0; d <= 12; ++d) {
            const std::string t = std::string(static_cast<size_t>(c), ' ') + t0 + std::string(static_cast<size_t>(d), ' ');
            const int tMeasure = box.titleMeasure(t.c_str()), tDraw = box.titleDraw(t.c_str());
            double tl = 0, tr = 0;
            const double tMid = GlyphMiddle(t, box.titleDraw, &tl, &tr);
            for (int a = 0; a <= 12; ++a) {
                for (int b = 0; b <= 12; ++b) {
                    const std::string s = std::string(static_cast<size_t>(a), ' ') + s0 + std::string(static_cast<size_t>(b), ' ');
                    const int sw = box.textWidth(s.c_str());
                    const int chosen = sw < tMeasure ? tMeasure : sw;
                    if (minWidth > 0 && chosen < minWidth) {
                        continue;
                    }
                    const int w = chosen + 20, panel = (w / 16 + 1) * 16;
                    const double mid = panel / 2.0;
                    double sl = 0, sr = 0;
                    const double sMid = GlyphMiddle(s, box.textWidth, &sl, &sr);
                    const double sStart = box.textX + std::max(0, (w - 10 - sw) / 2);
                    if (sStart + sl < kMargin || sStart + sr > panel - kMargin) {
                        continue;
                    }
                    const double tStart = box.titleX + std::max(0, (w - tDraw) / 2);
                    const int bw = w / 3;
                    const double buttons = w / 2 - (bw / 2) * 2 + bw;
                    const double cost = std::abs(tStart + tMid - mid) + std::abs(sStart + sMid - mid) +
                                        std::abs(buttons - mid) + 0.01 * (a + b + c + d);
                    if (cost < best) {
                        best = cost;
                        bestT = t;
                        bestS = s;
                    }
                }
            }
        }
    }
    *title = bestT;
    *text = bestS;
}

std::string PadToWidth(std::string text, int wide, int (*width)(const char *text))
{
    for (int i = 0; i < 40 && width(text.c_str()) < wide; ++i) {
        text = " " + text + " ";
    }
    return text;
}

} // namespace core::options::pause_menu
