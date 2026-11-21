// Held scroll arrows at the original pace: see scroll_repeat.h.
#include "core/ui/scroll_repeat.h"

#include <cstring>

namespace core::ui::scroll_repeat {
namespace {

constexpr std::uint16_t kScrollArrow = 6, kPlainArrow = 4;

std::int16_t Word(const unsigned char *b, int offset)
{
    std::int16_t v;
    std::memcpy(&v, b + offset, sizeof v);
    return v;
}

} // namespace

void Prepare(const Layout &layout, void *table, int count, int originX, int originY, const Input &in, Gate &gate,
             unsigned long now, Changed &out)
{
    if (table == nullptr || count <= 0) {
        return;
    }
    unsigned char *const first = static_cast<unsigned char *>(table);
    if (in.pressed) {
        gate.Pressed(now); // the press itself does not act (the game acts at the release, or while held)
        return;
    }
    if (!in.held) {
        return;
    }
    // Only a table with an enabled scroll arrow under the mouse asks the timer: another table's call in the same pass
    // (a screen with two scroll bars) must not use up the held arrow's turn.
    bool under = false;
    for (int i = 0; i < count && !under; ++i) {
        const unsigned char *b = first + static_cast<long>(i) * layout.stride;
        const unsigned char state = b[layout.state];
        if (static_cast<std::uint16_t>(Word(b, layout.type)) != kScrollArrow || state == 0 || state == 2) {
            continue;
        }
        const int x = originX + Word(b, layout.x), y = originY + Word(b, layout.y);
        under = x <= in.mouseX && in.mouseX < x + Word(b, layout.w) && y <= in.mouseY && in.mouseY < y + Word(b, layout.h);
    }
    if (!under || gate.Due(now)) {
        return;
    }
    for (int i = 0; i < count && out.count < kMaxHeld; ++i) {
        unsigned char *b = first + static_cast<long>(i) * layout.stride;
        if (static_cast<std::uint16_t>(Word(b, layout.type)) == kScrollArrow) {
            std::uint16_t *type = reinterpret_cast<std::uint16_t *>(b + layout.type);
            out.types[out.count] = type; // recorded first: Restore puts it back even if the write below faults
            ++out.count;
            *type = kPlainArrow;
        }
    }
}

void Restore(Changed &changed)
{
    for (int i = 0; i < changed.count; ++i) {
        *changed.types[i] = kScrollArrow;
    }
    changed.count = 0;
}

} // namespace core::ui::scroll_repeat
