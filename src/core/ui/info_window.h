// [ui] info_window = original | centered | cursor: where the building information window opens (both games; moved
// from games/emperor/resolution.cpp when Zeus needed it). Only what has no game in it: the choices and
// their panel labels, the value in effect, and the position for centered / cursor in a city area. Each game layer
// registers the item itself (core::settings::Register(Item{"ui", "info_window", ...}) with these choices; the
// texts are the game layer's, and a check compares them) and hands the index to Watch; how the window is placed
// (the game's own rule, the windows the game always opens at one place) is the game layer's.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace core::ui::info_window {

enum class Mode { Original, Centered, Cursor };

inline constexpr int kChoiceCount = 3;
extern const char *const kChoices[kChoiceCount];      // "original", "centered", "cursor"
extern const char *const kChoiceLabels[kChoiceCount]; // shown in the panel

// From the game layer's settings registration, with the index Register returned: the value read at start-up and
// every change (it applies to the next window opened) set the mode, logged with logPrefix ("resolution: ").
void Watch(int index, const char *logPrefix);

// The mode in effect (Original until Watch or Set).
Mode Current();
void Set(Mode mode); // logged (for the offline tests and Watch)

// The city area the window must stay in (right and bottom exclusive).
struct Area {
    int left, top, right, bottom;
};

// The top left corner of a w x h window for Centered and Cursor (Original: the area's top left). Never past the
// area's right or bottom; when the window is larger than the area, at its left or top. Cursor: right of the cursor
// (24 px between), or left of it when it does not fit; centred on it vertically.
POINT Position(Mode mode, const Area &area, int w, int h, int mouseX, int mouseY);

} // namespace core::ui::info_window
