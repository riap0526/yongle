// Screen output through SDL3 into the game's own window (CLAUDE.md: in the hook phase SDL3 only takes over the
// picture; the window, its messages and all input stay with the game's Win32 code).
//
// The SDL window wraps the existing HWND; the window procedure is given straight back to its previous owner (SDL
// subclasses the window when wrapping it; verified by tools/sdl_hwnd_test). Without its window procedure SDL does not
// notice size changes, so the output compares sizes and rebuilds the SDL window and renderer when needed
// (re-instating the current window procedure, which SDL_DestroyWindow resets). SDL is told not to touch the system
// timer resolution (SDL_HINT_TIMER_RESOLUTION=0); that is the engine's own setting (core/platform/timing.h).
//
// Two ways to present (emperor_engine.ini display.present):
//
//   original  Present() on the calling (game) thread, vsync off: exactly the frames the game presents (M3).
//
//   display / cap60  An output thread of our own owns SDL (it calls SDL_Init, so it is SDL's video thread). The game
//      thread only hands frames over with Submit(), which copies the picture into a triple buffer and returns: it
//      never waits for the GPU or for vsync, so the game's main loop, and with it the simulation (at 100% speed one
//      tick per loop iteration), runs exactly as before. The output thread takes the newest complete frame and
//      presents it with vsync (display: every refresh; cap60: every Nth refresh so that at most 60 per second).
//      Triple buffer: three slots; the producer fills its own slot and swaps it with the "ready" slot, the consumer
//      swaps its own slot with "ready" when that holds a new frame. Each side only ever touches its own slot, so a
//      frame is never shown half-written (no tearing from the hand-over) and neither side blocks the other.
//      FrameWanted() tells the game layer when the output thread is ready for a new frame (set after each present),
//      so frames are copied at most once per refresh, not once per game loop iteration.
//
// Framed full-screen pictures (R2a, core/display/framing.h): a frame can carry a Framing. Then only its picture box
// is drawn, straight from the frame's own pixels into its place in the framed layout for the output size (one
// resample), over the background layers the game layer composed for that layout (SetBackground; black until they
// match). A status line (SetCaption) is drawn over the picture's bottom edge. Without framing the whole frame is
// scaled into dst as before.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/config.h"

namespace core::output {

struct Framing {
    bool on = false;
    RECT picture{};      // the picture in frame pixels
    int below = 0;       // units of the framed block below the picture (e.g. a loading screen's status panel, drawn
                         // in the background): the layout is for the picture's width x (its height + below)
    bool labels = false; // debug: the block is drawn plain white (for the layout check, tools/verify)
    bool native = false; // R2d.1: the block at its own size when it fits (ComputeFrameLayout's native)
};

// Starts the output on hwnd. For display/cap60 this starts the output thread and waits until it is ready, still
// handling messages sent to the caller's windows meanwhile (SDL sends some while wrapping the window). Returns false
// (and logs why) if SDL or the renderer cannot be set up; nothing is shown then.
bool Init(HWND hwnd, core_scaling scaling, core_present_mode mode);

// True when frames go through the output thread (display/cap60).
bool Threaded();

// original: shows a 16-bit RGB565 image (w x h, pitch in bytes) scaled into dst (window client pixels) right away.
// client is the window's current client size; when it differs from the renderer's, the output is rebuilt.
void Present(const void *pixels, int pitch, int w, int h, const RECT &dst, SIZE client,
             const Framing &framing);

// display/cap60: hands a frame to the output thread (copied; returns at once).
void Submit(const void *pixels, int pitch, int w, int h, const RECT &dst, SIZE client, const Framing &framing);

// display/cap60: true (and cleared) when the output thread has presented the last frame and wants the next one.
bool FrameWanted();

// display/cap60: the next submitted frame is one the game insists on showing (e.g. a pressed button). It is kept on
// screen for at least holdMs -- what the game's own presentation guaranteed -- instead of being replaced by the next
// frame within the same refresh.
void HoldNextFrame(DWORD holdMs);

// Frames presented by the game (Present or Submit calls) and frames actually shown, since the last call.
void TakeCounts(long *presented, long *shown);

// Settings (P2): switches between display and cap60 while running (the output thread changes its vsync interval
// before its next frame). Returns false for a switch to or from original, which needs a restart.
bool SetPresentMode(core_present_mode mode);

// Display refresh rate seen by the output (0 until known).
int RefreshHz();

// Any thread: the background layers for a framed picture of pictureW x pictureH on a screenW x screenH output, laid
// out by core::display::ComputeFrameLayout: the outer fill (outerU x outerV) and the frame (frameU x frameV), RGB565
// with the given pitches. Copied; used while the output size and the picture size match.
void SetBackground(int screenW, int screenH, int pictureW, int pictureH, bool native, const void *outer, int outerPitch,
                   const void *frame, int framePitch);

// Any thread: a status line (UTF-8; "" clears it) for full-screen pictures: on a dark strip over the picture's
// bottom edge when framed; otherwise centred at frameY (frame pixels) of the scaled frame.
void SetCaption(const char *utf8, int frameY);

// Any thread: the next frame drawn is read back from the screen (before our panel is drawn) and saved by
// core/capture as <stem>_screen_<w>x<h>_<time>.png. stem and time are the capture's.
void RequestScreenCapture(const char *stem, const char *time);

} // namespace core::output
