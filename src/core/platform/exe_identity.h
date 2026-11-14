// Which exe the game is. Once at start-up: the exe file's SHA-256, size and PE time
// stamp go to the log, every time (a problem report then always says which exe it was). When the SHA-256 is not the
// one the game layer was tested with (core_game_info.exe_sha256), the log says so with the caveat, and a one-line
// notice is shown for a while from the first frame on (core::output::SetNoticeFromFirstFrame). Nothing else changes:
// whether the game's hooks are installed is still decided by the game layer's byte checks. Patched exes (widescreen,
// resolution or animation fixes, other mods) are not told apart: there are too many to list.
// Kernel32 only (no bcrypt.dll), so it is safe under the loader lock. Nothing here knows a game.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Check() below, for the entry point (proxy/ddraw/ddraw.c). */
int core_exe_check(void);

#ifdef __cplusplus
}

#include <cstddef>
#include <cstdint>

namespace core::exe {

// SHA-256 of size bytes; hex is 64 lowercase digits and a 0.
void Sha256(const void *data, std::size_t size, char hex[65]);

// SHA-256 of a file (read in pieces); false when it cannot be read.
bool Sha256File(const wchar_t *path, char hex[65], std::uint64_t *size);

// From the deferred start-up work, before the game layer's attach: logs the exe and, when it is not the tested one,
// the caveat and the notice. True when it is the tested exe (or the game layer names none).
bool Check();

// What Check found (false before Check, or when it is not the tested exe).
bool Tested();

} // namespace core::exe

#endif
