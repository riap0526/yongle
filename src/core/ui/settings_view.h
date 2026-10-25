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

// The panel was closed: an unanswered "keep these settings?" is answered with Revert.
void PanelClosed();

} // namespace core::ui
