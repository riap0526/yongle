// Programs known to draw on the screen over or beside a game (overlays, recorders, streamers, desktop tools), for the
// present mode log (core/diag/present_mode.h, black block diagnosis, 2026-11-07). Only a fixed list is compared and
// only the names that match are reported; no other process is named or logged. The list is kList in
// screen_programs.cpp: add entries there.
#pragma once

#include <cstddef>

namespace core::diag::screen_programs {

// The list entry whose executable name matches exe (the file name only, any case), or null.
const char *Match(const wchar_t *exe);

// The running programs of the list as "name (exe), ..." or "none of the list", and the processes looked at. One
// Toolhelp snapshot; the caller decides how often (it takes about a millisecond). False when the snapshot failed.
bool Find(char *out, std::size_t size, int *processes);

} // namespace core::diag::screen_programs
