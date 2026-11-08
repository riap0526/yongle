#include "games/emperor/options_entry.h"

#include "core/log.h"
#include "games/emperor/options_text.h"
#include "games/emperor/options_window.h"
#include "games/emperor/site.h"

#include <algorithm>
#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/archive/resolution_ui_original_settings.md §12.2, §12.4 and
// §14.7 (tools/recon/menu_tables.py, Ghidra, capstone, 2026-11-08); calling conventions: docs/symbols.md.
namespace emperor::options_entry {
namespace {

namespace ow = emperor::options_window;
namespace ot = emperor::options_text;

// lang_get_string(group, index) 0x5278b0 (__cdecl; ret): our texts for the menu items we add.
const Site kLangString{"lang_get_string", 0x5278b0, {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x15, 0xcc, 0xd8}};
// menu_width(group) 0x535c70 (__cdecl; ret): the drop-down's width in 16-pixel tiles, measured once at start-up from
// the group's own texts (0x535bc0); the drop-down's drawing 0x449840 and its hit test 0x44a690 read it.
const Site kMenuWidth{"menu_width", 0x535c70, {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x04, 0x85, 0x18}};
// main_menu_click 0x4ec2b0, "Visit Website" (button 3): ShowWindow(game, SW_MINIMIZE) at 0x4ec336 and
// ShellExecuteA(game, "open", lang(9, 10), ...) at 0x4ec35b, both call dword ptr [import]; the rest (the result, the
// menu object's 0x5034b0) is the same for every button and stays.
const Site kWebsiteMinimize{"main_menu_website_minimize", 0x4ec336, {0xff, 0x15, 0x80, 0x93, 0x7a, 0x00, 0x6a, 0x05}};
const Site kWebsiteOpen{"main_menu_website_open", 0x4ec35b, {0xff, 0x15, 0x5c, 0x93, 0x7a, 0x00, 0x8b, 0x4c}};

using Lang_fn = const char *(__cdecl *)(int group, int index);
using Width_fn = int(__cdecl *)(int group);
using Handler_fn = void(__cdecl *)();
using ImageGetId_fn = int(__thiscall *)(void *, int);
using TextWidth_fn = int(__cdecl *)(const char *text, int font);
Lang_fn o_Lang = nullptr;
Width_fn o_Width = nullptr;

// The game's (checked by options_window, which is attached when we are).
const DWORD kImageGetId = 0x408170, kImages = 0x1c42130, kTextWidth = 0x527a80;
constexpr int kFontNormal = 0x1e14; // the drop-down's font (0x449840)

// The Options menu: group 2 of EmperorText.eng. Ours use text numbers the group does not have (it has 7).
constexpr int kOptionsGroup = 2, kTextGeneral = 20, kTextVideo = 21, kTextAudio = 22, kTextHotkeys = 23,
              kTextAutosave = 24;
constexpr int kAutosaveOn = 4, kAutosaveOff = 5; // the game's "Autosave - ON" / "- OFF" (0x449460)
const Var<int> kAutosaveDisabled{0x10de15c};      // not 0: no autosave (shown as "Autosave - OFF")
constexpr int kMainMenuGroup = 30, kTextWebsite = 4; // the main menu's "Visit Website" (0x84fa50)

// The game's dialogs, the fall-back when our window is not available: Speed, Display, Sound options; Autosave.
const DWORD kSpeedDialog = 0x402440, kDisplayDialog = 0x401ea0, kSoundDialog = 0x402030, kAutosaveToggle = 0x449460;

void __cdecl MenuGeneral()
{
    if (!ow::Open(ow::kPageGeneral, 0)) {
        reinterpret_cast<Handler_fn>(kSpeedDialog)();
    }
}
void __cdecl MenuVideo()
{
    if (!ow::Open(ow::kPageVideo, 0)) {
        reinterpret_cast<Handler_fn>(kDisplayDialog)();
    }
}
void __cdecl MenuAudio()
{
    if (!ow::Open(ow::kPageAudio, 0)) {
        reinterpret_cast<Handler_fn>(kSoundDialog)();
    }
}
const DWORD kMessagesDialog = 0x402690, kMessagesButtonHandler = 0x857efc;
void __cdecl MessagesButton()
{
    if (!ow::OpenCategory(ow::kPageGeneral, ot::kCategoryMessages)) {
        reinterpret_cast<Handler_fn>(kMessagesDialog)();
    }
}

void __cdecl MenuHotkeys()
{
    if (!ow::OpenHotkeys()) {
        core_log("options window: not available; the Options menu's Hotkeys does nothing"); // the game has none
    }
}

// A drop-down's item (16 bytes, as 0x81e2a0): y, text number, handler; +8 is 1 for Autosave (the game keeps the menu
// open after it, 0x44a3c0), +12 goes to button_param.
#pragma pack(push, 1)
struct MenuItem {
    short y, text;
    Handler_fn handler;
    int flags, param;
};
// A menu bar's menu (16 bytes, as 0x81e490): x0, x1, y, text group, items, count.
struct Menu {
    short x0, x1, y, group;
    MenuItem *items;
    int count;
};
#pragma pack(pop)
static_assert(sizeof(MenuItem) == 16 && sizeof(Menu) == 16, "the game's layouts");

// Single player (0x81e2a0 had Display, Sound, Speed, Autosave, Messages) and the modes without Autosave (0x81e2f0).
MenuItem g_items[] = {
    {0, kTextGeneral, &MenuGeneral, 0, 0},
    {20, kTextVideo, &MenuVideo, 0, 0},
    {40, kTextAudio, &MenuAudio, 0, 0},
    {60, kTextHotkeys, &MenuHotkeys, 0, 0},
    {80, kTextAutosave, reinterpret_cast<Handler_fn>(kAutosaveToggle), 1, 0},
};
MenuItem g_itemsNoAutosave[] = {
    {0, kTextGeneral, &MenuGeneral, 0, 0},
    {20, kTextVideo, &MenuVideo, 0, 0},
    {40, kTextAudio, &MenuAudio, 0, 0},
    {60, kTextHotkeys, &MenuHotkeys, 0, 0},
};
const DWORD kBars[] = {0x81e480, 0x81e4b0, 0x81e4e0, 0x81e510, 0x81e540, 0x81e570}; // menu_bar_select 0x449590
const DWORD kItemsSingle = 0x81e2a0, kItemsOther = 0x81e2f0;

const char *__cdecl Lang_detour(int group, int index)
{
    if (group == kOptionsGroup && index >= kTextGeneral) {
        switch (index) {
        case kTextGeneral: return ot::kMenuGeneral;
        case kTextVideo: return ot::kMenuVideo;
        case kTextAudio: return ot::kMenuAudio;
        case kTextHotkeys: return ot::kMenuHotkeys;
        case kTextAutosave: return o_Lang(group, *kAutosaveDisabled != 0 ? kAutosaveOff : kAutosaveOn);
        default: break;
        }
    }
    if (group == kMainMenuGroup && index == kTextWebsite) {
        return ot::kMainMenuOptions;
    }
    return o_Lang(group, index);
}

int g_ourWidth = 0; // tiles, measured at the first drop-down (the fonts are loaded by then)

int __cdecl Width_detour(int group)
{
    int w = o_Width(group);
    if (group != kOptionsGroup) {
        return w;
    }
    if (g_ourWidth == 0) {
        const int font = reinterpret_cast<ImageGetId_fn>(kImageGetId)(reinterpret_cast<void *>(kImages), kFontNormal);
        int px = 1;
        for (const char *t : {ot::kMenuGeneral, ot::kMenuVideo, ot::kMenuAudio, ot::kMenuHotkeys, o_Lang(kOptionsGroup, kAutosaveOn),
                              o_Lang(kOptionsGroup, kAutosaveOff)}) {
            px = std::max(px, reinterpret_cast<TextWidth_fn>(kTextWidth)(t, font));
        }
        g_ourWidth = px / 16 + 2; // as 0x535bc0
    }
    return std::max(w, g_ourWidth);
}

BOOL WINAPI KeepWindow(HWND, int)
{
    return TRUE; // "Visit Website" minimized the game before opening the browser
}

HINSTANCE WINAPI OpenOptions(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT)
{
    if (!ow::Open(ow::kPageGeneral, 0)) {
        core_log("options window: not available; the main menu's Options does nothing");
    }
    return reinterpret_cast<HINSTANCE>(33); // ShellExecute's success
}

bool CallTo(DWORD site, const void *target)
{
    BYTE code[6] = {0xe8, 0, 0, 0, 0, 0x90}; // call rel32; nop (in place of call dword ptr [import], 6 bytes)
    const DWORD rel = reinterpret_cast<DWORD>(target) - (site + 5);
    std::memcpy(code + 1, &rel, 4);
    return PatchCode(site, code, sizeof(code));
}


} // namespace

bool Attach()
{
    if (!ow::Available()) {
        core_log("options window: not available; the game's Options menu and main menu stay as they are");
        return false;
    }
    const Site *sites[] = {&kLangString, &kMenuWidth, &kWebsiteMinimize, &kWebsiteOpen};
    for (const Site *s : sites) {
        if (!Matches(*s)) {
            core_log("options window: %s at 0x%lx does not match; the game's Options menu and main menu stay", s->name,
                     s->address);
            return false;
        }
    }
    for (DWORD bar : kBars) {
        const Menu &m = *reinterpret_cast<const Menu *>(bar + sizeof(Menu)); // the bar's second menu: Options
        const DWORD items = reinterpret_cast<DWORD>(m.items);
        if (m.group != kOptionsGroup || !((items == kItemsSingle && m.count == 5) || (items == kItemsOther && m.count == 4))) {
            core_log("options window: the menu bar at 0x%lx is not as expected; the game's Options menu stays", bar);
            return false;
        }
    }
    if (!Install(kLangString, &Lang_detour, o_Lang) || !Install(kMenuWidth, &Width_detour, o_Width)) {
        core_log("options window: the menu texts could not be hooked; the game's Options menu stays");
        return false;
    }
    for (DWORD bar : kBars) {
        Menu &m = *reinterpret_cast<Menu *>(bar + sizeof(Menu));
        const bool single = reinterpret_cast<DWORD>(m.items) == kItemsSingle;
        m.items = single ? g_items : g_itemsNoAutosave; // .data: writable as it is
        m.count = single ? 5 : 4;
    }
    const bool mainMenu = CallTo(kWebsiteMinimize.address, reinterpret_cast<const void *>(&KeepWindow)) &&
                          CallTo(kWebsiteOpen.address, reinterpret_cast<const void *>(&OpenOptions));
    // The message list's "Click to change how messages are received" (messages_text_buttons 0x857ed8, button 1:
    // its left-click handler at +0x24 = 0x857efc is the game's Messages dialog 0x402690): our General > Messages.
    Handler_fn &messagesButton = *reinterpret_cast<Handler_fn *>(kMessagesButtonHandler);
    const bool messages = reinterpret_cast<DWORD>(messagesButton) == kMessagesDialog;
    if (messages) {
        messagesButton = &MessagesButton; // .data
    }
    else {
        core_log("options window: the message list's button is not as expected; it opens the game's Messages");
    }
    core_log("options window: the game's Options menu opens it (General Settings, Video, Audio, Hotkeys; Autosave as the "
             "game's); the main menu's Visit Website is Options%s",
             mainMenu ? "" : " (not changed: the code could not be written)");
    if (messages) {
        core_log("options window: the message list's button opens it at General > Messages");
    }
    return true;
}

} // namespace emperor::options_entry
