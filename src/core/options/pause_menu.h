// The pause menu (Yongle and Pericles): our menu window over the city (core/options/menu, drawn with the
// game layer's Painter) while the game is paused; its items are carried out at the start of the next game loop pass,
// and a confirmation answered No sends the player back to it. Shared by both game layers; nothing here knows a game
// address: the game layer gives its drawing, input origin, sounds, what each item does and the options key in a
// PauseGame (every field required), and the items' texts at Open.
//
// Input: core::display::SetModalInput has one slot. The game layer registers it (Pericles: Active / HandleMessage
// below; Yongle: its options window, which passes messages here while it is not open).
//
// Failure: input and drawing run under the guard "pause menu (core/options/pause_menu)"; when it trips the menu
// closes, the screen is drawn again and the menu stays unavailable (the game's own Esc and File menu stay).
//
// Threads: the game thread only (the window procedure, the game loop, screen_present).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/options/menu.h"
#include "core/options/window.h"

namespace core::options::pause_menu {

struct PauseGame {
    using Origin_fn = void (*)(int *x, int *y);
    using Void_fn = void (*)();
    using CanDraw_fn = bool (*)(unsigned long caller);
    using Act_fn = void (*)(int item);
    using Key_fn = bool (*)(unsigned vk, unsigned mods);

    PauseGame(const char *logName, Painter &painter, const MenuLook &look, Origin_fn origin, Void_fn beginDraw,
              Void_fn redraw, Void_fn click, CanDraw_fn canDraw, Act_fn act, Key_fn optionsKey)
        : logName(logName), painter(painter), look(look), origin(origin), beginDraw(beginDraw), redraw(redraw),
          click(click), canDraw(canDraw), act(act), optionsKey(optionsKey)
    {
    }

    const char *logName;  // e.g. "emperor", "zeus" (the attach line)
    Painter &painter;     // the game's art; drawn in window coordinates (640 x 480) after beginDraw
    MenuLook look;        // the menu's layout and look (both: the default layout, the darker bar under the mouse)
    Origin_fn origin;     // where the game's 640 x 480 dialogs start on the screen (the mouse is made relative to it)
    Void_fn beginDraw;    // before each drawing: the game's clip reset and the painter's origin
    Void_fn redraw;       // the game draws the whole screen again next frame (under the menu; after it closes)
    Void_fn click;        // the game's click sound (at every left press, as the game's own buttons)
    CanDraw_fn canDraw;   // screen_present's caller: false where nothing of ours may be drawn (a WM_PAINT frame of a
                          // display change, no draw buffer)
    Act_fn act;           // an item was clicked (0-based; Continue is the game layer's too): close and Request, or
                          // open the options window over the menu
    Key_fn optionsKey;    // a key the menu does not use (not Esc), with core/ui/hotkey kMod* modifiers: true when the
                          // game layer handled it (Yongle: its options key opens the options window over the menu); a
                          // game without an options window yet gives one that returns false
};

// Game layer, once (after its Painter's art is checked). The menu is available from then on.
void Attach(const PauseGame &game);
bool Available(); // attached and not switched off after an error

// Opens the menu with this title and items (the game is paused while it is open). true: open (or already open);
// false: not available.
bool Open(std::string title, std::vector<std::string> items);
bool Reopen(); // again with the last title and items (a confirmation answered No)
bool IsOpen();
void Close(const char *why); // logged as "pause menu: closed (why)"; the screen is drawn again

// Input (core::display::ModalInput's two functions; see above who registers them).
bool Active();
void HandleMessage(UINT m, WPARAM w, LPARAM l, int x, int y);

// Before the game's frame is presented: draws the menu over it. caller: the game address screen_present returns to.
void OnPresent(unsigned long caller);

// An item to carry out at the start of the next game loop pass (from the menu's act, after Close), so the game's
// function runs in the state it has when called from its own File menu. One at a time; a new one replaces it.
void Request(int item);
bool TakePending(int *item); // the game loop: true once for each Request

// After a confirmation the game layer showed: Yes runs action (what is logged as "<logPrefix>: <what>"); No reopens
// the pause menu when the confirmation came from it.
void AfterConfirm(bool yes, bool fromPause, void (*action)(), const char *logPrefix, const char *what);

// A confirmation's text padded with spaces on both sides (it is drawn centred) until it is at least `wide` pixels in
// the measuring function's font (at most 40 times): the game's box takes its size from its texts.
std::string PadToWidth(std::string text, int wide, int (*width)(const char *text));

// The game's confirmation box (Zeus modal_dialog_draw 0x5a04f0, Emperor 0x5bfa00, the same rules): its width W is the
// wider of the title (in titleMeasure's font) and the text (textWidth's) plus 20; the panel is W / 16 + 1 tiles (1 to 16
// pixels wider than W); the title is centred in W from titleX, the text in W - 10 from textX, the buttons around W / 2.
// So the text and buttons sit left of the panel's middle and the title right of it. The game layer gives the box's
// numbers and its fonts' measures (titleDraw: the title's drawing font; Zeus draws it in another font than it sizes
// the box by).
struct ConfirmBox {
    using Width_fn = int (*)(const char *text);
    ConfirmBox(int titleX, int textX, Width_fn titleMeasure, Width_fn titleDraw, Width_fn textWidth)
        : titleX(titleX), textX(textX), titleMeasure(titleMeasure), titleDraw(titleDraw), textWidth(textWidth)
    {
    }
    int titleX, textX;
    Width_fn titleMeasure, titleDraw, textWidth;
};

// Spaces added before and after the title and the text so that, with the box's rules, the title, the text and the
// buttons sit as near the panel's middle as they can, the text at least 20 pixels inside the panel's sides, and the box
// at least minWidth + 20 wide (0: no minimum). Both games' confirmations of ours go through it.
void LayOutConfirm(const ConfirmBox &box, std::string *title, std::string *text, int minWidth);

} // namespace core::options::pause_menu
