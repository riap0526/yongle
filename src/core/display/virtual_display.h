// Virtual display for an emulated fullscreen DirectDraw game.
//
// The game asks for a display mode (e.g. 1024x768x16); the real desktop is never changed. Instead the game's
// window covers its monitor at native resolution, the frames are scaled into it (aspect kept, black bars), and the
// game is shown a consistent w x h world while the emulated mode is set:
//   - GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN) return the mode size;
//   - the game window's client and window rectangles are (0,0,w,h), and its client and screen coordinates coincide
//     (ClientToScreen/ScreenToClient are identity), as for a real fullscreen window at the origin;
//   - GetCursorPos and the coordinates of mouse messages are mapped from the real window into the mode, clamped to
//     the edges (so edge scrolling also works with the cursor in a black bar); WindowFromPoint maps back;
//   - setting and restoring the mode sends the WM_DISPLAYCHANGE and WM_SIZE a real mode change would cause;
//   - once attached, SetWindowPos/MoveWindow of the game window keep it covering the monitor (the game sizes its
//     window to each mode, which with a real mode change is the whole screen) -- or, windowed (R2e), where the
//     player put it.
// Our settings panel (core/ui/panel.h) is opened and closed here with its hotkey (ui.panel_key, default Insert;
// Esc also closes it). While it is open
// the game window's keyboard and mouse messages go to the panel instead of the game (Alt+F4 still reaches the game),
// and for the exe's calls GetCursorPos stays where the cursor was when the panel opened and GetAsyncKeyState /
// GetKeyState report no key down.
// The screenshot hotkey (debug.screenshot_key, default F11; also while the panel is open) saves the frame the game
// last presented (core/capture/screenshot.h); the key is kept from the game.
// The API hooks only answer calls made from the exe's own code; SDL and the system keep seeing the real window.
// Nothing here knows any game address.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/config.h"
#include "core/output/sdl_output.h"

namespace core::display {

// From start-up (DllMain): installs the user32 hooks (inactive until a mode is set). Kernel32 only.
void InstallHooks();

// SetCooperativeLevel(DDSCL_FULLSCREEN): starts the output on hwnd, subclasses it for mouse messages and makes it
// cover its monitor. Returns false if the output could not be started (the game then sees no picture; logged).
bool Attach(HWND hwnd, core_scaling scaling, core_present_mode present);

void SetMode(int w, int h);   // emulated SetDisplayMode
void ClearMode();             // RestoreDisplayMode, or release of the exclusive IDirectDraw
bool ModeActive();
SIZE ModeSize();              // the mode size, or the real screen size when no mode is set
SIZE NativeSize();            // the real size of the monitor the game window is on (the primary one before attach);
                              // windowed: the window's client size when it last came to rest (R2e)

// R2e: windowed (a window that can be moved and sized, at least 1024x768 of client area) or covering the monitor
// (game thread; display.window). The game is not told: it stays in its fullscreen mode. Windowed, the exe's own
// SetWindowPos / MoveWindow / SetWindowLong(GWL_STYLE) calls on its window are not applied, the cursor outside the
// window is kept off the mode's edges (no edge scrolling), and the window's place is saved when a move or size ends.
void SetWindowed(bool on);
bool Windowed();
// Windowed: the client area becomes w x h (at least 1024x768, at most the monitor's work area; the top left corner
// stays, moved back into the work area if needed) and is saved. Nothing when not windowed.
void SetWindowClient(int w, int h);

// A key of a game layer's own (e.g. its city picture key): when it is pressed with exactly its modifiers, fire runs
// on the game thread (the game window's message); the press, its release and its character are kept from the game
// (so F10 never reaches Windows' menu handling), also while our panel is open. Not while the panel waits for a new
// key, and not when it is the same as our panel key, screenshot key or report key (those come first). vk and mods are
// read at each key press: the owner changes them when its setting changes (vk 0: none). Up to 8 keys.
struct ActionKey {
    volatile unsigned vk;
    volatile unsigned mods;
    void (*fire)();
};
void AddActionKey(ActionKey *key);

// A filter for the key messages (WM_KEYDOWN, WM_SYSKEYDOWN) of the game window, called on the game thread before
// the game sees them; returning true swallows the message. One filter (a game layer's hotkeys); nullptr removes it.
using KeyFilter = bool (*)(UINT message, WPARAM wparam, LPARAM lparam);
void SetKeyFilter(KeyFilter filter);

// A filter for the mouse messages of the game window (movement, buttons, the wheel), called on the game thread after
// our panel and the framed-picture background had them, with the position in the mode's (the game's) coordinates;
// returning true swallows the message. One filter (a game layer's); nullptr removes it.
using MouseFilter = bool (*)(UINT message, WPARAM wparam, int x, int y);
void SetMouseFilter(MouseFilter filter);

// A modal window of a game layer's own (e.g. its options window): while active() is true, the game window's keys,
// characters and mouse messages go to message() on the game thread instead of the game (mouse positions in the mode's
// coordinates; the wheel too), after our panel and the action keys had them; Alt+F4 still reaches the game. A button
// released after the window closed is kept from the game when its press went to the window. One; nullptr removes it.
struct ModalInput {
    bool (*active)();
    void (*message)(UINT message, WPARAM wparam, LPARAM lparam, int x, int y);
};
void SetModalInput(const ModalInput *input);

// Called on the game thread when the game window gets the focus back (WM_ACTIVATEAPP becoming active, WM_SETFOCUS),
// before the game and our panel see the message: e.g. what a game layer paused when the focus went goes on, whether
// or not the panel is open. One handler; nullptr removes it.
using FocusHandler = void (*)();
void SetFocusHandler(FocusHandler handler);

// Shows a frame of the current mode (16-bit RGB565, w x h, pitch in bytes) scaled into the window: right away
// (present=original) or through the output thread (display/cap60; see core/output/sdl_output.h).
// how: what handed it over (a string literal: "Blt", "BltFast", "Unlock", ...), for the frame check
// (core/diag/report.h).
void Present(const void *pixels, int pitch, int w, int h, const char *how = nullptr);

// Game thread: frames presented from now on are framed full-screen pictures (or not): the output draws the picture
// box in the framed layout (core/display/framing.h) and the mouse is mapped through the picture's place on the
// screen; presses and the wheel on the background around it are kept from the game.
void SetFraming(const core::output::Framing &framing);

// The game window's real client size (the output's size), or 0 x 0 before attach.
SIZE ClientSize();

// R4: where the mode's picture lies on the game window and how much it is enlarged -- for drawing at the output's own
// resolution over the scaled picture (the Chinese text layer, docs/resolution_ui/display_scale.md §6.2) and for our
// panel. A framed full-screen picture (SetFraming) gives its box and that box's place. Any thread.
struct Placement {
    RECT box;            // the part of the mode shown (mode pixels): the whole mode, or a framed picture's box
    double x, y, w, h;   // where that part is on the window (real client pixels)
    double scale;        // window pixels per mode pixel (w / box width)
    bool framed;
};
bool ModePlacement(Placement *out); // false before a mode is set or while the window has no size
// Real client pixels -> mode pixels (clamped to the picture, as the mouse is) and back (the centre of the pixel).
POINT ClientToMode(POINT client);
POINT ModeToClient(POINT mode);

// The memory of a presented frame is about to be freed (core/ddraw: a primary surface is released), so a screenshot
// must not read it any more.
void FrameGone(const void *pixels);

} // namespace core::display
