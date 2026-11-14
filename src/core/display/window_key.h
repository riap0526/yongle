// F5 and Alt+Enter: our window mode (core/display/virtual_display.h SetWindowed) instead of the game's own. Both
// game layers use it from their key filter (the game never sees these keys, so its own fullscreen flag and its
// settings file keep the original's values; the change goes through the video.window setting, saved and applied on
// the game thread). Nothing here knows a game address.
#pragma once

#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/settings/settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace core::display {

// True when the message is F5 (without Shift or Ctrl) or Alt+Enter: the first press switches (logged with the
// prefix, e.g. "zeus"), auto-repeats only swallow.
inline bool WindowKey(UINT message, WPARAM key, LPARAM l, const char *prefix)
{
    const bool f5 = message == WM_KEYDOWN && key == VK_F5 && !(GetKeyState(VK_SHIFT) & 0x8000) &&
                    !(GetKeyState(VK_CONTROL) & 0x8000);
    const bool altEnter = message == WM_SYSKEYDOWN && key == VK_RETURN && (l & (1 << 29));
    if (!f5 && !altEnter) {
        return false;
    }
    if (!(l & (1 << 30))) { // not an auto-repeat
        const bool to = !Windowed();
        core_log("%s: %s -> %s (the game does not see the key)", prefix, f5 ? "F5" : "Alt+Enter",
                 to ? "windowed" : "fullscreen");
        core::settings::Request(core::settings::Find("video", "window"), to ? "windowed" : "fullscreen", true);
    }
    return true;
}

} // namespace core::display
