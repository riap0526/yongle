#include "games/emperor/options_entry.h"

#include "core/log.h"
#include "games/emperor/city_picture.h"
#include "games/emperor/keys.h"
#include "games/emperor/options_text.h"
#include "games/emperor/options_window.h"
#include "games/emperor/site.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>

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
// The File menu: group 1 (19 texts); ours use numbers it does not have. They go after Save game (text 4).
constexpr int kFileGroup = 1, kTextSaveGame = 4, kTextCityShot = 40, kTextMinimapShot = 41;
bool g_optionsOn = false, g_fileOn = false; // which of our menus are in
// The pause banner: group 13, string 2 "Game paused ('P' key continues)" (FUN_005b6b50 0x5b6ee9 -> 0x535f50 ->
// lang_text_draw_centered 0x528890 -> lang_set_current_text -> lang_get_string).
constexpr int kPauseGroup = 13, kTextPaused = 2;

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

void __cdecl MenuCityShot()
{
    emperor::city_picture::RequestCity(); // the menu has put the city screen back before calling (0x44a3c0)
}
void __cdecl MenuMinimapShot()
{
    emperor::city_picture::RequestMinimap();
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
// The File menu with our two items: the game's items (0x81e0b0: 6; 0x81e110: 7, with Proverbs) copied at Attach,
// ours inserted after Save game, every item 20 pixels below the one before.
const DWORD kFileItems6 = 0x81e0b0, kFileItems7 = 0x81e110;
MenuItem g_file6[8], g_file7[9];

void MakeFileMenu(const MenuItem *game, int count, MenuItem *out)
{
    int n = 0;
    for (int k = 0; k < count; ++k) {
        out[n++] = game[k];
        if (game[k].text == kTextSaveGame) {
            out[n++] = MenuItem{0, kTextCityShot, &MenuCityShot, 0, 0};
            out[n++] = MenuItem{0, kTextMinimapShot, &MenuMinimapShot, 0, 0};
        }
    }
    for (int k = 0; k < n; ++k) {
        out[k].y = static_cast<short>(20 * k);
    }
}

const DWORD kBars[] = {0x81e480, 0x81e4b0, 0x81e4e0, 0x81e510, 0x81e540, 0x81e570}; // menu_bar_select 0x449590
const DWORD kItemsSingle = 0x81e2a0, kItemsOther = 0x81e2f0;

const char *__cdecl Lang_detour(int group, int index)
{
    if (group == kPauseGroup && index == kTextPaused) { // the pause banner: the player's pause keys
        const char *t = emperor::keys::PauseText();
        if (t) {
            return t;
        }
    }
    if (g_fileOn && group == kFileGroup && (index == kTextCityShot || index == kTextMinimapShot)) {
        return index == kTextCityShot ? ot::kMenuCityScreenshot : ot::kMenuMinimapScreenshot;
    }
    if (g_optionsOn && group == kOptionsGroup && index >= kTextGeneral) {
        switch (index) {
        case kTextGeneral: return ot::kMenuGeneral;
        case kTextVideo: return ot::kMenuVideo;
        case kTextAudio: return ot::kMenuAudio;
        case kTextHotkeys: return ot::kMenuHotkeys;
        case kTextAutosave: return o_Lang(group, *kAutosaveDisabled != 0 ? kAutosaveOff : kAutosaveOn);
        default: break;
        }
    }
    if (g_optionsOn && group == kMainMenuGroup && index == kTextWebsite) {
        return ot::kMainMenuOptions;
    }
    return o_Lang(group, index);
}

int g_ourWidth = 0, g_fileWidth = 0; // tiles, measured at the first drop-down (the fonts are loaded by then)

// The width in tiles of the widest text, as 0x535bc0 measures a group's own.
int Tiles(std::initializer_list<const char *> texts)
{
    const int font = reinterpret_cast<ImageGetId_fn>(kImageGetId)(reinterpret_cast<void *>(kImages), kFontNormal);
    int px = 1;
    for (const char *t : texts) {
        px = std::max(px, reinterpret_cast<TextWidth_fn>(kTextWidth)(t, font));
    }
    return px / 16 + 2;
}

int __cdecl Width_detour(int group)
{
    int w = o_Width(group);
    if (g_optionsOn && group == kOptionsGroup) {
        if (g_ourWidth == 0) {
            g_ourWidth = Tiles({ot::kMenuGeneral, ot::kMenuVideo, ot::kMenuAudio, ot::kMenuHotkeys,
                                o_Lang(kOptionsGroup, kAutosaveOn), o_Lang(kOptionsGroup, kAutosaveOff)});
        }
        return std::max(w, g_ourWidth);
    }
    if (g_fileOn && group == kFileGroup) {
        if (g_fileWidth == 0) {
            g_fileWidth = Tiles({ot::kMenuCityScreenshot, ot::kMenuMinimapScreenshot});
        }
        return std::max(w, g_fileWidth);
    }
    return w;
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

// The Options menu, the main menu and the message list's button (the options window).
bool OptionsWanted()
{
    if (!ow::Available()) {
        core_log("options window: not available; the game's Options menu and main menu stay as they are");
        return false;
    }
    for (const Site *site : {&kWebsiteMinimize, &kWebsiteOpen}) {
        if (!Matches(*site)) {
            core_log("options window: %s at 0x%lx does not match; the game's Options menu and main menu stay",
                     site->name, site->address);
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
    return true;
}

void OptionsIn()
{
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
}

// The File menu's screenshot items (0.2.0 item 6, docs/archive/game_notices.md section 3.4): when the screenshots are there,
// in the single-player File menus (0x81e0b0, 0x81e110; the other bars' File menus, e.g. multiplayer, stay).
bool IsSinglePlayerFile(const Menu &m)
{
    const DWORD items = reinterpret_cast<DWORD>(m.items);
    return m.group == kFileGroup && ((items == kFileItems6 && m.count == 6) || (items == kFileItems7 && m.count == 7));
}

bool FileWanted()
{
    if (!emperor::city_picture::Available()) {
        core_log("file menu: the screenshots are not available; the game's File menu stays");
        return false;
    }
    int found = 0;
    for (DWORD bar : kBars) {
        found += IsSinglePlayerFile(*reinterpret_cast<const Menu *>(bar)) ? 1 : 0; // the bar's first menu: File
    }
    if (found == 0) {
        core_log("file menu: no File menu as expected; the game's File menu stays");
        return false;
    }
    return true;
}

void FileIn()
{
    MakeFileMenu(reinterpret_cast<const MenuItem *>(kFileItems6), 6, g_file6);
    MakeFileMenu(reinterpret_cast<const MenuItem *>(kFileItems7), 7, g_file7);
    int changed = 0;
    for (DWORD bar : kBars) {
        Menu &m = *reinterpret_cast<Menu *>(bar);
        if (!IsSinglePlayerFile(m)) {
            continue;
        }
        const bool seven = reinterpret_cast<DWORD>(m.items) == kFileItems7;
        m.items = seven ? g_file7 : g_file6;
        m.count = seven ? 9 : 8;
        ++changed;
    }
    core_log("file menu: Save city screenshot and Save minimap screenshot added after Save game (%d menu bar(s))", changed);
}

} // namespace

bool Attach()
{
    for (const Site *site : {&kLangString, &kMenuWidth}) {
        if (!Matches(*site)) {
            core_log("menus: %s at 0x%lx does not match; the game's menus stay as they are", site->name, site->address);
            return false;
        }
    }
    const bool options = OptionsWanted(), file = FileWanted();
    if (!options && !file) {
        return false;
    }
    if (!Install(kLangString, &Lang_detour, o_Lang) || !Install(kMenuWidth, &Width_detour, o_Width)) {
        core_log("menus: the menu texts could not be hooked; the game's menus stay as they are");
        return false;
    }
    g_optionsOn = options;
    g_fileOn = file;
    if (options) {
        OptionsIn();
    }
    if (file) {
        FileIn();
    }
    return options;
}

} // namespace emperor::options_entry
