// Work split across our own threads while the caller waits (R5: the city zoom's scaling, docs/resolution_ui/
// r5_city_zoom.md section 9.2). The game thread is the one that limits the frame rate (docs/archive/optimization.md
// section 1.8), and what it waits for here must be done before it goes on (the game draws its interface over the
// scaled city next), so the work is cut into pieces of rows that the caller and up to kMaxWorkers threads of ours
// take one by one; ForRows returns when every piece is done. The workers are ours for the processor affinity
// (core/platform/affinity: on the processors apart from the game's); with no processor apart (cpu_affinity =
// original on a one-processor install, or a one-core computer) everything runs on the caller, as before. A piece
// that fails on a worker (a hardware exception, core/diag Guarded) switches the workers off for the rest of the run
// (logged) and the whole job is done again on the caller. Nothing here knows a game.
#pragma once

#include "core/display/scaled_rect.h"

namespace core::platform::parallel {

constexpr int kMaxWorkers = 3;
constexpr int kRowsPerPiece = 16;

// core::display::ForRows: fn over [0, count) in pieces of kRowsPerPiece rows, on the caller and our workers (the
// workers start at the first call that can use them). One caller at a time (the game thread).
void ForRows(int count, core::display::RowsFn fn, void *ctx);

// How many workers ForRows uses now (0: everything on the caller), for the log.
int Workers();

// For the offline test: at most n workers from now on (0: none); the next call starts what is missing.
void LimitWorkers(int n);

} // namespace core::platform::parallel
