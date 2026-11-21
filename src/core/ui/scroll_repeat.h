// Held scroll arrows at the original pace (both games). The games' image buttons of the scroll-arrow
// kind (type 6: the up / down arrows of every list's scroll bar) act on every call of the buttons' input function while
// the left button is held on them, with no timer: one line per pass, about 20 a second on the machines of the time,
// some 390 a second now. Augustus gives its scroll arrows a timer; here a held arrow acts once every kIntervalMs,
// counted from the press (so a click scrolls one line: the line of its release), which is the pace of the time.
//
// How: before the game's function runs, while the left button is held on an enabled type-6 button and its time has not
// come, every type-6 button of that table is made type 4 for the call (the game treats 4 and 6 alike everywhere else:
// the press effect's countdown; the press and release paths do not read the type) and put back right after.
//
// Nothing here knows a game address: the game layer gives the table's layout (offsets) and the input state of the
// pass, and calls the game's function between Prepare and Restore. Data only; no game function is called.
//
// Threads: the game thread only.
#pragma once

#include <cstdint>

namespace core::ui::scroll_repeat {

constexpr unsigned long kIntervalMs = 50; // 20 a second: one line per pass of the time

// Where a button's fields are (both games: 0x28 bytes a button; x, y, w, h words at 0..6, type word at 8, the state
// byte at 0x18: 0 and 2 do not take left clicks).
struct Layout {
    Layout(int stride, int x, int y, int w, int h, int type, int state)
        : stride(stride), x(x), y(y), w(w), h(h), type(type), state(state)
    {
    }
    int stride, x, y, w, h, type, state;
};

// The input of this pass, as the game's function reads it.
struct Input {
    bool pressed; // the left button went down in this pass
    bool held;    // the left button is down
    int mouseX, mouseY;
};

// The timer: the press starts it; a held arrow may act once it is kIntervalMs old, which restarts it.
class Gate {
public:
    void Pressed(unsigned long now) { last_ = now; }
    bool Due(unsigned long now)
    {
        if (now - last_ < kIntervalMs) {
            return false;
        }
        last_ = now;
        return true;
    }

private:
    unsigned long last_ = 0;
};

constexpr int kMaxHeld = 16; // type-6 buttons put back per call (a scroll bar has two)

// What Prepare changed, for Restore.
struct Changed {
    std::uint16_t *types[kMaxHeld];
    int count = 0;
};

// Before the game's buttons function with this table (origin: the function's x, y arguments). Adds what it changes to
// out (count 0: nothing; the arrow acts as the game has it); each change is recorded before it is made, so a call
// abandoned part-way (a guard) still leaves out complete for Restore.
void Prepare(const Layout &layout, void *table, int count, int originX, int originY, const Input &in, Gate &gate,
             unsigned long now, Changed &out);

inline Changed Prepare(const Layout &layout, void *table, int count, int originX, int originY, const Input &in,
                       Gate &gate, unsigned long now)
{
    Changed out;
    Prepare(layout, table, count, originX, originY, in, gate, now, out);
    return out;
}

// After the game's function: the types put back.
void Restore(Changed &changed);

} // namespace core::ui::scroll_repeat
