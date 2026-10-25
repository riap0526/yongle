// R2b.1-7: small layout faults the original game has on every resolution (docs/archive/resolution_ui_r2b1.md, R2b.1-7).
// Each fix moves something the game draws and, where it takes clicks, the place it takes them together; nothing is
// saved, and without our DLL the game is as before.
//   - Messages screen (0x11): without the third text button (multiplayer only), "Click to change how messages are
//     received" -- its frame and its text -- is centred under "Delete opened messages" instead of standing to the
//     left of the empty place.
//   - Mill window: the list of foods starts kMillListShift pixels lower, as far below its header line as the
//     Warehouse's list (measured in screenshots of the seventh R2b.1 test).
//   - Build submenu (screen 0x06): each item's name and cost kSubmenuTextShift pixels lower, in the middle of its
//     frame instead of against its top.
#pragma once

namespace emperor::ui_fixes {

// Pixels the Mill's list moves down (the game's 0x73 below the window's layout top becomes 0x73 + this).
constexpr int kMillListShift = 2;

// Pixels the build submenu's names and costs move down within their frames.
constexpr int kSubmenuTextShift = 2;

// Checks the game's bytes and applies the fixes whose bytes match (each is logged). Returns false when none could.
bool Attach();

// Once per main-loop iteration (game thread), before the game's pass: the messages screen's buttons for the current
// state of the third button.
void OnLoop();

} // namespace emperor::ui_fixes
