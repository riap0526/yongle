// Esc and the pause menu (0.2.0 item 7, K4): docs/archive/key_bindings.md section 8.
//
// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/archive/key_bindings.md section 8.2; calling conventions:
// docs/symbols.md.
#include "games/emperor/game_menu.h"

#include "core/diag/guard.h"
#include "core/hook/hook.h"
#include "core/log.h"
#include "games/emperor/options_text.h"
#include "games/emperor/options_window.h"
#include "games/emperor/site.h"
#include <string>

namespace emperor::game_menu {
namespace {

namespace ot = emperor::options_text;

// modal_dialog_run 0x5bfe60: __cdecl(title, text, buttons), returns 6 for Yes with buttons 4 (Yes / No); runs its own
// message loop (game_screen 0x54 meanwhile).
const Site kModalDialog{"modal_dialog_run", 0x5bfe60, {0x83, 0xec, 0x20, 0x53, 0x33, 0xc0, 0xa0, 0x45}};
using ModalDialog_fn = int(__cdecl *)(const char *title, const char *text, int buttons);
constexpr int kYesNo = 4, kYes = 6;
// What file_menu_leave_game 0x4493c0 calls after Yes: back to the main menu (or the campaign menu) / replay.
const Site kLeaveToMenu{"leave_to_main_menu", 0x401260, {0x53, 0xe8, 0x5a, 0x78, 0x00, 0x00, 0x8b, 0xc8}};
const Site kReplay{"replay_map", 0x42f500, {0x68, 0xea, 0xe2, 0x0d, 0x01, 0xe8, 0x36, 0x0a}};
// The File menu's Load / Save / Delete game: the game's file dialog (game_screen 0x0d).
const Site kLoad{"file_menu_load", 0x4014a0, {0x81, 0xec, 0x04, 0x01, 0x00, 0x00, 0x55, 0x56}};
const Site kSave{"file_menu_save", 0x401660, {0x81, 0xec, 0x04, 0x01, 0x00, 0x00, 0x53, 0x56}};
const Site kDelete{"file_menu_delete", 0x4017a0, {0x81, 0xec, 0x04, 0x01, 0x00, 0x00, 0x53, 0x56}};
// What a right click on the city calls (screen_handle_input 0x538842): drops the building tool, back to game_screen 1
// (from a selected company 0x48 or god 0x49); true when there was something to drop.
const Site kCityCancel{"city_cancel_tool", 0x404880, {0x53, 0xe8, 0x1a, 0x20, 0x0c, 0x00, 0x8a, 0x1d}};
// The China map's "return to the city" button (0x81d7e0, its handler 0x488510 jumps here): game_screen 1, drawn again.
const Site kMapToCity{"china_map_to_city", 0x406cc0, {0x83, 0x3d, 0x38, 0xec, 0x88, 0x00, 0x01, 0x75}};
using Void_fn = void(__cdecl *)();
using Bool_fn = bool(__cdecl *)();
// input_update_mouse 0x52b000 (__cdecl, no arguments): the buttons' edges of this pass; a pending right click of ours
// is added after it (right button released: 0x10de064).
const Site kInputMouse{"input_update_mouse", 0x52b000, {0x83, 0xec, 0x08, 0x8d, 0x44, 0x24, 0x00, 0x53}};
Void_fn o_InputMouse = nullptr;
// text_get_width(text, font) 0x527a80 (__cdecl) and image_get_id 0x408170 (__thiscall on the image table 0x1c42130,
// ret 4): what modal_dialog_draw 0x5bfa00 measures with. Its box is max(title in font 0x1e1c, text in font 0x1e14)
// + 20 pixels wide and each button a third of that [confirmed: Ghidra, 0x5bfa00].
const Site kTextWidth{"text_get_width", 0x527a80, {0x51, 0x53, 0x55, 0x56, 0x57, 0x68, 0x1c, 0x1e}};
using TextWidth_fn = int(__cdecl *)(const char *text, int font);
using ImageGetId_fn = int(__thiscall *)(void *, int);
const DWORD kImageGetId = 0x408170, kImages = 0x1c42130;
constexpr int kTitleFont = 0x1e1c, kTextFont = 0x1e14;
// lang_get_string 0x5278b0 (__cdecl(group, index); hooked by options_entry, which passes the game's texts on).
const DWORD kLangString = 0x5278b0;
using Lang_fn = const char *(__cdecl *)(int group, int index);

const Var<char> kGameScreen{0x10de045};
const Var<char> kRightUp{0x10de064};        // the right button released in this input pass (the dialogs close)
// The China map's right click is 0x10de063 (pressed), not the release (0x489d20): its selection is let go here directly.
const Var<int> kMapSelected{0xc5d0d8};      // the China map's selected city or army (0: none)
const Var<int> kMapSelected2{0x82b7cc};     // ... its second selection (-1: none)
const Var<char> kQuit{0x10c7205};           // the game ends
const Var<char> kRedrawAll{0x10de036};      // the screen is drawn again
const Var<char> kModalSeen{0x10de073};      // cleared after each confirmation (0x4770e7, 0x44940e)
const Var<char> kModalDone{0x13f8090};      // modal_dialog_done
const Var<int> kModalChoice{0x13f8094};     // modal_dialog_choice
const Var<int> kModalButtons{0x13f8098};    // modal_dialog_buttons
const Var<int> kBuildType{0x88ebdc};        // the building tool (0: none)
const Var<int> kBuilding{0x88ebd8};         // a building being placed
const Var<int> kEditor{0x88ec38};           // 1: the map editor

core::diag::Feature g_feature{"Esc and the pause menu (games/emperor/game_menu)"};
bool g_attached = false;
bool g_rightClick = false; // a right click for the next input pass
Item g_pending{};
bool g_hasPending = false;

const char *Lang(int group, int index)
{
    const char *t = reinterpret_cast<Lang_fn>(kLangString + g_imageShift)(group, index);
    return t ? t : "";
}

// Screens that close (or step back) with a right click: the dialogs and pop-ups over the city and the map
// (docs/resolution_ui/r2_screens.md section 4): Esc does the same.
bool RightClickScreen(int s)
{
    switch (s) {
    case 0x03: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
    case 0x0e: case 0x0f: case 0x11: case 0x16: case 0x19: case 0x1a: case 0x1c: case 0x1d: case 0x1e:
    case 0x21: case 0x23: case 0x2a: case 0x2d: case 0x3b: case 0x3f: case 0x45: case 0x51: case 0x52: case 0x55:
        return true;
    default:
        return false;
    }
}

// The menu screens that go back a step with a right click (released, 0x10de064), as Augustus' menus do with Esc:
// tutorials 0x02 (0x5edbc0), choose a campaign 0x1f, choose an emperor 0x2f (0x5a6df0), the emperor's campaigns 0x30
// (0x5165e0 -> screen_enter_registry), the mission intro 0x35, mission choice and briefing 0x3d (0x4472d0), high
// scores 0x53 (0x50b150 -> the main menu) [confirmed: Ghidra, screen_handle_input 0x538160 and those functions;
// 0x1f and 0x35 go through objects' virtual functions, not followed]. Not the victory and defeat screens (0x43, 0x33:
// a right click there moves on to the next mission): Esc keeps the game's own question there.
bool MenuBackScreen(int s)
{
    switch (s) {
    case 0x02: case 0x1f: case 0x2f: case 0x30: case 0x35: case 0x3d: case 0x53:
        return true;
    default:
        return false;
    }
}

bool Confirm(const char *title, const char *text)
{
    const int r = Function<ModalDialog_fn>(kModalDialog)(title, text, kYesNo);
    *kModalSeen = 0;
    return r == kYes;
}

void Replay(bool fromPause)
{
    if (Confirm(ot::kMenuReplayMap, Lang(5, 129))) { // "Do you wish to leave the current game?"
        core_log("game menu: replay");
        Function<Void_fn>(kReplay)();
    }
    else if (fromPause) {
        emperor::options_window::OpenPause();
    }
}

void MainMenu(bool fromPause)
{
    if (Confirm(ot::kMenuExitToMainMenu, Lang(5, 129))) {
        core_log("game menu: back to the main menu");
        Function<Void_fn>(kLeaveToMenu)();
    }
    else if (fromPause) {
        emperor::options_window::OpenPause();
    }
}

int TextWidth(const char *text, int fontGroup)
{
    const int font = reinterpret_cast<ImageGetId_fn>(kImageGetId + g_imageShift)(
        reinterpret_cast<void *>(kImages + g_imageShift), fontGroup);
    return Function<TextWidth_fn>(kTextWidth)(text, font);
}

void Exit(bool fromPause)
{
    // Title "Quit", text "Leave Emperor?" (as Augustus has them; the game's own box at 0x4770bb has them the other way
    // round). The short title would make the box and its buttons small, so the text gets spaces on both sides (it is
    // drawn centred) until it is as wide as the game's own box: "Leave Emperor?" in the title font.
    const char *title = Lang(5, 0);
    std::string text = Lang(5, 1);
    const int wide = TextWidth(text.c_str(), kTitleFont);
    for (int i = 0; i < 40 && TextWidth(text.c_str(), kTextFont) < wide; ++i) {
        text = " " + text + " ";
    }
    if (Confirm(title, text.c_str())) {
        core_log("game menu: exit");
        *kQuit = 1;
        *kRedrawAll = 1;
    }
    else if (fromPause) {
        emperor::options_window::OpenPause();
    }
}

void RunPending(void *)
{
    const Item item = g_pending;
    switch (item) {
    case Item::Replay: Replay(true); break;
    case Item::MainMenu: MainMenu(true); break;
    case Item::Exit: Exit(true); break;
    case Item::Load: Function<Void_fn>(kLoad)(); break;
    case Item::Save: Function<Void_fn>(kSave)(); break;
    case Item::Delete: Function<Void_fn>(kDelete)(); break;
    }
}

struct EscCall {
    LPARAM lparam;
    bool handled;
};

void EscapeNow(void *p)
{
    EscCall &c = *static_cast<EscCall *>(p);
    const int s = static_cast<unsigned char>(*kGameScreen);
    if (s == 0x54) { // a confirmation: its last button (No / Cancel)
        const int n = *kModalButtons;
        if (n > 0 && (c.lparam & (1 << 30)) == 0) {
            *kModalChoice = n - 1;
            *kModalDone = 1;
        }
        c.handled = true;
        return;
    }
    if (c.lparam == 0 || *kEditor == 1) {
        return; // posted by the game (lParam 0), or the editor: the game's own Esc
    }
    if ((c.lparam & (1 << 30)) != 0) { // held: repeats do nothing more
        c.handled = s == 0 || s == 1 || s == 0x48 || s == 0x49 || s == 0x14 || RightClickScreen(s) ||
                    MenuBackScreen(s);
        return;
    }
    if (s == 0) { // the main menu: the game quits at once; ours asks first (as Augustus does), the same box as Exit game
        c.handled = true;
        Exit(false);
        return;
    }
    if (MenuBackScreen(s)) {
        g_rightClick = true;
        c.handled = true;
        return;
    }
    if (s == 0x14) { // the China map: its selection let go as its right click does (0x489d20), else back to the city
        if (*kMapSelected != 0 || *kMapSelected2 != -1) {
            *kMapSelected = 0;
            *kMapSelected2 = -1;
            *kRedrawAll = 1;
        }
        else {
            Function<Void_fn>(kMapToCity)();
        }
        c.handled = true;
        return;
    }
    if (s == 1 || s == 0x48 || s == 0x49) {
        if (s != 1 || *kBuildType != 0 || *kBuilding != 0) {
            Function<Bool_fn>(kCityCancel)(); // as a right click: the tool dropped, the company or god let go
            c.handled = true;
            return;
        }
        c.handled = emperor::options_window::OpenPause();
        return;
    }
    if (RightClickScreen(s)) {
        g_rightClick = true;
        c.handled = true;
    }
}

void __cdecl InputMouse_detour()
{
    Original(o_InputMouse, kInputMouse)();
    if (g_rightClick) {
        g_rightClick = false;
        *kRightUp = 1;
    }
}

} // namespace

bool Attach()
{
    const Site *sites[] = {&kModalDialog, &kLeaveToMenu, &kReplay, &kLoad, &kSave, &kDelete, &kCityCancel, &kInputMouse,
                           &kMapToCity, &kTextWidth};
    for (const Site *s : sites) {
        if (!Matches(*s)) {
            core_log("game menu: %s at 0x%lx does not match; Esc, the pause menu and the File menu stay the game's",
                     s->name, s->address);
            return false;
        }
    }
    if (!emperor::options_window::Available()) {
        core_log("game menu: the options window is not available; Esc and the File menu stay the game's");
        return false;
    }
    if (!Install(kInputMouse, &InputMouse_detour, o_InputMouse)) {
        core_log("game menu: input_update_mouse could not be hooked; Esc and the File menu stay the game's");
        return false;
    }
    g_attached = true;
    core_log("game menu: Esc closes windows (as a right click), goes back on the menu screens (as a right click), "
             "asks before quitting on the main menu and opens the pause menu on the city screen; confirmations take "
             "Esc as No");
    return true;
}

bool Available()
{
    return g_attached && core::diag::Enabled(g_feature);
}

bool Escape(LPARAM lparam)
{
    if (!Available()) {
        return false;
    }
    EscCall c{lparam, false};
    if (!core::diag::Guarded(g_feature, &EscapeNow, &c)) {
        g_rightClick = false;
        core_log("game menu: off after an error; Esc is the game's own again");
        return false;
    }
    return c.handled;
}

bool OnLoop()
{
    if (!g_hasPending) {
        return false;
    }
    g_hasPending = false;
    if (!Available() || !core::diag::Guarded(g_feature, &RunPending, nullptr)) {
        core_log("game menu: the pause menu's item was not carried out (an error)");
    }
    return true;
}

void Request(Item item)
{
    g_pending = item;
    g_hasPending = true;
}

void __cdecl MenuReplay()
{
    if (!core::diag::Guarded(g_feature, [](void *) { Replay(false); }, nullptr)) {
        core_log("game menu: Replay map failed (an error)");
    }
}

void __cdecl MenuMainMenu()
{
    if (!core::diag::Guarded(g_feature, [](void *) { MainMenu(false); }, nullptr)) {
        core_log("game menu: Exit to main menu failed (an error)");
    }
}

void __cdecl MenuExit()
{
    if (!core::diag::Guarded(g_feature, [](void *) { Exit(false); }, nullptr)) {
        core_log("game menu: Exit game failed (an error)");
    }
}

} // namespace emperor::game_menu
