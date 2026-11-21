#include "core/options/options_host.h"

#include "core/diag/guard.h"
#include "core/display/virtual_display.h"
#include "core/input/keys.h"
#include "core/log.h"
#include "core/options/menu_text.h"
#include "core/options/setting_rows.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"
#include "core/ui/settings_view.h"

#include <windows.h>

#include <string>
#include <vector>

namespace core::options::options_host {
namespace {

namespace pm = core::options::pause_menu;
namespace mt = core::options::menu_text;
namespace cs = core::settings;
namespace sr = core::options::setting_rows;

const OptionsGame *g_game = nullptr;
Window g_window; // the options window
Window g_keys;   // the Hotkeys window, over it or alone (from the game's Options menu)
bool g_open = false;     // either is open
bool g_keysOpen = false; // the Hotkeys window is the one shown
bool g_keysOnly = false; // ... opened alone: closing it closes everything
bool g_waiting = false;  // a Key row waits for a key (core/ui SetOtherCapture)
core::diag::Feature g_feature{"options window (core/options/options_host)"};

Window &Front()
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

// Everything closed after an error: the window, a wait for a key (our keys work again), the pause menu under it.
void Reset()
{
    g_open = g_keysOpen = g_keysOnly = false;
    SetWaiting(false, 0);
}

void Close(const char *why)
{
    if (!g_open) {
        return;
    }
    g_open = g_keysOpen = g_keysOnly = false;
    SetWaiting(false, 0);
    g_game->pause.redraw(); // the screen under the window is drawn again
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

void Prepare(Window &w, bool keys)
{
    Look look;
    look.title = g_game->title;
    look.hotkeysButton = g_game->keyPages != kNoHotkeys;
    if (keys) {
        look.title = mt::kHotkeysTitle;
        look.hotkeysButton = look.applyButton = false;
        look.plainHeaders = true; // Augustus's hotkey list: the section titles in gold at the left
        look.columnsAbove = true; // ... and "Hotkey" / "Alternative" above the list
    }
    w.SetLook(look);
    Texts t;
    t.pressKey = mt::kPressKey;
    t.notChanged = mt::kKeyNotChanged;
    t.notUsable = mt::kKeyNotUsable;
    t.inUse = mt::kKeyInUse;
    t.reserved = mt::kKeyReserved;
    t.clearHint = mt::kKeyClearHint;
    t.keyTip = mt::kKeyTip;
    t.keepTitle = mt::kKeepTitle;
    t.keepCountdown = mt::kKeepCountdown;
    t.keep = mt::kKeep;
    t.revert = mt::kRevert;
    w.SetTexts(t);
    // The game's keys are in the Hotkeys window's list: a key in use says which action has it (no game key note).
    w.SetReservedKey(g_game->reservedKey);
    w.SetOnKept(&Kept);
    w.SetRestartTip(mt::kRestartTip);
}

void OpenKeys(bool alone)
{
    Prepare(g_keys, true);
    if (alone) {
        sr::Clear(); // over the options window its rows still read and write their settings: theirs stay
    }
    g_keys.SetPages(g_game->keyPages());
    g_keys.Open(0, 0);
    g_keysOpen = true;
    g_keysOnly = alone;
    core_log("options window: Hotkeys opened%s", alone ? " (from the Options menu)" : "");
}

struct Where {
    int page, category;
    bool keys;        // the Hotkeys window alone
    const char *name; // the category by its name (nullptr: by number)
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
    sr::Clear();
    g_window.SetPages(g_game->pages());
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
    int ox = 0, oy = 0;
    g_game->pause.origin(&ox, &oy);
    const int x = msg.x - ox, y = msg.y - oy;
    Window &w = Front();
    Action a = Action::None;
    switch (msg.m) {
    case WM_MOUSEMOVE: w.MouseMove(x, y); break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        g_game->pause.click(); // as the game: a click sound at every press, even on the panel
        a = w.MouseDown(x, y, GetTickCount());
        break;
    case WM_LBUTTONUP: a = w.MouseUp(x, y); break;
    case WM_RBUTTONUP: // the game closes its dialogs with a right click (not while a key is awaited); on a key box it
                       // removes that key
        if (w.RightClick(x, y)) {
            g_game->pause.click();
            break;
        }
        if (!w.WaitingForKey()) {
            w.Closing();
            w.Cancel();
            a = Action::Close;
        }
        break;
    case WM_MOUSEWHEEL: w.Wheel(GET_WHEEL_DELTA_WPARAM(msg.w) / WHEEL_DELTA); break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: { // Alt combinations and F10 come as system keys
        auto held = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };
        const unsigned mods = (held(VK_CONTROL) ? core::ui::kModCtrl : 0u) |
                              (held(VK_SHIFT) ? core::ui::kModShift : 0u) | (held(VK_MENU) ? core::ui::kModAlt : 0u);
        if (!w.WaitingForKey() && !w.Confirming() &&
            core::input::keys::IsOptionsKey(static_cast<unsigned>(msg.w), mods)) {
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
    if (a == Action::Close) {
        if (g_keysOpen) {
            CloseKeys("OK or Cancel");
        }
        else {
            Close("OK or Cancel");
        }
    }
    else if (a == Action::OpenHotkeys && g_game->keyPages != kNoHotkeys) {
        OpenKeys(false);
    }
}

// The options window, or the pause menu under it (its input comes through here: one ModalInput slot).
bool Active()
{
    return g_open || pm::Active();
}

void OnMessage(UINT m, WPARAM w, LPARAM l, int x, int y)
{
    if (!g_open) {
        pm::HandleMessage(m, w, l, x, y); // the pause menu alone (it guards itself)
        return;
    }
    Message msg{m, w, x, y};
    if (!core::diag::Guarded(g_feature, &HandleNow, &msg)) {
        Reset();
        pm::Close("an error in the options window");
        g_game->pause.redraw();
        core_log("options window: closed after an error; the game's own options stay");
    }
}

const core::display::ModalInput kInput{&Active, &OnMessage};

void DrawNow(void *)
{
    g_game->pause.beginDraw(); // the game's clip reset and the painter's origin
    Window &w = Front();
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
    Front().Draw(g_game->pause.painter); // the Hotkeys window covers the options window (the same size)
    g_game->pause.redraw(); // the next frame draws the screen under the window again (tooltips, the button under
                            // the mouse)
}

bool OpenAt(const Where &w, const char *failure)
{
    if (!Available()) {
        if (g_attached) { // switched off after an error (the pause menu's Options, a menu item, the key)
            core_log("options window: not available after an error; nothing opens");
        }
        return false;
    }
    if (g_open) {
        return true;
    }
    Where copy = w;
    if (!core::diag::Guarded(g_feature, &OpenNow, &copy)) {
        Reset();
        core_log("options window: %s", failure);
        return false;
    }
    return true;
}

// The debug panel leaves out what the windows list, except the settings to change there when something goes wrong.
void HideListed(const OptionsGame &game)
{
    sr::SetDry(true);
    game.pages();
    if (game.keyPages != kNoHotkeys) {
        game.keyPages();
    }
    sr::SetDry(false);
    sr::Clear();
    std::vector<int> hide;
    for (int i : sr::Listed()) {
        bool keep = false;
        for (int k = 0; k < game.emergencyCount; ++k) {
            keep = keep || i == cs::Find(game.emergency[k][0], game.emergency[k][1]);
        }
        if (!keep) {
            hide.push_back(i);
        }
    }
    core::ui::SetListedElsewhere(hide.data(), static_cast<int>(hide.size()));
    core_log("options window: the debug panel leaves out %d setting(s) listed here (\"Show all settings\" shows them)",
             static_cast<int>(hide.size()));
}

} // namespace

void Attach(const OptionsGame &game)
{
    g_game = &game;
    core::display::SetModalInput(&kInput); // the pause menu's input too (one slot)
    g_attached = true;
    pm::Attach(game.pause);
    core_log("options window: available (%s)", game.pause.logName);
    if (game.hideListed) {
        HideListed(game);
    }
    else {
        core_log("options window: the debug panel lists every setting (the game layer keeps them there for now)");
    }
}

bool Available()
{
    return g_attached && core::diag::Enabled(g_feature);
}

bool Open(int page, int category)
{
    return OpenAt(Where{page, category, false, nullptr}, "not available after an error (the game's own options stay)");
}

bool OpenCategory(int page, const char *name)
{
    return OpenAt(Where{page, 0, false, name}, "not available after an error (the game's own options stay)");
}

bool OpenHotkeys()
{
    if (g_game && g_game->keyPages == kNoHotkeys) {
        return false;
    }
    return OpenAt(Where{0, 0, true, nullptr}, "not available after an error");
}

bool IsOpen()
{
    return g_open || pm::IsOpen();
}

bool WindowOpen()
{
    return g_open;
}

bool PauseOptionsKey(unsigned vk, unsigned mods)
{
    if (!core::input::keys::IsOptionsKey(vk, mods)) {
        return false;
    }
    Open(0, 0); // over the pause menu; closing it shows the pause menu again
    return true;
}

void OnPresent(unsigned long caller)
{
    if (!g_open) {
        pm::OnPresent(caller); // the pause menu alone
        return;
    }
    if (!g_game->pause.canDraw(caller)) {
        return;
    }
    if (!core::diag::Guarded(g_feature, &DrawNow, nullptr)) {
        Reset();
        pm::Close("an error in the options window");
        g_game->pause.redraw();
        core_log("options window: closed after an error while drawing; the game's own options stay");
    }
}

} // namespace core::options::options_host
