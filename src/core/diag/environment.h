// What the process runs among (O1a, docs/archive/optimization.md): which other programs' modules are loaded into it (overlays,
// recorders, injected hooks), how much processor time our threads actually get, and the process's resources over a
// long session. Only logs; nothing here changes how the game runs. Nothing here knows a game: the game layer calls
// TakeWindow from its rate log.
#pragma once

#include <cstddef>

namespace core::diag::env {

// Writer: the screenshot writer (core/capture). Workers: the parallel workers (core/platform/parallel), up to
// kMaxWorkers threads measured together (their processor time summed, so it can pass 100%).
enum class Thread { Game, Output, Writer, Workers };
constexpr int kMaxWorkers = 4;

// Once at start-up (any thread): logs the modules from outside Windows' folder (with the known overlays named), the
// power source and plan, the process's priority class and power throttling; later loads are noted as they happen and
// logged with the next TakeWindow.
void Start();

// On the thread itself: the thread to measure (called again: the newer thread; Workers: one more worker, up to
// kMaxWorkers). Logs its processor affinity mask.
void RegisterThread(Thread which);

// On the measured thread, once per pass or frame: which kind of core it runs on (hybrid processors: performance or
// efficiency cores). Cheap (one processor-number query). On the game thread it also looks at the process's affinity
// mask at most once a second and logs a line when it changed (e.g. set in Task Manager), with each thread's mask.
void NoteCore(Thread which);

// For the rate log (any thread, one caller): "game thread 85% of the time on a processor (P cores 100%), output
// thread 12%, process 1.4 of 20 logical processors (affinity 0x1), system 23% busy" since the last call (the screenshot
// writer too once it runs; the affinity is the process's mask now); modules loaded since the last
// call and changes of the priority class, power throttling or power source are logged as lines of their own; every
// kResourceMs a "resources" line (working set, private bytes, address space, handles, GDI and USER objects).
void TakeWindow(char *out, std::size_t size);

constexpr unsigned long kResourceMs = 60000;

} // namespace core::diag::env
