// Offline test of core/ui/scroll_repeat (no DLL, no game): a fake of the games' buttons function (Zeus 0x4319c0,
// Emperor 0x449e70: the press records the button, the release on it acts once, a held type-6 button acts at every
// call) run at 400 passes a second, with Prepare / Restore around it as the game layers' hooks do. A click scrolls one
// line; a held arrow 20 lines a second from the press; a second scroll bar's call in the same pass does not use up the
// held arrow's turn; types are put back; disabled buttons and other types are left alone.
#include "core/ui/scroll_repeat.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sr = core::ui::scroll_repeat;

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-72s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

#pragma pack(push, 1)
struct Button { // 0x28 bytes, as both games
    std::int16_t x, y, w, h;
    std::uint16_t type;
    unsigned char pad1[0x18 - 0x0a];
    unsigned char state;
    unsigned char pad2[0x28 - 0x19];
};
#pragma pack(pop)
static_assert(sizeof(Button) == 0x28, "layout");

const sr::Layout kLayout(0x28, 0, 2, 4, 6, 8, 0x18);

struct Mouse {
    bool pressed, released, held;
    int x, y;
};

const Button *g_pressedOn = nullptr;
int g_actions[2][2]; // [table][button]

// The game's function: the first enabled button under the mouse; press: remember it; release on it: act; held on a
// type-6 button: act.
void FakeButtons(int table, Button *b, int count, int ox, int oy, const Mouse &m)
{
    for (int i = 0; i < count; ++i) {
        const Button &k = b[i];
        if (k.state == 0 || k.state == 2) continue;
        if (!(ox + k.x <= m.x && m.x < ox + k.x + k.w && oy + k.y <= m.y && m.y < oy + k.y + k.h)) continue;
        if (m.pressed) {
            g_pressedOn = &k;
            return;
        }
        if (m.released) {
            if (g_pressedOn == &k) ++g_actions[table][i];
            g_pressedOn = nullptr;
            return;
        }
        if (k.type == 6 && m.held) ++g_actions[table][i];
        return;
    }
}

Button g_bars[2][2];
sr::Gate g_gate;
bool g_typesBack = true;

void Init()
{
    std::memset(g_bars, 0, sizeof g_bars);
    for (int t = 0; t < 2; ++t) {
        for (int i = 0; i < 2; ++i) {
            Button &k = g_bars[t][i];
            k.x = 0;
            k.y = static_cast<std::int16_t>(i * 100);
            k.w = 39;
            k.h = 26;
            k.type = 6;
            k.state = 1;
        }
    }
    std::memset(g_actions, 0, sizeof g_actions);
    g_pressedOn = nullptr;
}

// One pass at time now: both scroll bars' calls (table 0 at x 0, table 1 at x 300), each wrapped like the hooks.
void Pass(unsigned long now, const Mouse &m)
{
    for (int t = 0; t < 2; ++t) {
        const int ox = t * 300, oy = 0;
        const sr::Input in{m.pressed, m.held, m.x, m.y};
        sr::Changed c = sr::Prepare(kLayout, g_bars[t], 2, ox, oy, in, g_gate, now);
        FakeButtons(t, g_bars[t], 2, ox, oy, m);
        sr::Restore(c);
        g_typesBack = g_typesBack && g_bars[t][0].type == 6 && g_bars[t][1].type == 6;
    }
}

// Press at t0 on (x, y), held for ms, released; 400 passes a second (2.5 ms; whole ms alternating 2 and 3).
void Hold(unsigned long t0, int x, int y, int ms)
{
    Pass(t0, {true, false, true, x, y});
    unsigned long t = t0;
    int k = 0;
    while (t + 3 < t0 + static_cast<unsigned long>(ms)) {
        t += (k++ % 2) ? 3 : 2;
        Pass(t, {false, false, true, x, y});
    }
    Pass(t0 + ms, {false, true, false, x, y});
}

} // namespace

