// [debug] window_background_test (black block diagnosis, 2026-11-07): the game window's class background brush
// (GCLP_HBRBACKGROUND) becomes magenta (RGB 255,0,255) while the setting is on, and the original brush comes back when
// it is switched off. The window's WM_ERASEBKGND goes to DefWindowProc, which fills the window's redirection surface
// with the class brush (docs/symbols.md, game_wndproc), so if the blocks that flash are that surface they flash
// magenta instead of black. Only the running class changes (nothing is saved anywhere); a failure switches the
// feature off (core/diag/guard.h) and leaves the original brush.
#pragma once

struct HWND__;

namespace core::diag::window_background {

// The drawing thread, often (each pass of its loop): applies or restores the test color when on differs from what
// is in effect, or when the window changed. hwnd null: nothing is done.
void Update(HWND__ *hwnd, bool on);

} // namespace core::diag::window_background
