// Offline test of core/options/pause_menu (no DLL, no game): opening and closing, the mouse relative to the game's
// dialog origin, a click on an item (pressed and released on it), right click and Esc, other keys to the game layer,
// drawing only where the game layer allows it, the pending item, a confirmation answered Yes / No (No from the menu
// opens it again with the same items), the padding of a confirmation's text, and a fault in the game layer's callback
// (the menu closes and stays unavailable). A fake game layer records what it is asked.
#include "core/diag/guard.h"
#include "core/options/menu.h"
#include "core/options/pause_menu.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace co = core::options;
namespace pm = core::options::pause_menu;

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-72s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

class FakePainter : public co::Painter {
public:
    int panels = 0, bars = 0, frames = 0, texts = 0;
    std::vector<std::string> drawn;
    void Panel(int, int, int, int) override { ++panels; }
    void Inset(int, int, int, int) override {}
    void ButtonBar(int, int, int, int) override { ++bars; }
    void Frame(int, int, int, int) override { ++frames; }
    void Strip(int, int, int) override {}
    void Check(int, int, bool) override {}
    void ArrowButton(int, int, co::Arrow, co::ArrowState) override {}
    void Knob(int, int) override {}
    void Thumb(int, int) override {}
    void Line(int, int, int) override {}
    void Text(const char *t, int, int, co::Font) override
    {
        ++texts;
        drawn.push_back(t);
    }
    int TextWidth(const char *t, co::Font) override { return 7 * static_cast<int>(std::strlen(t)); }
    void TipBox(int, int, int, int) override {}
    void Reset() { *this = FakePainter(); }
};

FakePainter g_painter;
int g_ox = 100, g_oy = 50; // the game's dialog origin on the screen
int g_redraws = 0, g_clicks = 0, g_begins = 0, g_lastAct = -99, g_acts = 0, g_keys = 0;
bool g_canDraw = true, g_handleKey = false, g_faultInAct = false;
unsigned g_lastKey = 0;

void Origin(int *x, int *y)
{
    *x = g_ox;
    *y = g_oy;
}
void BeginDraw() { ++g_begins; }
void Redraw() { ++g_redraws; }
void Click() { ++g_clicks; }
bool CanDraw(unsigned long caller) { return g_canDraw && caller != 0x1234; }
void Act(int item)
{
    if (g_faultInAct) {
        *static_cast<volatile int *>(nullptr) = 1; // a fault in the game layer's code
    }
    g_lastAct = item;
    ++g_acts;
    if (item == 0) {
        pm::Close("Continue");
    }
    else if (item == 1) {
        pm::Close("Load");
        pm::Request(7);
    }
}
bool Key(unsigned vk, unsigned)
{
    ++g_keys;
    g_lastKey = vk;
    return g_handleKey;
}

const pm::PauseGame kGame("test", g_painter, co::MenuLook{}, &Origin, &BeginDraw, &Redraw, &Click, &CanDraw, &Act, &Key);

// Screen coordinates of item i's button centre.
int ItemX(const co::Menu &m) { return g_ox + m.ButtonX() + co::Menu::kButtonW / 2; }
int ItemY(const co::Menu &m, int i) { return g_oy + m.ButtonY(i) + co::Menu::kButtonH / 2; }

void Send(UINT m, int x, int y, WPARAM w = 0)
{
    pm::HandleMessage(m, w, 0, x, y);
}

int g_actions = 0;
void Action() { ++g_actions; }

int Width(const char *t) { return 5 * static_cast<int>(std::strlen(t)); }

// Fonts for the confirmation box: letters and spaces of different widths, as the game's (a space narrower).
int FontW(const char *t, int letter, int space)
{
    int w = 0;
    for (const char *c = t; *c; ++c) {
        w += *c == ' ' ? space : letter;
    }
    return w;
}
int TitleMeasureW(const char *t) { return FontW(t, 13, 6); }
int TitleDrawW(const char *t) { return FontW(t, 15, 8); }
int TextW(const char *t) { return FontW(t, 9, 4); }

const char *const kPairs[3][2] = {{"Replay map", "Do you wish to leave the current game?"},
                                  {"Exit to main menu", "Do you wish to leave the current game?"},
                                  {"Quit", "Exit Game?"}};

