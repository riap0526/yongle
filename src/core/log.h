// Thread-safe line logger shared by the C proxy and the C++ core.
// Writes to <folder>\<name> (our files' folder, core/platform/files, or the directory of a module), appending, one
// line per call, prefixed with time and thread id.
#pragma once

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the log next to the DLL identified by module (an HMODULE). Safe to call from DllMain. A log over 4 MB is first
   renamed to <name>.old.<ext> (replacing the previous one) and a new one starts. */
void core_log_open(void *module, const char *file_name);

/* The same in folder (with a trailing backslash). Lines logged before, after core_log_begin_buffer, are written first. */
void core_log_open_folder(const wchar_t *folder, const char *file_name);

/* From DllMain before the log's folder is known (core/platform/files): lines are kept in memory (64 KB) until
   core_log_open_folder. */
void core_log_begin_buffer(void);

void core_log_close(void);
void core_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Formats an address as "module+0xoffset (0xaddress)" into buf; returns buf. */
const char *core_describe_address(const void *addr, char *buf, unsigned len);

#ifdef __cplusplus
}
#endif
