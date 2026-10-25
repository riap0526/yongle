// R3: the game's own Display options dialog (game_screen 0x0a) with our display settings
// (docs/archive/resolution_ui_r3.md; addresses and evidence: docs/symbols.md).
//
// The original dialog has four buttons: Full screen / Windowed screen, 800 x 600, 1024 x 768 and Cancel. Ours, drawn
// with the game's own panel, button frames and fonts in the same place:
//   Windowed screen / Full screen   our window mode (as F5), saved at once; the dialog stays open
//   1024 x 768 resolution           display.resolution = 1024x768 (the game's own size)
//   Resolution: W x H               the size the game draws now
//   Display scale: 125%  [v][^]     (or Auto (135%) / off) the volume arrows of Sound options: display.resolution =
//                                   scaled with the scale one step (5 %) smaller / larger
//   [ 100% ] [ Auto ]               scaled, scale 100 / auto
//   [ OK ] [ Cancel ]               OK keeps the changes (saved to emperor_engine.ini); Cancel puts back the resolution
//                                   and scale the dialog opened with. Closing it any other way (Esc) is a Cancel.
// The changes apply while the dialog is open (the game is laid out again behind it); they are only saved with OK.
// 800 x 600 is gone (playtest 2026-10-25: never 800x600; docs/archive/simulation.md §9.1).
//
// How: display_options_draw 0x5ba9c0 is hooked (our layout), and the three places in screen_handle_input that hand
// the dialog's button table (display_options_buttons 0x81ba50, 4 entries) to the game's button functions get ours
// (8 entries, the same 28-byte layout, our handlers). Failure: the original table and drawing come back (the
// game's own four buttons; F5 and F7 still go to our filters as before).
#pragma once

namespace emperor::display_options {

// Checks the game's bytes and installs the dialog (logged). False: the game's own dialog stays.
bool Attach();

// Once per main-loop iteration (game thread), before the game's pass: a dialog closed without OK puts the settings
// back.
void OnLoop();

// Pure parts, for the offline test.

// The largest scale (100..250, steps of 5) at which native / scale is still at least min (100 when none).
int MaxScale(int nativeW, int nativeH, int minW, int minH);
// The scale one step away from current (dir -1 or +1; current 0 = auto, which steps from the scale it chose, to
// the next 5 % step: 133 % -> 135 % / 130 %), kept within 100..MaxScale. Returns current's effective value when there is no step to take.
int StepScale(int current, int dir, int nativeW, int nativeH, int minW, int minH);
// The two status lines ("Resolution: 2048 x 1152", "Display scale: 125%" / "Display scale: Auto (135%)" /
// "Display scale: off"), in the game's character set (plain ASCII).
void StatusLines(int screenW, int screenH, bool scaled, int scale, int effectiveScale, char *line1, int size1,
                 char *line2, int size2);

} // namespace emperor::display_options
