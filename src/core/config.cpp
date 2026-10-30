#include "core/config.h"

#include "core/settings/settings.h"
#include "core/ui/hotkey.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <cstring>

// The core's items in the settings model (core/settings/settings.h) and core_config, their typed view. The ini keys
// and their meanings are those of M3..P1; only the reading moved into the model (P2).
namespace {

using core::settings::Effect;
using core::settings::Item;
using core::settings::Type;

constexpr wchar_t kFileName[] = L"emperor_engine.ini";

core_config g_config = {CORE_BACKEND_EMULATED, CORE_SCALING_SHARP_BILINEAR, CORE_PRESENT_DISPLAY,
                        CORE_RESOLUTION_SCALED, 1024, 768, 100, 1, 1, 0x2D /* Insert */, 0, 0,
                        0x7A /* F11 */, 0, 0x7B /* F12 */, 0, 0, 0, 0, 0,
                        0 /* fail_direct3d9 */, 0 /* d3d9_pacing */, 0, 0, 0, CORE_WINDOW_CENTER, CORE_WINDOW_CENTER, 0};

const char *const kBackend[] = {"emulated", "system"};
const char *const kRendererLabels[] = {"auto (Direct3D 11; Direct3D 9 with the Steam overlay)", "direct3d (Direct3D 9)",
                                       "direct3d11 (Direct3D 11, flip model)"};
const char *const kScaling[] = {"nearest", "sharp_bilinear"};

// [debug] renderer (0.1.1) became [display] renderer (O1). Only direct3d, a choice the player made, is carried over:
// direct3d11 was the default of the time (and in the shipped file), and the others are no longer offered.
int MigrateRenderer(const char *old, core::settings::Migration *out, int max)
{
    if (_stricmp(old, "direct3d") != 0 || max < 1) {
        return 0;
    }
    out[0] = core::settings::Migration{"display", "renderer", "direct3d"};
    return 1;
}
const char *const kWindow[] = {"fullscreen", "windowed"};
const char *const kWindowLabels[] = {"fullscreen (covering the screen)", "windowed (move and size it freely)"};
const char *const kPresent[] = {"original", "display", "cap60"};
const char *const kPresentLabels[] = {"original (the game's own ~20 FPS)", "display (every screen refresh)",
                                      "cap60 (at most 60 per second)"};

int Index(const char *const *list, int n, const char *v)
{
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(list[i], v) == 0) {
            return i;
        }
    }
    return 0;
}

int g_backend, g_scaling, g_present, g_resolution, g_scale, g_timer, g_smooth, g_panelScale, g_panelKey,
    g_shotKey, g_reportKey, g_labels, g_loadingHold, g_perfOverlay, g_renderer, g_failD3d9, g_d3d9Pacing, g_window, g_windowW, g_windowH,
    g_windowX, g_windowY, g_windowLock;

