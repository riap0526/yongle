#include "core/display/window_move.h"

#include "core/diag/perf.h"
#include "core/log.h"

namespace core::display {

namespace {

const int kWindowMovePhase = core::diag::perf::Register("info window move (ours)", core::diag::perf::Thread::Game);

} // namespace

bool WindowMove::Begin(BYTE *volatile *drawBuffer, int pitch, int screenW, int screenH, int x, int y, int w,
                       int h, int dx, int dy, volatile int *mouseX, volatile int *mouseY)
{
    core::diag::perf::Scope timed(kWindowMovePhase);
    Surface16 back{*drawBuffer, pitch, screenW, screenH,
                   pitch > 0 && screenH > 0 ? static_cast<size_t>(pitch) * static_cast<size_t>(screenH) : 0};
    const RECT from{x, y, x + w, y + h};
    const RECT to{from.left + dx, from.top + dy, from.right + dx, from.bottom + dy};
    // Both rectangles inside the screen (the placement keeps them in the city area; anything else: not moved).
    if (!MovedRectValid(back, from, to)) {
        static long logs = 0;
        if (++logs <= 4) {
            core_log("%sinfo window not moved: (%ld,%ld)-(%ld,%ld) to (%ld,%ld) does not fit a %dx%d screen (pitch %d)",
                     prefix_, from.left, from.top, from.right, from.bottom, to.left, to.top, screenW, screenH, pitch);
        }
        return false;
    }
    if (back.bytes > copySize_) {
        if (copy_) {
            VirtualFree(copy_, 0, MEM_RELEASE);
        }
        copy_ = static_cast<BYTE *>(VirtualAlloc(nullptr, back.bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        copySize_ = copy_ ? back.bytes : 0;
    }
    if (!copy_ || !MovedRectPrepare(copy_, back, from, to)) {
        return false;
    }
    back_ = back;
    from_ = from;
    to_ = to;
    r_.Set(drawBuffer, copy_);
    r_.Set(mouseX, *mouseX - dx); // hover in the game's own place of the window
    r_.Set(mouseY, *mouseY - dy);
    active_ = true;
    return true;
}

void WindowMove::End()
{
    core::diag::perf::Scope timed(kWindowMovePhase);
    if (!active_) {
        return;
    }
    active_ = false;
    r_.Undo(); // draw_buffer and the mouse
    // The screen it started with (the game does not change modes inside a window draw; checked again inside).
    MovedRectCompose(back_, copy_, from_, to_);
}

} // namespace core::display
