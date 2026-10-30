// Screenshots (R2.0, R2a; docs/resolution_ui/r2_screens.md §2). One key press saves two PNG files in shots\ next to
// the DLL (the game folder), with the same time in their names:
//   <tag>_game_<w>x<h>_<time>.png    the frame the game last presented, at its own resolution (e.g. 1024x768 on a
//                                    front-end screen, 1920x1080 in the city): for debugging
//   <tag>_screen_<w>x<h>_<time>.png  what the output shows on the screen (e.g. 2560x1440): the framed layout, the
//                                    status line; read back from the next frame drawn, before our panel is drawn
//                                    (the panel is never in either picture). For checking layouts.
// e.g. screen_0x00_game_1024x768_20261023-213045-123.png and screen_0x00_screen_2560x1440_20261023-213045-123.png.
// The screen picture is read back before our panel and our performance overlay are drawn, so neither is in it; when
// the frame fills the window 1:1 with nothing around or over it, it equals the game picture and is not saved (O1b).
// The game layer supplies the tag (SetTagProvider; "frame" without one). The game frame is copied on the calling
// (game) thread; encoding and writing happen on one writer thread at normal priority, one picture at a time
// (O1b, docs/optimization.md), which logs one line per file with the time of each step.
// Nothing here knows a game.
#pragma once

#include <cstddef>

struct SDL_Surface;

namespace core::capture {

// The first part of the file name, e.g. "screen_0x14" (letters, digits, '_', '-' and '.'; anything else becomes
// '_'). Called on the game thread when a screenshot is taken.
using TagProvider = void (*)(char *out, size_t size);
void SetTagProvider(TagProvider provider);

// Game thread: saves a 16-bit RGB565 frame (w x h, pitch in bytes) and asks the output for the screen picture.
// Returns false (logged) when the game frame could not even be started (no picture, no memory, folder not
// writable). With a label (e.g. "report3", R2b.1) the file names start with it: report3_screen_0x01_game_....
// screen false: no screen picture (the caller knows it would be the same as the game picture; logged).
bool Save(const void *pixels, int pitch, int w, int h, const char *label = nullptr, bool screen = true);

// Any thread: saves one 16-bit RGB565 picture as <label>_<tag>_<kind>_<w>x<h>_<time>.png (e.g. a city buffer for a
// report), without a screen picture.
bool SaveOne(const void *pixels, int pitch, int w, int h, const char *label, const char *kind);

// Any thread: saves one 16-bit RGB565 picture as <fileName> next to the DLL (not in shots\), replacing it; written on
// a worker thread, which logs the result (R2d.1: the city picture for the front-end screens' background). wait: written
// on the calling thread before returning (when the process is about to end, which would cut a worker short).
bool SaveAs(const void *pixels, int pitch, int w, int h, const char *fileName, bool wait = false);

// Game thread, when the game ends (not from DllMain: the loader lock is held there): waits until the pictures queued
// so far are written, at most timeoutMs.
// Returns how many were not written (logged). O1: a large PNG can take several seconds on a slow computer.
int Flush(unsigned long timeoutMs);

// Any thread: false (logged as "<what> not saved") when 6 pictures are already queued or being written; checked before
// a frame is copied or the screen read back, so a picture that would be dropped costs nothing (O1, 2026-10-30).
bool HasRoom(const char *what);

// Output thread: the screen picture read back for a Save (takes ownership of the surface); readBackMs for the log.
void SaveScreen(SDL_Surface *surface, const char *stem, const char *time, unsigned long readBackMs);

} // namespace core::capture
