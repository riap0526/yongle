// The real system of drag scrolling (drag.h DragSystem): Win32's cursor and capture, core::display's window, scale,
// cursor hiding and mouse filter, and the two settings.
#include "core/scroll/drag.h"

#include "core/config.h"
#include "core/display/virtual_display.h"

namespace core::scroll::drag {
namespace {

bool CursorPos(POINT *p)
{
    return GetCursorPos(p) != 0; // ours, not the exe's: the real screen position
}

void SetPos(POINT p)
{
    SetCursorPos(p.x, p.y);
}

void Capture(bool on)
{
    const HWND w = core::display::GameWindow();
    if (on) {
        if (w) {
            SetCapture(w);
        }
    }
    else if (w && GetCapture() == w) {
        ReleaseCapture();
    }
}

bool MiddleHeld()
{
    return (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
}

bool Focused()
{
    const HWND w = core::display::GameWindow();
    return w && GetForegroundWindow() == w;
}

double Scale()
{
    core::display::Placement p{};
    return core::display::ModePlacement(&p) && p.scale > 0 ? p.scale : 1.0;
}

bool Enabled()
{
    return core_config_get()->drag_scroll != 0;
}

int EdgeWidth()
{
    return core_config_get()->edge_scroll_width;
}

void Filter(bool (*f)(UINT, WPARAM, int, int))
{
    core::display::SetMouseFilter(f);
}

const DragSystem kReal{&CursorPos, &SetPos,  &core::display::HideCursor, &Capture,  &MiddleHeld,
                       &Focused,   &Scale,   &Enabled,                   &EdgeWidth, &Filter};

} // namespace

const DragSystem &RealSystem()
{
    return kReal;
}

} // namespace core::scroll::drag
