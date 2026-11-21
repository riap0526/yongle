// Offline test of core/options/options_host: the options window in a game on a fake game layer (no DLL, no game):
// opening, drawing where the game allows it, the Hotkeys window over the options window (the options window's rows
// keep their settings), Apply after it, the options key and a right click closing it, Options from the pause menu
// and back, and an error while drawing (the window and the pause menu close, the window stays unavailable).
#include "core/diag/guard.h"
#include "core/display/virtual_display.h"
#include "core/input/keys.h"
#include "core/options/options_host.h"
#include "core/options/pause_menu.h"
#include "core/options/setting_rows.h"
#include "core/settings/settings.h"
#include "core/ui/settings_view.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace co = core::options;
namespace oh = core::options::options_host;
namespace pm = core::options::pause_menu;

// ---------------------------------------------------------------------------------------------------------------
// What the host uses from the rest of the core, faked

const core::display::ModalInput *g_modal = nullptr;
int g_clears = 0, g_saves = 0, g_listed = -1;

namespace core::display {
void SetModalInput(const ModalInput *input) { g_modal = input; }
} // namespace core::display
namespace core::input::keys {
bool IsOptionsKey(unsigned vk, unsigned mods) { return vk == VK_F6 && mods == 0; }
} // namespace core::input::keys
namespace core::settings {
void RequestSave() { ++g_saves; }
void RunPending() {}
int Find(const char *, const char *) { return -1; }
} // namespace core::settings
namespace core::ui {
void SetOtherCapture(bool, unsigned) {}
void SetListedElsewhere(const int *, int count) { g_listed = count; }
} // namespace core::ui
namespace core::options::setting_rows {
std::vector<int> g_none;
void Clear() { ++g_clears; }
void SetDry(bool) {}
bool Dry() { return false; }
const std::vector<int> &Listed() { return g_none; }
} // namespace core::options::setting_rows

namespace {

int g_failures = 0;
void Check(bool ok, const char *what)
{
    std::printf("%-78s %s\n", what, ok ? "ok" : "FAILED");
    g_failures += ok ? 0 : 1;
}

// The game layer: a painter that counts what it draws (and faults when asked), its callbacks.
struct Op {
    std::string op, text;
    int x, y, w;
};
bool g_fault = false;
class FakePainter : public co::Painter {
public:
    std::vector<Op> ops;
    void Add(const char *op, int x, int y, int w, const char *t = "")
    {
        if (g_fault) {
            *static_cast<volatile int *>(nullptr) = 1; // an access violation, as a fault in the game's art would be
        }
        ops.push_back({op, t, x, y, w});
    }
    void Panel(int x, int y, int w, int) override { Add("panel", x, y, w); }
    void Inset(int x, int y, int w, int) override { Add("inset", x, y, w); }
    void ButtonBar(int x, int y, int w, int) override { Add("bar", x, y, w); }
    void Frame(int x, int y, int w, int) override { Add("frame", x, y, w); }
    void Strip(int x, int y, int w) override { Add("strip", x, y, w); }
    void Check(int x, int y, bool) override { Add("check", x, y, 16); }
    void ArrowButton(int x, int y, co::Arrow, co::ArrowState) override { Add("arrow", x, y, 24); }
    void Knob(int x, int y) override { Add("knob", x, y, 16); }
    void Thumb(int x, int y) override { Add("thumb", x, y, 26); }
    void Line(int x, int y, int w) override { Add("line", x, y, w); }
    void Text(const char *t, int x, int y, co::Font f) override { Add("text", x, y, TextWidth(t, f), t); }
    int TextWidth(const char *t, co::Font) override { return 7 * static_cast<int>(std::strlen(t)); }
    void TipBox(int x, int y, int w, int) override { Add("tip", x, y, w); }
    const Op *Find(const char *t) const
    {
        for (const Op &o : ops) {
            if (o.op == "text" && o.text == t) return &o;
        }
        return nullptr;
    }
};
FakePainter g_painter;
int g_redraws = 0, g_clicks = 0, g_begins = 0;
bool g_canDraw = true;

void Origin(int *x, int *y) { *x = *y = 0; }
void BeginDraw() { ++g_begins; }
void Redraw() { ++g_redraws; }
void Click() { ++g_clicks; }
bool CanDraw(unsigned long) { return g_canDraw; }
void Act(int item)
{
    if (item == 1) {
        oh::Open(0, 0); // Options: over the pause menu
    }
    else {
        pm::Close("item");
    }
}

// The settings in effect, set only through Apply.
int g_values[4] = {0, 0, 0, 0};
int Get(const void *ctx) { return g_values[reinterpret_cast<intptr_t>(ctx)]; }
void Set(const void *ctx, int v) { g_values[reinterpret_cast<intptr_t>(ctx)] = v; }
co::Row CheckRow(const char *label, int slot)
{
    co::Row r;
    r.kind = co::Kind::Check;
    r.label = label;
    r.value = {Get, Set, reinterpret_cast<const void *>(static_cast<intptr_t>(slot))};
    return r;
}
std::vector<co::Page> Pages() { return {{"General", {{"", {CheckRow("Smooth scrolling", 0)}}}}}; }
std::vector<co::Page> KeyPages() { return {{"Hotkeys", {{"", {CheckRow("A key row's stand-in", 1)}}}}}; }
bool Reserved(unsigned, unsigned) { return false; }

const pm::PauseGame kPause("test", g_painter, co::MenuLook(), &Origin, &BeginDraw, &Redraw, &Click, &CanDraw, &Act,
                           &oh::PauseOptionsKey);
const char *const kEmergency[][2] = {{"video", "resolution"}};
const oh::OptionsGame kGame(kPause, "Test options", &Pages, &KeyPages, &Reserved, kEmergency, 1, false);

void Send(UINT m, WPARAM w = 0, int x = 0, int y = 0) { g_modal->message(m, w, 0, x, y); }
void ClickAt(int x, int y)
{
    Send(WM_LBUTTONDOWN, 0, x, y);
    Send(WM_LBUTTONUP, 0, x, y);
}
void Present()
{
    g_painter.ops.clear();
    oh::OnPresent(0x1234);
}

} // namespace