// Keeps core_config in step with the model (registered first, so owners' callbacks see the new typed value).
bool Sync(int i, const char *v, bool)
{
    if (i == g_backend) g_config.backend = static_cast<core_display_backend>(Index(kBackend, 2, v));
    else if (i == g_scaling) g_config.scaling = static_cast<core_scaling>(Index(kScaling, 2, v));
    else if (i == g_present) g_config.present = static_cast<core_present_mode>(Index(kPresent, 3, v));
    else if (i == g_resolution) {
        if (std::strcmp(v, "scaled") == 0) {
            g_config.resolution = CORE_RESOLUTION_SCALED;
        }
        else {
            g_config.resolution = CORE_RESOLUTION_FIXED;
            g_config.resolution_w = std::atoi(v);
            g_config.resolution_h = std::atoi(std::strchr(v, 'x') + 1);
        }
    }
    else if (i == g_window) g_config.windowed = std::strcmp(v, "windowed") == 0;
    else if (i == g_windowW) g_config.window_w = std::strcmp(v, "auto") == 0 ? 0 : std::atoi(v);
    else if (i == g_windowH) g_config.window_h = std::strcmp(v, "auto") == 0 ? 0 : std::atoi(v);
    else if (i == g_windowX) g_config.window_x = std::strcmp(v, "center") == 0 ? CORE_WINDOW_CENTER : std::atoi(v);
    else if (i == g_windowLock) g_config.window_lock_cursor = std::strcmp(v, "on") == 0;
    else if (i == g_windowY) g_config.window_y = std::strcmp(v, "center") == 0 ? CORE_WINDOW_CENTER : std::atoi(v);
    else if (i == g_scale) g_config.scale_percent = std::strcmp(v, "auto") == 0 ? 0 : std::atoi(v);
    else if (i == g_timer) g_config.high_resolution_timer = std::strcmp(v, "on") == 0;
    else if (i == g_smooth) g_config.smooth_scroll = std::strcmp(v, "on") == 0;
    else if (i == g_panelScale) g_config.panel_scale_percent = std::strcmp(v, "auto") == 0 ? 0 : std::atoi(v);
    else if (i == g_panelKey) {
        core::ui::Hotkey key{0x2D, 0};
        core::ui::ParseHotkey(v, v + std::strlen(v), &key);
        g_config.panel_key_vk = key.vk;
        g_config.panel_key_mods = key.mods;
    }
    else if (i == g_labels) g_config.frame_labels = std::strcmp(v, "on") == 0;
    else if (i == g_loadingHold) g_config.loading_stage_ms = std::atoi(v);
    else if (i == g_perfOverlay) g_config.perf_overlay = std::strcmp(v, "on") == 0;
    else if (i == g_renderer) g_config.renderer = Index(core_renderer_names, 3, v);
    else if (i == g_failD3d9) g_config.fail_direct3d9 = std::strcmp(v, "on") == 0;
    else if (i == g_d3d9Pacing) g_config.d3d9_pacing = Index(core_d3d9_pacing_names, 3, v);
    else if (i == g_shotKey) {
        core::ui::Hotkey key{0x7A, 0};
        core::ui::ParseHotkey(v, v + std::strlen(v), &key);
        g_config.screenshot_key_vk = key.vk;
        g_config.screenshot_key_mods = key.mods;
    }
    else if (i == g_reportKey) {
        core::ui::Hotkey key{0x7B, 0};
        core::ui::ParseHotkey(v, v + std::strlen(v), &key);
        g_config.report_key_vk = key.vk;
        g_config.report_key_mods = key.mods;
    }
    return true;
}

int Add(const Item &item)
{
    int i = core::settings::Register(item);
    core::settings::OnChange(i, &Sync);
    return i;
}

