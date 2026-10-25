// Thread-safe line logger shared by the C proxy and the C++ core.
// Writes to <directory of this DLL>\<name>, appending, one line per call, prefixed with time and thread id.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the log next to the DLL identified by module (an HMODULE). Safe to call from DllMain. */
void core_log_open(void *module, const char *file_name);
void core_log_close(void);
void core_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Formats an address as "module+0xoffset (0xaddress)" into buf; returns buf. */
const char *core_describe_address(const void *addr, char *buf, unsigned len);

#ifdef __cplusplus
}
#endif
