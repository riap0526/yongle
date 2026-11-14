#include "core/display/resolution_switch.h"

#include "core/diag/guard.h"
#include "core/display/logical_size.h"
#include "core/display/virtual_display.h"
#include "core/game.h"
#include "core/log.h"

#include <cstdio>
#include <cstring>
#include <optional>

namespace core::display::resolution {

namespace {

core::diag::Feature g_switching{"resolution switching (core/display/resolution_switch)"};

std::optional<SwitchGame> g_game;
bool g_installed = false;
void (*g_listener)() = nullptr;

int MinW()
{
    return core_game()->native_width;
}

int MinH()
{
    return core_game()->native_height;
}

// ---------------------------------------------------------------------------------------------
// The setting

struct Setting {
    core_resolution_kind kind;
    int w, h;
    int scale; // 0 = auto
};
Setting g_setting{CORE_RESOLUTION_SCALED, 1024, 768, 100};
int g_lastW = 0, g_lastH = 0;

// Front-end screens are laid out for a fixed frame of the game's own size; the output frames it.
Setting FrontEndSetting()
{
    return Setting{CORE_RESOLUTION_FIXED, MinW(), MinH(), 0};
}
bool g_frontEnd = true; // until the first city screen

// Windowed, the game is laid out for the window: its client area at rest divided by the display scale (a fixed
// resolution setting: 100%), never below the game's own size (NativeSize is the client area then).
Setting g_windowedSetting{CORE_RESOLUTION_SCALED, 0, 0, 100};

Setting Target()
{
    if (g_frontEnd) {
        return FrontEndSetting();
    }
    if (core::display::Windowed()) {
        g_windowedSetting.scale = g_setting.kind == CORE_RESOLUTION_SCALED ? g_setting.scale : 100;
        return g_windowedSetting;
    }
    return g_setting;
}

core::display::LogicalSize Logical(const Setting &s)
{
    SIZE native = core::display::NativeSize();
    return core::display::ChooseLogicalSize(native.cx, native.cy, s.kind, s.w, s.h, s.scale, MinW(), MinH());
}

void Describe(const Setting &s, char *out, int size)
{
    core::display::DescribeSetting(s.kind, s.w, s.h, s.scale, out, size);
}

const char *Prefix()
{
    return g_game ? g_game->logPrefix : "resolution";
}

int ModeW()
{
    return *g_game->modeWidth;
}

int ModeH()
{
    return *g_game->modeHeight;
}

void Changed()
{
    if (g_listener) {
        g_listener();
    }
}

// ---------------------------------------------------------------------------------------------
// Back to the game's own size

bool g_toGameSize = false; // a mode change to the game's own size is due (OnLoop)
DWORD g_lastTry = 0;

// ---------------------------------------------------------------------------------------------
// Mode size

struct ModeCall {
    core::diag::Restore r;
};

void ModeSize(ModeCall &c)
{
    const Setting target = Target();
    core::display::LogicalSize l = Logical(target);
    c.r.Set(g_game->modeWidth.ptr(), l.w);
    c.r.Set(g_game->modeHeight.ptr(), l.h);
    if (l.w != g_lastW || l.h != g_lastH) {
        char what[64];
        Describe(target, what, sizeof(what));
        if (g_frontEnd) {
            std::strcat(what, ", front-end screen");
        }
        SIZE native = core::display::NativeSize();
        char limited[80] = "";
        if (l.limited) {
            std::snprintf(limited, sizeof(limited), ", limited to the %dx%d minimum", MinW(), MinH());
        }
        if (l.evened) {
            std::strcat(limited, ", width made even");
        }
        core_log("%s: mode 3 = %dx%d (%s; native %ldx%ld, scale %d%%%s)", Prefix(), l.w, l.h, what, native.cx,
                 native.cy, l.scale_percent, limited);
        g_lastW = l.w;
        g_lastH = l.h;
    }
    c.r.Forget(); // the new size is the result
}

// ---------------------------------------------------------------------------------------------
// Shift+F7

const Setting kCycle[] = {
    {CORE_RESOLUTION_FIXED, 1024, 768, 0}, {CORE_RESOLUTION_SCALED, 0, 0, 0},   {CORE_RESOLUTION_SCALED, 0, 0, 100},
    {CORE_RESOLUTION_SCALED, 0, 0, 125},   {CORE_RESOLUTION_SCALED, 0, 0, 150}, {CORE_RESOLUTION_SCALED, 0, 0, 175},
    {CORE_RESOLUTION_SCALED, 0, 0, 200},   {CORE_RESOLUTION_SCALED, 0, 0, 250},
};
int g_cycle = -1; // -1: the setting from the settings file

// On a front-end screen only the setting changes: the frame stays at the game's own size (a mode change would give
// the same size), and the framing follows the new scale (the listener).
void CycleSetting()
{
    const int n = sizeof(kCycle) / sizeof(kCycle[0]);
    for (int tries = 0; tries < n; ++tries) { // the next setting that gives another size on this screen
        g_cycle = (g_cycle + 1) % n;
        core::display::LogicalSize next = Logical(kCycle[g_cycle]);
        if (next.w != g_lastW || next.h != g_lastH) {
            break;
        }
    }
    g_setting = kCycle[g_cycle];
    char what[48];
    Describe(g_setting, what, sizeof(what));
    core_log("%s: Shift+F7 -> %s (until the game ends; %s is not changed)%s", Prefix(), what,
             core_game()->ini_file_utf8, g_frontEnd ? " (applies on the city screen)" : "");
    if (!g_frontEnd) {
        g_game->changeMode();
    }
    Changed();
}

// ---------------------------------------------------------------------------------------------
// Switching

long g_switchLogs = 0;
constexpr long kSwitchLogs = 40;

void Switch()
{
    const unsigned char s = *g_game->gameScreen;
    if (g_frontEnd && g_game->cityScreen(s)) {
        g_frontEnd = false;
        if (++g_switchLogs <= kSwitchLogs) {
            core_log("%s: city screen 0x%02x -> the configured resolution", Prefix(), s);
        }
    }
    if (*g_game->screenMode != 3) {
        return; // the game's own mode (e.g. F6, or a video)
    }
    core::display::LogicalSize t = Logical(Target());
    if (t.w == ModeW() && t.h == ModeH()) {
        return;
    }
    DWORD now = GetTickCount();
    if (now - g_lastTry < 500) {
        return; // the game's mode change does nothing on a few screens; try again later
    }
    g_lastTry = now;
    g_game->changeMode();
}

} // namespace

void Install(const SwitchGame &game, const core_config *cfg)
{
    g_game.emplace(game);
    g_setting = Setting{cfg->resolution, cfg->resolution_w, cfg->resolution_h, cfg->scale_percent};
    core::display::SetGameMinimum(MinW(), MinH());
    g_installed = true;
}

bool Installed()
{
    return g_installed;
}

void OnModeSize()
{
    if (!g_installed || !core::diag::Enabled(g_switching)) {
        return; // the game's own size
    }
    ModeCall c;
    if (!core::diag::Guarded(g_switching, [](void *p) { ModeSize(*static_cast<ModeCall *>(p)); }, &c, &c.r)) {
        ToGameSize("the mode size could not be worked out");
    }
}

// Only our own flag (nothing that can fault), so not guarded; with switching off it is left alone.
void EnterFrontEnd(int picture)
{
    if (!g_installed) {
        return;
    }
    const unsigned char s = *g_game->gameScreen;
    if (!g_frontEnd && !g_game->cityScreen(s) && core::diag::Enabled(g_switching)) {
        g_frontEnd = true; // OnLoop switches to the frame
        if (++g_switchLogs <= kSwitchLogs) {
            char index[16] = "";
            if (picture >= 0) {
                std::snprintf(index, sizeof(index), " %d", picture);
            }
            core_log("%s: full-screen picture%s on screen 0x%02x -> %dx%d frame", Prefix(), index, s, MinW(), MinH());
        }
    }
}

void OnLoop()
{
    if (!g_installed) {
        return;
    }
    if (g_toGameSize) {
        // Back to the game's own size: mode 3 is the game's own size again (switching is off). Not guarded: from here
        // on everything is the game's own.
        if (*g_game->screenMode == 3 && (ModeW() != MinW() || ModeH() != MinH())) {
            DWORD now = GetTickCount();
            if (now - g_lastTry >= 500) {
                g_lastTry = now;
                g_game->changeMode();
            }
            return;
        }
        g_toGameSize = false;
        core_log("%s: the game runs at its own %dx%d", Prefix(), ModeW(), ModeH());
        return;
    }
    if (!core::diag::Enabled(g_switching)) {
        return;
    }
    if (!core::diag::Guarded(g_switching, [](void *) { Switch(); }, nullptr)) {
        ToGameSize("the resolution could not be changed");
    }
}

bool CycleKey(UINT message, WPARAM key, LPARAM l)
{
    if (!g_installed || message != WM_KEYDOWN || key != VK_F7 || !(GetKeyState(VK_SHIFT) & 0x8000)) {
        return false;
    }
    if ((l & (1 << 30)) || !core::diag::Enabled(g_switching)) {
        return true; // auto-repeat: one step per press; off: Shift+F7 does nothing (the game does not see it)
    }
    if (!core::diag::Guarded(g_switching, [](void *) { CycleSetting(); }, nullptr)) {
        ToGameSize("Shift+F7 could not change the resolution");
    }
    return true;
}

bool SettingsChanged(const core_config *cfg)
{
    if (!g_installed) {
        return true;
    }
    if (!core::diag::Enabled(g_switching)) {
        core_log("%s: setting not applied: resolution switching was switched off after an error (the game runs at "
                 "its own %dx%d)",
                 Prefix(), MinW(), MinH());
        return false;
    }
    g_setting = Setting{cfg->resolution, cfg->resolution_w, cfg->resolution_h, cfg->scale_percent};
    g_cycle = -1; // Shift+F7 starts again from the setting
    g_lastTry = 0;
    char what[48];
    Describe(g_setting, what, sizeof(what));
    core_log("%s: setting changed to %s%s", Prefix(), what, g_frontEnd ? " (applies on the city screen)" : "");
    if (core::display::Windowed() && g_setting.kind == CORE_RESOLUTION_FIXED) {
        // Windowed the game is laid out for the window: a size chosen here becomes the window's client area.
        core::display::SetWindowClient(g_setting.w, g_setting.h);
    }
    Changed();
    return true;
}

void ToGameSize(const char *why)
{
    core::diag::Disable(g_switching, why);
    if (g_game && !g_toGameSize && (ModeW() != MinW() || ModeH() != MinH())) {
        g_toGameSize = true;
        core_log("%s: back to the game's own %dx%d on the next loop (%s)", Prefix(), MinW(), MinH(), why);
    }
}

bool SwitchingOn()
{
    return core::diag::Enabled(g_switching);
}

bool FrontEnd()
{
    return g_installed && g_frontEnd;
}

double CityScale()
{
    Setting s = g_setting;
    if (core::display::Windowed()) {
        s = g_windowedSetting;
        s.scale = g_setting.kind == CORE_RESOLUTION_SCALED ? g_setting.scale : 100;
    }
    const core::display::LogicalSize l = Logical(s);
    const SIZE n = core::display::NativeSize(); // windowed: the client area
    if (l.w <= 0 || l.h <= 0 || n.cx <= 0 || n.cy <= 0) {
        return 1.0;
    }
    const double kx = static_cast<double>(n.cx) / l.w, ky = static_cast<double>(n.cy) / l.h;
    return kx < ky ? kx : ky;
}

void SetChangedListener(void (*listener)())
{
    g_listener = listener;
}

void InstalledForTest(const SwitchGame &game, const core_config *cfg)
{
    Install(game, cfg);
}

void ResetForTest()
{
    g_game.reset();
    g_installed = false;
    g_frontEnd = true;
    g_toGameSize = false;
    g_lastTry = 0;
    g_lastW = g_lastH = 0;
    g_cycle = -1;
    g_switchLogs = 0;
    g_listener = nullptr;
}

} // namespace core::display::resolution
