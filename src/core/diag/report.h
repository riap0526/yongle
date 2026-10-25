// Diagnostics for problems that come and go (R2b.1, docs/archive/resolution_ui_r2b1.md): the flashing black blocks seen once
// in a large city. Nothing here knows a game.
//
// Frame integrity, two checks:
//   - per game frame: the game layer marks the start and the end of each of its frames (GameFrameBegin/End, one
//     main-loop pass); every frame handed to the output inside one is noted (NotePresent, with how: Blt, BltFast,
//     Unlock, Blt(colorfill)). Two or more in one game frame means the output may have shown a picture the game had
//     not finished (e.g. filled black, then drawn): logged (the first 10 in full, then counted).
//   - per output frame: the output's copy of a frame carries a sequence number that is odd while it is written
//     (core/output); the output thread checks it before and after drawing. A change means it drew a copy that was
//     being overwritten: logged the same way (NoteTornFrame).
// Both counts are in the periodic rate line (TakeIntegrity).
//
// Window events: activation (alt-tab), focus, minimise / restore and display changes of the game window are logged
// with their time (at most 400 lines a run).
//
// Report key (debug.report_key, default F12): BeginReport marks the log ("==== REPORT #n ===="); the caller saves
// the pictures it has (core/output: the frame shown and the screen), and the game thread takes the request
// (TakeReportRequest) to log its statistics and save what only it can (e.g. a city buffer).
#pragma once

#include <cstddef>

namespace core::diag {

// Game thread.
void GameFrameBegin();
void GameFrameEnd();
void NotePresent(const char *how); // how: a string literal

// Output thread: the copy drawn changed while it was drawn (sequence before / after; odd = being written).
void NoteTornFrame(unsigned before, unsigned after, int w, int h);

// The counts since the last call: game frames with 2+ presents, torn output frames.
void TakeIntegrity(long *multiPresent, long *torn);

// The game window's message procedure (any thread that runs it): logs the window events listed above.
void LogWindowEvent(unsigned message, std::size_t wparam, std::ptrdiff_t lparam);

// Any thread: starts report n (1, 2, ...), logged with why (e.g. "F12"), and asks the game thread for its part.
int BeginReport(const char *why);
// Game thread: the report number waiting for the game's part, or 0.
int TakeReportRequest();

} // namespace core::diag
