#include "games/emperor/scroll_arrows.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/ui/scroll_repeat.h"
#include "games/emperor/site.h"

#include <windows.h>
#include <mmsystem.h>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): the image buttons
// function 0x449e70 (__cdecl x, y, table, count; plain ret) acts on a type-6 button at every call while the left button
// is held on it (0x44a053..0x44a067), with no timer; its press path (0x10de06f) only remembers the button and its
// release path (0x10de070) acts once. 0x44a0b0 treats types 4 and 6 alike (the press effect's countdown).
// input_update_mouse 0x52b000: mouse_left_held 0x10de06c; mouse_left_pressed 0x10de06f = 1 when it went from 0 to 1
// (0x52b1c8). The same function as Zeus's buttons_update 0x4319c0, the same button layout.
namespace emperor::scroll_arrows {
namespace {

namespace sr = core::ui::scroll_repeat;

const Site kButtons{"image_buttons_update", 0x449e70, {0x51, 0x55, 0x8b, 0x6c, 0x24, 0x18, 0x56, 0x8b}};
using Buttons_fn = int(__cdecl *)(int x, int y, void *table, int count);
Buttons_fn o_Buttons = nullptr;

const Var<char> kLeftHeld{0x10de06c};
const Var<char> kLeftPressed{0x10de06f};
const Var<int> kMouseX{0x10de0a0}, kMouseY{0x10de09c};

// The image button: 0x28 bytes; x, y, w, h words at 0..6, the type word at 8, the state byte at 0x18 (2 takes no
// click; 0 only a right click on some buttons, 0x449e50).
const sr::Layout kLayout(0x28, 0, 2, 4, 6, 8, 0x18);

core::diag::Feature g_feature{"held scroll arrows (games/emperor/scroll_arrows)"};
sr::Gate g_gate;

struct Call {
    int x, y;
    void *table;
    int count;
    sr::Changed changed;
};

void PrepareNow(void *p)
{
    Call &c = *static_cast<Call *>(p);
    const sr::Input in{*kLeftPressed != 0, *kLeftHeld != 0, *kMouseX, *kMouseY};
    sr::Prepare(kLayout, c.table, c.count, c.x, c.y, in, g_gate, timeGetTime(), c.changed);
}

int __cdecl Buttons_detour(int x, int y, void *table, int count)
{
    Call c{x, y, table, count, {}};
    if (core::diag::Enabled(g_feature) && !core::diag::Guarded(g_feature, &PrepareNow, &c)) {
        sr::Restore(c.changed); // what it changed before the fault (types only); then the game's own way
        core_log("scroll arrows: off after an error; held scroll arrows act as the game has them");
    }
    const int r = Original(o_Buttons, kButtons)(x, y, table, count);
    sr::Restore(c.changed);
    return r;
}

} // namespace

void Attach()
{
    if (!Matches(kButtons)) {
        core_log("scroll arrows: the image buttons function at 0x%lx does not match; held scroll arrows act as the "
                 "game has them",
                 kButtons.address);
        return;
    }
    if (!Install(kButtons, &Buttons_detour, o_Buttons)) {
        core_log("scroll arrows: the image buttons function could not be hooked; held scroll arrows act as the game "
                 "has them");
        return;
    }
    core_log("scroll arrows: held scroll arrows scroll a line every %lu ms (from the press)", sr::kIntervalMs);
}

} // namespace emperor::scroll_arrows
