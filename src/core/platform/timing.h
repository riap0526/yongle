// System timer diagnostics. Games of this era pace frames and simulation with timeGetTime, whose granularity is
// the system clock-interrupt period: 15.625 ms by default, finer while any process has raised it (timeBeginPeriod).
// Emperor never raises it itself (docs/performance.md), so its timing depends on what else is running.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Current system timer resolution in microseconds (NtQueryTimerResolution), or 0 if unavailable. Does not wait. */
unsigned core_timer_resolution_us(void);

/* timeBeginPeriod(1) for this process (emperor_engine.ini timing.high_resolution_timer), once; logged. Not under the
   loader lock (winmm): called from the exe entry hook. */
void core_timer_begin(void);

/* The matching timeEndPeriod(1), if core_timer_begin raised it; later calls do nothing. Called when the game's main
   function returns, and as a fallback when the DLL detaches. */
void core_timer_end(void);

#ifdef __cplusplus
}
#endif
