// R1: custom logical resolution (docs/resolution_ui/display_scale.md §6, §8); R2b: the city screen's layout
// (docs/archive/resolution_ui_r2b.md).
#pragma once

#include "core/config.h"
#include "games/emperor/site.h"

namespace emperor::resolution {

// Checks the sites and installs the hooks and code patches (emulated DirectDraw only: the real one cannot set
// arbitrary modes). Returns false (nothing installed) if anything does not match the known build.
bool Attach(const core_config *cfg);

// The display.resolution or display.scale setting changed (game thread): the next OnLoop switches to it. False
// when it cannot be applied (resolution switching was switched off after an error).
bool SettingsChanged(const core_config *cfg);

// From the main-loop hook, once per iteration on the game thread: switches between the 1024x768 frame of the
// front-end screens and the configured resolution when the screen changes.
void OnLoop();

// True while a front-end screen (a full-screen picture: menus, loading, victory and defeat) runs in its 1024x768
// frame, i.e. from a full-screen picture until the next city screen (R2a frames these, games/emperor/fullscreen.h).
bool FrontEnd();

// The leather below (and right of) the sidebar, as after the city screen's sidebar (R2b), for another screen's
// sidebar that the game draws only down to 768: the China map's (R2c). Game thread, after that sidebar was drawn.
void DrawSidebarFiller();

// Where the building information window opens ([ui] info_window, R2b): the game's own rule (moved with the centre
// of the city area), centred in the city area, or beside the cursor. Takes effect from the next window opened.
enum class InfoMode { Original, Centered, Cursor };
void SetInfoMode(InfoMode mode);

// The city area the window must stay in (right and bottom exclusive).
struct InfoArea {
    int left, top, right, bottom;
};
// The top left corner of a w x h window for Centered and Cursor (Original: the area's top left). Never past the
// area's right or bottom; when the window is larger than the area, at its left or top.
POINT InfoPosition(InfoMode mode, const InfoArea &area, int w, int h, int mouseX, int mouseY);

// The row of the sidebar backdrop (China_Interface entry 2045) drawn at screen row y in a column that starts at
// screen row top: 0 right of the sidebar (the top bar's band, then the leather with the edge), 761 or 768 below it
// (leather with the edge only; no band). See resolution.cpp.
int SidebarBackdropRow(int y, int top);

// For the offline tests (proxy/ddraw/test_restore.cpp): the detours by site; the state Attach would set (without
// hooks or patches); Shift+F7; China_Interface.sg3 from another place than DATA next to the exe.
const Detour *Detours(int *count);
void InstalledForTest(const core_config *cfg);
bool KeyFilterForTest(UINT message, WPARAM key, LPARAM l);
void SetInterfacePackForTest(const wchar_t *path);

} // namespace emperor::resolution
