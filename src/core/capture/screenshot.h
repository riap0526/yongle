// Screenshots (R2.0, R2a; docs/resolution_ui/r2_screens.md §2). One key press saves two PNG files in shots\ next to
// the DLL (the game folder), with the same time in their names:
//   <tag>_game_<w>x<h>_<time>.png    the frame the game last presented, at its own resolution (e.g. 1024x768 on a
//                                    front-end screen, 1920x1080 in the city): for debugging
//   <tag>_screen_<w>x<h>_<time>.png  what the output shows on the screen (e.g. 2560x1440): the framed layout, the
//                                    status line; read back from the next frame drawn, before our panel is drawn
//                                    (the panel is never in either picture). For checking layouts.
// e.g. screen_0x00_game_1024x768_20261023-213045-123.png and screen_0x00_screen_2560x1440_20261023-213045-123.png.
// The game layer supplies the tag (SetTagProvider; "frame" without one). The game frame is copied on the calling
// (game) thread; encoding and writing happen on worker threads, which log one line per file.
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
bool Save(const void *pixels, int pitch, int w, int h, const char *label = nullptr);

// Any thread: saves one 16-bit RGB565 picture as <label>_<tag>_<kind>_<w>x<h>_<time>.png (e.g. a city buffer for a
// report), without a screen picture.
bool SaveOne(const void *pixels, int pitch, int w, int h, const char *label, const char *kind);

// Any thread: saves one 16-bit RGB565 picture as <fileName> next to the DLL (not in shots\), replacing it; written on
// a worker thread, which logs the result (R2d.1: the city picture for the front-end screens' background). wait: written
// on the calling thread before returning (when the process is about to end, which would cut a worker short).
bool SaveAs(const void *pixels, int pitch, int w, int h, const char *fileName, bool wait = false);

// Output thread: the screen picture read back for a Save (takes ownership of the surface).
void SaveScreen(SDL_Surface *surface, const char *stem, const char *time);

} // namespace core::capture