int main()
{
    core::diag::InstallExceptionLog();
    oh::Attach(kGame);
    Check(g_modal && oh::Available() && !oh::IsOpen() && g_listed == -1,
          "attach: the modal input registered; the debug panel keeps every setting (not hidden)");

    Check(oh::Open(0, 0) && oh::IsOpen() && oh::WindowOpen() && g_modal->active() && g_clears == 1,
          "open: the window, its rows built afresh");
    g_canDraw = false;
    Present();
    Check(g_painter.ops.empty(), "draw: nothing where the game does not allow it (canDraw false)");
    g_canDraw = true;
    Present();
    const Op *row = g_painter.Find("Smooth scrolling");
    Check(row && g_begins > 0 && g_redraws > 0, "draw: the window (after beginDraw), the screen drawn again next frame");

    // A change, the Hotkeys window over the window and back, then Apply: the row still has its setting.
    ClickAt(row->x + 5, row->y + 3);
    Check(g_clicks == 1 && g_values[0] == 0, "a click: the game's click sound, a draft only");
    const Op *hk = g_painter.Find("Hotkeys");
    Check(hk != nullptr, "the Hotkeys button (the game has a Hotkeys page)");
    ClickAt(hk->x + 5, hk->y + 3);
    Present();
    Check(g_painter.Find("A key row's stand-in") && g_clears == 1,
          "Hotkeys over the window: its page shown, the window's rows kept (not cleared)");
    Send(WM_KEYDOWN, VK_ESCAPE);
    Present();
    const Op *apply = g_painter.Find("Apply");
    Check(oh::WindowOpen() && g_painter.Find("Smooth scrolling") && apply, "Esc closes Hotkeys: the window again");
    ClickAt(apply->x + 5, apply->y + 3);
    Check(g_values[0] == 1, "Apply after Hotkeys: the change set through the row's setting");

    Send(WM_KEYDOWN, VK_F6);
    Check(!oh::IsOpen(), "the options key closes the window");
    oh::Open(0, 0);
    Send(WM_RBUTTONUP, 0, 100, 100);
    Check(!oh::IsOpen(), "a right click closes the window");

    // Options from the pause menu: over it; closing the window shows the pause menu again.
    pm::Open("Pause", {"Continue", "Options", "Exit"});
    Check(pm::IsOpen() && oh::IsOpen() && !oh::WindowOpen(), "pause menu: open (the window not)");
    Send(WM_KEYDOWN, VK_F6);
    Check(oh::WindowOpen() && pm::IsOpen(), "pause menu: the options key opens the window over it");
    Send(WM_KEYDOWN, VK_ESCAPE);
    Check(!oh::WindowOpen() && pm::IsOpen(), "... Esc closes the window: the pause menu again");

    // An error while drawing: the window and the pause menu close, the window stays unavailable.
    oh::Open(0, 0);
    const int redraws = g_redraws;
    g_fault = true;
    Present();
    g_fault = false;
    Check(!oh::IsOpen() && !pm::IsOpen() && !oh::Available() && g_redraws > redraws,
          "an error while drawing: everything closed, the screen drawn again, the window unavailable");
    Check(!oh::Open(0, 0), "... Open refuses from then on (the game layer's menus fall back)");
    // The pause menu stays usable (its own guard); its Options does nothing but log.
    Check(pm::Open("Pause", {"Continue", "Options", "Exit"}) && pm::IsOpen() && g_modal->active(),
          "after the window's error: the pause menu still opens (its input through the same slot)");
    Send(WM_KEYDOWN, VK_F6);
    Check(pm::IsOpen() && !oh::WindowOpen(), "... its options key: no window, the pause menu stays");
    pm::Close("test");

    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
