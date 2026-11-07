// The settings part of our panel (docs/resolution_ui/settings.md §5.2): draws every item of the settings model
// (core/settings/settings.h) that is shown in the panel, grouped by section, with how a change takes effect.
//
// - On/off and lists apply when changed; sliders when released (or with Apply); the custom resolution with Apply.
//   Every applied change is saved to emperor_engine.ini.
// - "Re-layout" items (resolution, display scale) are applied, then the panel asks whether to keep them: Keep saves
//   them; Revert, closing the panel or 15 seconds without an answer restore the previous values (as Windows does
//   for a new display resolution). Meanwhile the game draws frames (WantsGameFrames) so the result can be seen.
// - Items that need a restart say so once changed. "Restore defaults" sets every item that the panel can change.
// Output thread (ImGui), except WantsGameFrames and PanelClosed.
#pragma once

namespace core::ui {

// Draws the settings (inside the panel's window). scale is the panel's size factor.
void DrawSettings(float scale);

// Game thread (any thread): true while the panel asks to keep a new layout and shortly after, so the paused game
// draws frames that show it.
bool WantsGameFrames();

// The panel was closed: an unanswered "keep these settings?" is answered with Revert (and a key change is cancelled).
void PanelClosed();

// Key settings (city picture, docs/archive/city_export.md section 5.3): "Change" waits for the next key with
// the modifiers held (Ctrl, Shift, Alt); Esc cancels; a key already used by another of our key settings is refused.
// Any thread: true while the panel waits for a key (the window subclass then gives every key message to the panel),
// and afterwards until the key that ended the wait is released (so our key polls do not take it for a new press).
bool CapturingKey();
// Output thread (the panel's input): a key went down while CapturingKey(), with the modifiers held then.
void CaptureKey(unsigned vk, bool ctrl, bool shift, bool alt);
// From the game layer at start-up: a sentence for a key (and modifiers, core/ui/hotkey.h kMod*) the game itself uses,
// shown when such a key is chosen; nullptr when the game does not use it.
using GameKeyNote = const char *(*)(unsigned vk, unsigned mods);
void SetGameKeyNote(GameKeyNote fn);

} // namespace core::ui
