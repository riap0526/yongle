// Our own settings panel (docs/resolution_ui/settings.md §5.2), drawn with Dear ImGui over the game's picture.
//
// Threads: the panel's ImGui state lives on the output thread (core/output, present = display or cap60), which draws
// it after the game's frame at the window's real resolution, so it is sharp whatever the game's resolution. The
// game's window messages arrive on the game thread: while the panel is open the window subclass
// (core/display/virtual_display) hands the input messages to QueueMessage instead of the game; they are translated
// into ImGui events on the output thread. Modifier keys are read with GetKeyState on the game thread, where that
// state is valid (the reason the official Win32 backend is not used: third_party.md).
//
// The game layer decides what "paused" means (Emperor: the main loop's step is skipped while the panel is open) and
// adds the panel's contents as sections. Nothing here knows a game address.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct SDL_Renderer;

namespace core::ui {

// Game thread. Opens or closes the panel (its hotkey, ui.panel_key); false when the panel cannot be shown (no output thread).
bool Toggle();
bool IsOpen();

// Game thread: an input message for the panel (mouse coordinates in real window client pixels).
void QueueMessage(UINT message, WPARAM wparam, LPARAM lparam);

// Output thread: draws the panel with the renderer (output size ow x oh). Call before presenting.
void Render(SDL_Renderer *renderer, int ow, int oh);
// Output thread: the panel failed (its guard caught an exception): it is closed and stays unavailable for the rest of
// the run, so the game is not left paused.
void Abandon();

// Output thread: the renderer is about to be destroyed (the window was resized).
void RendererLost();

// Whichever thread owns the renderer: draws a status line (UTF-8) with ImGui's font, in its own ImGui frame (so it is
// part of the picture: drawn before a screenshot reads the screen back and before the panel). strip: centred on a
// translucent dark strip x, y, w, h; otherwise centred horizontally in x..x+w with its top at y, on black behind the
// text only (as the game's GDI line). fontPx: the text height in output pixels. Core's text layer for now; the
// translation and text engine work can replace what draws it without changing the callers.
void RenderCaption(SDL_Renderer *renderer, int ow, int oh, const char *utf8, float x, float y, float w, float h,
                   float fontPx, bool strip);

// The panel's title and its sections (drawn on the output thread with ImGui calls; reading game state from there is
// for display only). At most 8 sections, added at start-up.
void SetTitle(const char *utf8);
using Section = void (*)();
void AddSection(const char *utf8Title, Section draw);

// A small window drawn by draw in the top left corner over the game, without input (output thread, while our panel
// is closed; e.g. the performance overlay, core/ui/perf_view.h).
void RenderOverlay(SDL_Renderer *renderer, int ow, int oh, Section draw);

} // namespace core::ui
