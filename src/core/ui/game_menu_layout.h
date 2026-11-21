// The layout of the games' own menu bars and drop-down items: the same in Emperor and Zeus (their menu code is the
// same, only the addresses differ). The game layers give a bar's menu our own item table.
#pragma once

#include <windows.h>

namespace core::ui::game_menu_layout {

using Handler_fn = void(__cdecl *)();

#pragma pack(push, 1)
// A drop-down's item (16 bytes): y, text number (in the menu's text group), handler; +8 a flag (Emperor's Autosave:
// 1, the menu stays open), +12 the game's button_param while the handler runs.
struct MenuItem {
    short y, text;
    Handler_fn handler;
    int flags, param;
};
// A menu bar's menu (16 bytes): x0, x1 (the game writes them while drawing), y, text group, items, count.
struct Menu {
    short x0, x1, y, group;
    MenuItem *items;
    int count;
};
#pragma pack(pop)
static_assert(sizeof(MenuItem) == 16 && sizeof(Menu) == 16, "the games' layouts");

} // namespace core::ui::game_menu_layout