int main()
{
    // A click (30 ms): one line, from the release.
    Init();
    Hold(1000, 10, 10, 30);
    Check(g_actions[0][0] == 1 && g_actions[1][0] == 0, "click (30 ms): one line");

    // A click just under the interval: still one line.
    Init();
    Hold(5000, 10, 10, 49);
    Check(g_actions[0][0] == 1, "click (49 ms): one line");

    // Held 1 s: 20 lines while held (at 50, 100, ... 1000 ms less the last) and one at the release.
    Init();
    Hold(9000, 10, 10, 1000);
    std::printf("  held 1 s on table 0's first arrow: %d lines\n", g_actions[0][0]);
    Check(g_actions[0][0] >= 19 && g_actions[0][0] <= 21, "held 1 s: about 20 lines (not 400)");

    // Held on the second scroll bar (its call comes after the first bar's in each pass): the same pace.
    Init();
    Hold(20000, 310, 110, 1000);
    std::printf("  held 1 s on table 1's second arrow: %d lines\n", g_actions[1][1]);
    Check(g_actions[1][1] >= 19 && g_actions[1][1] <= 21 && g_actions[0][0] == 0 && g_actions[0][1] == 0,
          "second scroll bar: the same pace (the first bar's call does not use its turn)");
    Check(g_typesBack, "types put back after every call");

    // The old way for comparison (no Prepare): one line per pass.
    {
        Init();
        int lines = 0;
        g_pressedOn = nullptr;
        FakeButtons(0, g_bars[0], 2, 0, 0, {true, false, true, 10, 10});
        for (int i = 0; i < 400; ++i) FakeButtons(0, g_bars[0], 2, 0, 0, {false, false, true, 10, 10});
        lines = g_actions[0][0];
        Check(lines == 400, "without the gate: 400 lines a second (the fault)");
    }

    // A disabled scroll arrow, or another type, under the mouse: nothing changed.
    {
        Init();
        g_bars[0][0].state = 2;
        const sr::Input in{false, true, 10, 10};
        sr::Gate g;
        g.Pressed(100);
        sr::Changed c = sr::Prepare(kLayout, g_bars[0], 2, 0, 0, in, g, 110);
        Check(c.count == 0 && g_bars[0][0].type == 6, "disabled arrow under the mouse: nothing changed");
        g_bars[0][0].state = 1;
        g_bars[0][0].type = 4;
        c = sr::Prepare(kLayout, g_bars[0], 2, 0, 0, in, g, 110);
        Check(c.count == 0 && g_bars[0][0].type == 4, "a plain arrow (type 4) under the mouse: nothing changed");
        g_bars[0][0].type = 6;
        c = sr::Prepare(kLayout, g_bars[0], 2, 0, 0, sr::Input{false, true, 500, 500}, g, 110);
        Check(c.count == 0, "mouse elsewhere: nothing changed");
        c = sr::Prepare(kLayout, g_bars[0], 2, 0, 0, in, g, 110);
        Check(c.count == 2 && g_bars[0][0].type == 4 && g_bars[0][1].type == 4, "held, not due: both arrows type 4");
        sr::Restore(c);
        Check(c.count == 0 && g_bars[0][0].type == 6 && g_bars[0][1].type == 6, "Restore: type 6 again");
        Check(sr::Prepare(kLayout, nullptr, 2, 0, 0, in, g, 110).count == 0 &&
                  sr::Prepare(kLayout, g_bars[0], 0, 0, 0, in, g, 110).count == 0,
              "no table / no buttons: nothing");
    }

    // The timer across the tick counter's wrap.
    {
        sr::Gate g;
        g.Pressed(0xFFFFFFF0ul);
        Check(!g.Due(0xFFFFFFFFul) && g.Due(0x30ul), "the timer across the counter's wrap");
    }

    std::printf("%s\n", g_failures == 0 ? "RESULT: PASS (0 failure(s))" : "RESULT: FAILED");
    return g_failures == 0 ? 0 : 1;
}
