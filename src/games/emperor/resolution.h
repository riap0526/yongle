// Custom logical resolution and the city screen's layout
// (resolution.cpp lists the parts).
#pragma once

#include "core/ui/info_window.h"

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
// frame, i.e. from a full-screen picture until the next city screen (framed by games/emperor/fullscreen.h).
bool FrontEnd();

// The city's scale on the screen (0.2.0 item 8): screen (or window client) pixels per game pixel at the city's
// resolution setting, the smaller of width and height (Auto, a fixed size and the window alike). The front-end
// screens are shown at their own size times it. Game thread.
double CityScale();

// The leather below (and right of) the sidebar, as after the city screen's sidebar, for another screen's
// sidebar that the game draws only down to 768: the China map's. Game thread, after that sidebar was drawn.
void DrawSidebarFiller();

// Where the building information window opens ([ui] info_window): the game's own rule (moved with the centre
// of the city area), centred in the city area, or beside the cursor. Takes effect from the next window opened.
// The mode, the city area and the position are the core's (core/ui/info_window, shared with Zeus).
using InfoMode = core::ui::info_window::Mode;
using InfoArea = core::ui::info_window::Area;
inline void SetInfoMode(InfoMode mode)
{
    core::ui::info_window::Set(mode);
}
inline POINT InfoPosition(InfoMode mode, const InfoArea &area, int w, int h, int mouseX, int mouseY)
{
    return core::ui::info_window::Position(mode, area, w, h, mouseX, mouseY);
}

// The row of the sidebar backdrop (China_Interface entry 2045) drawn at screen row y in a column that starts at
// screen row top: 0 right of the sidebar (the top bar's band, then the leather with the edge), 761 or 768 below it
// (leather with the edge only; no band). See resolution.cpp.
int SidebarBackdropRow(int y, int top);

// The key filter's part (emperor.cpp): F6 blocked and Shift+F7 (core/display/resolution_switch) while resolution is
// installed. True when the key is ours (the game does not see it).
bool KeyFilter(UINT message, WPARAM key, LPARAM l);

// screen_select_initial_mode (it reads Emperor.inf) is hooked here; then after calls the given function after the
// original returns (options_original: a new player's speeds). Start-up.
bool InitialModeHooked();
void SetAfterInitialMode(void (*after)());

// For the offline tests (src/games/emperor/test_restore.cpp): the detours by site; the state Attach would set (without
// hooks or patches); China_Interface.sg3 from another place than DATA next to the exe; whether a site is one that
// must match for our resolutions to be installed.
const Detour *Detours(int *count);
void InstalledForTest(const core_config *cfg);
bool RequiresSiteForTest(const char *name);
void SetInterfacePackForTest(const wchar_t *path);

} // namespace emperor::resolution
