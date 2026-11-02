// Processor affinity (B plan, docs/archive/affinity.md): GOG's compatibility settings (SingleProcAffinity) keep the whole
// process on one logical processor, so our output thread took its time from the game thread on that one processor
// (docs/archive/optimization.md sections 1.7-1.8). The game's threads and Miles's stay on that original processor; only our
// own threads (an allow-list: the output, the screenshot writer, the stall watchdog, the city background) run on the
// others. Every other thread -- the game's, Miles's, Windows' helpers, the graphics driver's -- is put on the original
// mask when it starts (DLL_THREAD_ATTACH) and again by a periodic scan. Any failure puts the process back on its
// original mask, as without this feature. Nothing here knows a game.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* From DllMain on the thread itself (loader lock held): DLL_THREAD_ATTACH puts a new thread on the original mask (a
   graphics thread on ours); DLL_THREAD_DETACH takes one of ours off the list.
   System calls only; nothing is logged or allocated. */
void core_affinity_thread_attach(void);
void core_affinity_thread_detach(void);

#ifdef __cplusplus
}

#include <cstddef>
#include <cstdint>

namespace core::platform::affinity {

enum class Mode { Auto, Original }; // [compat] cpu_affinity

// Once, on the game thread, before our output thread starts (not under the loader lock): with Auto and a process on
// fewer processors than the system has, every other thread is suspended for a moment while the process is widened and
// each thread put back on the original mask (docs/archive/affinity.md section 3.3). Threads that start in a graphics module
// run on ours too (first loaded while the renderer was made, the display driver's (D3DKMT) or from its folder, or a
// Direct3D runtime module; section 5.1). Logged; on a failure the process stays on (or goes back to) its original
// mask.
void Start(Mode mode);

// First thing on each of our own threads: puts it on the allow-list (and on our mask once Start has run). name is a
// string literal (for the log).
void OurThread(const char *name);

// The stall watchdog's loop, about every 100 ms: once the feature runs, scans the process's threads every second for
// the first minute and every 5 s after (docs/archive/affinity.md section 3.6). Never while a thread is suspended for a sample
// (the watchdog does both, one after the other).
void Poll();

// Output thread, around making the window and the renderer: the modules first loaded meanwhile belong to the graphics
// stack. At the end every graphics module is logged with how it was found.
void BeginGraphicsSetup();
void EndGraphicsSetup();

// The loader's notification (core/diag/environment; loader lock held): a module named name (length characters, its
// file name without the folder) was loaded.
void NoteModuleLoaded(const wchar_t *name, unsigned long length);

// For the offline test: how a module (full path) counts as graphics (1 renderer, 2 driver, 4 runtime; 0 not), and the
// display drivers' DLLs Start found (nullptr past the last).
unsigned GraphicsHow(const wchar_t *path);
const wchar_t *DriverPath(int i);

// For the rate log's cpu line (one caller, any thread): "; affinity: game thread and 11 others on 0x1 (others 6%:
// amdxx32.dll 4% in 3, ...), ours on 0xffffc 15% in 3" since the last call; "" when the feature does not run.
void TakeWindow(char *out, std::size_t size);

// Our mask for an original one (docs/archive/affinity.md section 3.2): the system's processors without the physical cores
// that hold any of the original ones (cores: each core's mask of logical processors); without only the original
// ones when that leaves none; 0 when nothing is left. Exposed for the offline test.
std::uintptr_t ChooseOurs(std::uintptr_t original, std::uintptr_t system, const std::uintptr_t *cores, int count);

// For the offline test: "off", "binding", "on", "external" or "reverted".
const char *StateName();

} // namespace core::platform::affinity

#endif