// Where the game's box puts the title's letters, the text's letters and the buttons, from the panel's middle; and the
// text's room to the panel's sides (the box's rules: core/options/pause_menu.h ConfirmBox).
struct Placed {
    double title, text, buttons;
    double textLeft, textRight;
};
double Mid(const std::string &s, int (*w)(const char *), double *l, double *r)
{
    const size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    *l = a > 0 ? w(s.substr(0, a).c_str()) : 0;
    *r = *l + w(s.substr(a, b - a + 1).c_str());
    return (*l + *r) / 2;
}
Placed Place(const pm::ConfirmBox &box, const std::string &t, const std::string &s)
{
    const int tm = box.titleMeasure(t.c_str()), sw = box.textWidth(s.c_str());
    const int w = (sw < tm ? tm : sw) + 20, panel = (w / 16 + 1) * 16;
    const double mid = panel / 2.0;
    double l = 0, r = 0, sl = 0, sr = 0;
    const double tMid = Mid(t, box.titleDraw, &l, &r);
    const double tStart = box.titleX + std::max(0, (w - box.titleDraw(t.c_str())) / 2);
    const double sMid = Mid(s, box.textWidth, &sl, &sr);
    const double sStart = box.textX + std::max(0, (w - 10 - sw) / 2);
    const int bw = w / 3;
    return {tStart + tMid - mid, sStart + sMid - mid, w / 2 - (bw / 2) * 2 + bw - mid, sStart + sl, panel - (sStart + sr)};
}

} // namespace

