// Esc and the pause menu (0.2.0 item 7, K4; docs/archive/key_bindings.md section 8): Esc closes any window as a right click
// does; on the city screen with no window it opens our pause menu (games/emperor/options_window); the game's
// confirmation boxes take Esc as their last button (No / Cancel). The File menu's Replay map, Exit to main menu and
// Exit game, and the pause menu's items, go through here.
//
// Threads: the game thread (the window procedure and the game loop).
#pragma once

#include <windows.h>

namespace emperor::game_menu {

bool Attach();    // after options_window, before options_entry (its File menu asks Available)
bool Available(); // the confirmations and the game's functions are there (bytes checked)

// The keys translator's Esc (a WM_KEYDOWN of VK_ESCAPE): true when handled here (the game does not get it).
bool Escape(LPARAM lparam);

// The game loop, before each pass while nothing of ours pauses it: a pending pause menu item. true: one was carried
// out (the caller checks whether the pause menu is open again).
bool OnLoop();

enum class Item { Replay, Load, Save, Delete, MainMenu, Exit };
// From the pause menu, which has closed: carried out at the next OnLoop; a confirmation answered No (or Esc) opens
// the pause menu again.
void Request(Item item);

// The File menu's items (called by the game's drop-down, in the game's input pass).
void __cdecl MenuReplay();
void __cdecl MenuMainMenu();
void __cdecl MenuExit();

} // namespace emperor::game_menu