void RegisterCoreItems()
{
    // section, key, label, help, type, default, effect, inPanel, editable, min, max, step, autoWord, choices...
    g_backend = Add(Item{"display", "backend", "DirectDraw", "emulated: our own DirectDraw (no display mode changes, "
                         "scaled with SDL3); system: the real Windows DirectDraw behind the logging wrapper.",
                         Type::Choice, "emulated", Effect::Restart, false, false, 0, 0, 0, nullptr, kBackend, nullptr,
                         2});
    g_scaling = Add(Item{"display", "scaling", "Scaling", "nearest, or sharp_bilinear (nearest pixels with a one-pixel "
                         "blend at their edges).",
                         Type::Choice, "sharp_bilinear", Effect::Restart, false, false, 0, 0, 0, nullptr, kScaling,
                         nullptr, 2});
    g_present = Add(Item{"display", "present", "Frame output",
                         "How often frames reach the screen. display: every screen refresh; cap60: at most 60 per "
                         "second; original: the game's own ~20 per second. The game's timing is the same in all "
                         "three. display and cap60 switch at once; original needs a restart, and this panel is not "
                         "available with it.",
                         Type::Choice, "display", Effect::Live, true, true, 0, 0, 0, nullptr, kPresent, kPresentLabels,
                         3});
    g_resolution = Add(Item{"display", "resolution", "Resolution",
                            "The size the game draws, scaled to the screen. Scaled (default): the screen's native "
                            "resolution divided by the display scale -- at 100 % every pixel of the game is one pixel "
                            "of the screen, the sharpest picture. 1024x768 is the game's own size. Menus and other "
                            "full-screen pictures stay at 1024x768. Windowed, a fixed size becomes the window's size.",
                            Type::Resolution, "scaled", Effect::Relayout, true, true, 0, 0, 0, nullptr, nullptr,
                            nullptr, 0});
    g_scale = Add(Item{"display", "scale", "Display scale",
                       "For Resolution = Scaled. 100 % (default): the sharpest picture, everything at its own size "
                       "(small on a large, high-resolution screen). A larger scale makes everything bigger but "
                       "softer, because the picture is enlarged. Auto: 1080 pixels high on a taller screen (2560x1440: "
                       "1920x1080, 133 %; 4K: 1920x1080), the screen's own size otherwise. Always limited so that the "
                       "result stays at least 1024x768.",
                       Type::Int, "100", Effect::Relayout, true, true, 100, 250, 5, "auto", nullptr, nullptr, 0});
    g_window = Add(Item{"display", "window", "Window",
                        "fullscreen: the game covers its monitor, without a border; windowed: a window you can move "
                        "and size (the game is laid out again for its size when you let go). F5 switches.",
                        Type::Choice, "fullscreen", Effect::Live, true, true, 0, 0, 0, nullptr, kWindow, kWindowLabels,
                        2});
    g_windowW = Add(Item{"display", "window_width", "Window width",
                         "The windowed client width in pixels (auto: three quarters of the monitor). Kept when you "
                         "size the window.",
                         Type::Int, "auto", Effect::Live, false, true, 1024, 16384, 1, "auto", nullptr, nullptr, 0});
    g_windowH = Add(Item{"display", "window_height", "Window height", "The windowed client height in pixels.",
                         Type::Int, "auto", Effect::Live, false, true, 768, 16384, 1, "auto", nullptr, nullptr, 0});
    g_windowX = Add(Item{"display", "window_x", "Window position x",
                         "The window's left edge (center: centered on its monitor). Kept when you move the window.",
                         Type::Int, "center", Effect::Live, false, true, -32000, 32000, 1, "center", nullptr, nullptr,
                         0});
    g_windowY = Add(Item{"display", "window_y", "Window position y", "The window's top edge (center: centered).",
                         Type::Int, "center", Effect::Live, false, true, -32000, 32000, 1, "center", nullptr, nullptr,
                         0});
    g_windowLock = Add(Item{"display", "window_lock_cursor", "Window: keep the cursor inside",
                            "Windowed: the cursor cannot leave the window while it is the active one (released when "
                            "you switch to another program, minimize it or go fullscreen). The window's border cannot "
                            "be reached then: switch this off, or use F5, to move or size it.",
                            Type::Bool, "off", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    g_timer = Add(Item{"timing", "high_resolution_timer", "High-resolution timer",
                       "timeBeginPeriod(1) while the game runs: the game's timers tick in 1 ms steps on every "
                       "machine (otherwise 15.6 ms unless another program raised it).",
                       Type::Bool, "on", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    g_smooth = Add(Item{"gameplay", "smooth_scroll", "Smooth scrolling",
                        "Pixel scrolling of the city at the game's own average speed, with easing. Off: the game's "
                        "whole-tile steps. Either way the simulation pauses while the view scrolls, as in the "
                        "original.",
                        Type::Bool, "on", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    g_panelScale = Add(Item{"ui", "panel_scale", "Panel size",
                            "Size of this panel in percent. Auto: the output height divided by 1080 (1440p: 133%).",
                            Type::Int, "auto", Effect::Live, true, true, 100, 300, 10, "auto", nullptr, nullptr, 0});
    g_panelKey = Add(Item{"ui", "panel_key", "Panel key",
                          "Opens and closes this panel. Change it in emperor_engine.ini for now (e.g. F12, "
                          "Ctrl+F12); changing it here comes later.",
                          Type::Hotkey, "Insert", Effect::Restart, true, false, 0, 0, 0, nullptr, nullptr, nullptr,
                          0});
    g_shotKey = Add(Item{"debug", "screenshot_key", "Screenshot key",
                         "Saves the game's own picture (its logical resolution, without this panel) as a PNG in "
                         "shots\\ in the game folder; the file name has the screen number, the resolution and the "
                         "time. Change it in emperor_engine.ini.",
                         Type::Hotkey, "F11", Effect::Restart, true, false, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    g_reportKey = Add(Item{"debug", "report_key", "Report key",
                           "When something odd is on the screen (e.g. flashing blocks): marks the log, saves the "
                           "picture shown and the screen (report<n>_... in shots\\) and the game's statistics. "
                           "Change it in emperor_engine.ini.",
                           Type::Hotkey, "F12", Effect::Restart, true, false, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    g_perfOverlay = Add(Item{"debug", "perf_overlay", "Performance overlay",
                             "FPS, a graph of the time per main-loop pass and where the time goes (the game's "
                             "simulation and drawing, our copies, the output), in the top left corner while playing.",
                             Type::Bool, "off", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    // O1 (docs/optimization.md §1.4): moved from [debug] (0.1.1). Direct3D 11 ran best in the tests, but with the
    // Steam overlay in the process it stopped for about 2.5 s every 10-30 s, where Direct3D 9 did not.
    core::settings::Retire("debug", "renderer", &MigrateRenderer);
    g_renderer = Add(Item{"display", "renderer", "Renderer",
                          "What puts the picture on the screen. auto (default): Direct3D 11, or Direct3D 9 when the "
                          "Steam overlay is in the game (Direct3D 11 stutters with it); the other one if the first "
                          "cannot start. direct3d: Direct3D 9. direct3d11: Direct3D 11. Takes effect after a restart.",
                          Type::Choice, "auto", Effect::Restart, true, true, 0, 0, 0, nullptr, core_renderer_names,
                          kRendererLabels, 3});
    // O1: for testing auto's fall-back to direct3d11 once; not in the panel.
    g_failD3d9 = Add(Item{"debug", "fail_direct3d9", "Fail Direct3D 9 (test)",
                          "on: the Direct3D 9 renderer is made to fail at start, so renderer = direct3d (or auto "
                          "with the Steam overlay) falls back to direct3d11 (for testing that path). Takes effect "
                          "after a restart.",
                          Type::Bool, "off", Effect::Restart, false, false, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    // O1g (docs/optimization.md): the experiment with Direct3D 9's pacing; not in the panel.
    g_d3d9Pacing = Add(Item{"debug", "d3d9_pacing", "Direct3D 9 pacing (test)",
                            "How the Direct3D 9 output is paced. vsync (default): each present waits for the "
                            "refresh. timer: no vsync; our own high-resolution timer spaces the frames one refresh "
                            "apart (cap60: so that at most 60 per second are shown). dwmflush: no vsync; after each "
                            "present we wait for the desktop's next composition. Direct3D 11 is not affected. Takes "
                            "effect after a restart.",
                            Type::Choice, "vsync", Effect::Restart, false, false, 0, 0, 0, nullptr,
                            core_d3d9_pacing_names, core_d3d9_pacing_names, 3});
    g_labels = Add(Item{"debug", "frame_labels", "Frame labels",
                        "For checking the framed pictures' layout (tools/verify/frame_check.py): the border's dark "
                        "lines red, the rest of the border green, padding yellow, the picture white, outside blue.",
                        Type::Bool, "off", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    g_loadingHold = Add(Item{"debug", "loading_stage_ms", "Loading stage time (ms)",
                             "Each of the five start-up loading screens stays on screen at least this long, to look at "
                             "or screenshot them (they flash by on a fast computer). 0: as fast as the game loads. "
                             "Only at start-up, so set it in emperor_engine.ini before starting the game.",
                             Type::Int, "0", Effect::Restart, true, false, 0, 5000, 100, nullptr, nullptr, nullptr, 0});
}

} // namespace

extern "C" const char *const core_renderer_names[3] = {"auto", "direct3d", "direct3d11"};
extern "C" const char *const core_d3d9_pacing_names[3] = {"vsync", "timer", "dwmflush"};

extern "C" void core_config_load(void *module)
{
    RegisterCoreItems();
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(static_cast<HMODULE>(module), path, MAX_PATH);
    wchar_t *slash = nullptr;
    for (DWORD i = 0; i < len; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') {
            slash = path + i;
        }
    }
    if (slash && static_cast<size_t>(slash - path) + sizeof(kFileName) / sizeof(wchar_t) + 1 <= MAX_PATH) {
        std::memcpy(slash + 1, kFileName, sizeof(kFileName));
    }
    else {
        std::memcpy(path, kFileName, sizeof(kFileName));
    }
    core::settings::Load(path);
}

extern "C" const struct core_config *core_config_get(void)
{
    return &g_config;
}

extern "C" void core_config_force_system_backend(void)
{
    g_config.backend = CORE_BACKEND_SYSTEM;
}
