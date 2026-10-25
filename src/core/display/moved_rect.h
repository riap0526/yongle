// A rectangle of a 16-bit surface shown somewhere else (R2b: the info windows the game always opens at one place,
// games/emperor/resolution.cpp). The game draws into a copy of the screen; the rectangle it drew (`from`) is then shown
// at `to`, and everything it drew outside `from` stays where it was drawn. Nothing here knows a game.
//
// Every copy stays inside the surface: Valid() is checked first, and each function checks it again and does nothing
// when it fails (the caller then draws the plain way).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>

namespace core::display {

struct Surface16 {
    BYTE *px;     // first row
    int pitch;    // bytes per row
    int w, h;     // pixels
    size_t bytes; // what px points to (at least pitch * (h - 1) + w * 2)
};

// Both rectangles non-empty, the same size, and inside the surface; the surface itself consistent.
bool MovedRectValid(const Surface16 &s, const RECT &from, const RECT &to);

// copy := back, then copy[from] := back[to] (what is on screen where the rectangle will be shown, so that its
// transparent pixels show the right thing). copy and back have the same layout (s gives back; copy has s.bytes).
bool MovedRectPrepare(BYTE *copy, const Surface16 &back, const RECT &from, const RECT &to);

// back[everything outside from] := copy, then back[to] := copy[from].
bool MovedRectCompose(const Surface16 &back, const BYTE *copy, const RECT &from, const RECT &to);

} // namespace core::display
