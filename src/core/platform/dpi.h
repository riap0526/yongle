// DPI awareness for the game process. Old games are DPI unaware, so with display scaling above 100% Windows
// virtualizes their coordinates: Emperor at 125% presented only 819x614 of its 1024x768 frame
// (docs/testing/ddraw_wrapper_v1.md). Making the process DPI aware before its first window fixes that without
// the per-exe compatibility setting.
//
// All functions call user32, so none of them may run under the loader lock (see core/startup.h for when they
// are called). The DPI functions are resolved at run time, so this also works on Windows versions without them.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Makes the process DPI aware, once; later calls do nothing. Tries, in order: per-monitor v2
   (SetProcessDpiAwarenessContext, Windows 10 1703+), per-monitor v1 (same API, 1607), SetProcessDpiAwareness
   (shcore, 8.1) and SetProcessDPIAware (Vista). Stops at the first success, or when Windows reports that the
   awareness was already fixed (compatibility setting, manifest). Logs the state before and after. */
void core_dpi_apply(const char *when);

/* Logs the process and thread DPI awareness, the system DPI and the screen size as seen by this process. */
void core_dpi_log_state(const char *when);

/* Logs a window's position, client size, DPI and awareness, and its monitor (hwnd is an HWND). */
void core_dpi_log_window(void *hwnd, const char *when);

#ifdef __cplusplus
}
#endif