int main()
{
    core::diag::InstallExceptionLog();
    const std::vector<std::string> items{"Continue", "Load", "Exit"};

    Check(!pm::Available() && !pm::Open("Pause", items), "not attached: not available, Open refuses");
    pm::Attach(kGame);
    Check(pm::Available() && !pm::IsOpen() && !pm::Active(), "attached: available, closed");

    // Drawing and input while closed do nothing.
    pm::OnPresent(1);
    Send(WM_LBUTTONDOWN, 0, 0);
    Check(g_begins == 0 && g_clicks == 0, "closed: nothing drawn, no input taken");

    g_redraws = 0;
    Check(pm::Open("Pause", items) && pm::IsOpen() && pm::Active() && g_redraws == 1, "Open: open, screen redrawn");
    Check(pm::Open("Other", {"x"}) && pm::IsOpen(), "Open while open: stays open (true)");

    // Drawing: only where the game layer allows it; the menu's title and items, the screen redrawn under it.
    g_painter.Reset();
    g_redraws = 0;
    pm::OnPresent(0x1234);
    g_canDraw = false;
    pm::OnPresent(1);
    Check(g_begins == 0 && g_painter.panels == 0, "not drawn where canDraw says no");
    g_canDraw = true;
    pm::OnPresent(1);
    Check(g_begins == 1 && g_painter.panels == 1 && g_painter.bars == 3 && g_redraws == 1,
          "drawn: begin, one panel, three bars, screen redrawn");
    Check(g_painter.drawn.size() == 4 && g_painter.drawn[0] == "Pause" && g_painter.drawn[3] == "Exit",
          "drawn texts: the first title and items (a second Open did not replace them)");

    // The mouse is relative to the game's origin: hover over item 2 shows the frame.
    co::Menu layout;
    layout.Set("Pause", items);
    Send(WM_MOUSEMOVE, ItemX(layout), ItemY(layout, 2));
    g_painter.Reset();
    pm::OnPresent(1);
    Check(g_painter.frames == 1, "hover (screen coordinates less the origin): the frame drawn");

    // Pressed on one item, released on another: nothing; a click sound at every press.
    g_clicks = 0;
    Send(WM_LBUTTONDOWN, ItemX(layout), ItemY(layout, 2));
    Send(WM_LBUTTONUP, ItemX(layout), ItemY(layout, 0));
    Check(g_acts == 0 && g_clicks == 1 && pm::IsOpen(), "pressed on one item, released on another: no act; a click");

    // Other keys go to the game layer with the key; Esc closes.
    Send(WM_KEYDOWN, 0, 0, VK_F6);
    Check(g_keys == 1 && g_lastKey == VK_F6 && pm::IsOpen(), "another key: to the game layer; the menu stays");
    Send(WM_KEYDOWN, 0, 0, VK_ESCAPE);
    Check(!pm::IsOpen() && g_keys == 1, "Esc: closed (not passed on)");

    // Right click closes.
    pm::Open("Pause", items);
    Send(WM_RBUTTONUP, 0, 0);
    Check(!pm::IsOpen(), "right click: closed");

    // Click on item 1: act(1), which closes and requests 7; the pending item is taken once.
    pm::Open("Pause", items);
    Send(WM_LBUTTONDOWN, ItemX(layout), ItemY(layout, 1));
    Send(WM_LBUTTONUP, ItemX(layout), ItemY(layout, 1));
    int pending = -1;
    Check(g_acts == 1 && g_lastAct == 1 && !pm::IsOpen(), "click on item 1: act(1); closed by the game layer");
    Check(pm::TakePending(&pending) && pending == 7 && !pm::TakePending(&pending), "pending item taken once");

    // A confirmation: Yes runs the action, the menu stays closed; No from the menu opens it again with its items.
    pm::AfterConfirm(true, true, &Action, "test", "yes");
    Check(g_actions == 1 && !pm::IsOpen(), "Yes: the action runs; the menu stays closed");
    pm::AfterConfirm(false, false, &Action, "test", "no");
    Check(g_actions == 1 && !pm::IsOpen(), "No, not from the menu: nothing");
    pm::AfterConfirm(false, true, &Action, "test", "no");
    g_painter.Reset();
    pm::OnPresent(1);
    Check(g_actions == 1 && pm::IsOpen() && g_painter.drawn.size() == 4 && g_painter.drawn[1] == "Continue",
          "No from the menu: open again with the same title and items");
    pm::Close("test");

    // Padding: spaces on both sides until at least as wide; at most 40 times.
    Check(pm::PadToWidth("ab", 30, &Width) == "  ab  ", "padded to the width (both sides)");
    Check(pm::PadToWidth("abcdefgh", 30, &Width) == "abcdefgh", "already wide enough: unchanged");
    Check(pm::PadToWidth("a", 100000, &Width).size() == 81, "padding stops after 40 times");

    // The confirmation box's layout (the game's rules): before, the long text 10 px from the box's side and the title
    // right of the middle; after, all three near the panel's middle and the text inside its margins.
    {
        const pm::ConfirmBox zeus(16, 5, &TitleMeasureW, &TitleDrawW, &TextW);
        const pm::ConfirmBox emperor(16, 10, &TitleMeasureW, &TitleMeasureW, &TextW);
        for (const pm::ConfirmBox *box : {&zeus, &emperor}) {
            for (const char *const *pair : {kPairs[0], kPairs[1], kPairs[2]}) {
                std::string t = pair[0], s = pair[1];
                const Placed before = Place(*box, t, s);
                pm::LayOutConfirm(*box, &t, &s, 0);
                const Placed after = Place(*box, t, s);
                char what[160];
                std::snprintf(what, sizeof what, "confirm layout (text x %d): \"%s\": title %.1f -> %.1f, text %.1f -> "
                              "%.1f, buttons %.1f from the middle", box->textX, pair[0], before.title, after.title,
                              before.text, after.text, after.buttons);
                Check(std::abs(after.title) <= 4 && std::abs(after.text) <= 4 && std::abs(after.buttons) <= 4 &&
                          after.textLeft >= 20 && after.textRight >= 20,
                      what);
            }
        }
        std::string t = "Quit", s = "Exit Game?";
        pm::LayOutConfirm(zeus, &t, &s, TitleMeasureW("Exit Game?"));
        Check(TextW(s.c_str()) >= TitleMeasureW("Exit Game?"), "confirm layout: the minimum width kept (Exit game)");
    }

    // A fault in the game layer's act: the menu closes, the screen is redrawn, the menu stays unavailable.
    pm::Open("Pause", items);
    pm::Request(3);
    g_faultInAct = true;
    g_redraws = 0;
    Send(WM_LBUTTONDOWN, ItemX(layout), ItemY(layout, 2));
    Send(WM_LBUTTONUP, ItemX(layout), ItemY(layout, 2));
    Check(!pm::IsOpen() && !pm::Available() && g_redraws == 1, "fault in act: closed, redrawn, not available");
    Check(!pm::TakePending(&pending), "fault: the pending item dropped");
    Check(!pm::Open("Pause", items), "after the fault: Open refuses (the game's own Esc stays)");

    std::printf("%s\n", g_failures == 0 ? "RESULT: PASS (0 failure(s))" : "RESULT: FAILED");
    return g_failures == 0 ? 0 : 1;
}
