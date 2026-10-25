// Engine settings from emperor_engine.ini next to the DLL (UTF-8, optional): the typed view of the core's items in
// the settings model (core/settings/settings.h), which reads the file, checks values, applies changes made in our
// panel and writes them back. A missing file or key means the default; unknown sections, keys and values are
// logged and ignored.
//
//   [display]
//   backend = emulated     ; emulated (our DirectDraw, SDL3 output, no display mode changes) | system (real ddraw)
//   scaling = sharp_bilinear ; nearest | sharp_bilinear
//   present = display      ; original (the game's own ~20 FPS, on the game thread) | display (every refresh) | cap60
//   resolution = scaled    ; WxH: the logical resolution the game draws (scaled to the screen) | scaled: the
//                          ; screen's native resolution divided by the display scale (docs/resolution_ui/README.md)
//   scale = 100            ; display scale for resolution=scaled: auto | 100..250 (percent, steps of 5)
//
//   window = fullscreen    ; fullscreen (covering the monitor, no border) | windowed (moved and sized freely; F5)
//   window_width = auto    ; the window's client size (auto: three quarters of the monitor, at least 1024x768),
//   window_height = auto   ; kept when the window is resized
//   window_x = center      ; its position (center: centred on its monitor), kept when it is moved
//   window_y = center
//   window_lock_cursor = off ; on: windowed, the cursor stays inside the window while it is the active one
//
//   [gameplay]
//   smooth_scroll = on     ; on: pixel scrolling with the game's own average speed (M4) | off: the game's tile steps
//
//   [ui]
//   panel_key = Insert     ; opens and closes our settings panel; syntax in core/ui/hotkey.h (e.g. Ctrl+F12)
//   panel_scale = auto     ; size of the panel: auto (output height / 1080) | 100..300 (percent, steps of 10)
//
//   [debug]
//   screenshot_key = F11   ; saves the game's picture and the screen to shots\ (core/capture/screenshot.h)
//   report_key = F12       ; problem report: marks the log, saves the pictures and the statistics (core/diag/report.h)
//   frame_labels = off     ; on: framed pictures drawn in label colours for tools/verify/frame_check.py
//   loading_stage_ms = 0   ; 0..5000: each start-up loading stage stays on screen at least this long (for looking)
//   perf_overlay = off     ; on: FPS and where the time goes, in the top left corner while playing
//
//   [timing]
//   high_resolution_timer = on ; on: timeBeginPeriod(1) for the whole run (timeEndPeriod when the game ends) | off
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum core_display_backend { CORE_BACKEND_EMULATED = 0, CORE_BACKEND_SYSTEM = 1 };
enum core_scaling { CORE_SCALING_NEAREST = 0, CORE_SCALING_SHARP_BILINEAR = 1 };
enum core_present_mode { CORE_PRESENT_ORIGINAL = 0, CORE_PRESENT_DISPLAY = 1, CORE_PRESENT_CAP60 = 2 };
enum core_resolution_kind { CORE_RESOLUTION_FIXED = 0, CORE_RESOLUTION_SCALED = 1 };

struct core_config {
    enum core_display_backend backend;
    enum core_scaling scaling;
    enum core_present_mode present;
    enum core_resolution_kind resolution; /* FIXED: resolution_w x resolution_h; SCALED: native / scale */
    int resolution_w, resolution_h;
    int scale_percent;                    /* 0: auto; otherwise 100..250 in steps of 5 */
    int high_resolution_timer; /* nonzero: on */
    int smooth_scroll;         /* nonzero: on */
    unsigned panel_key_vk;     /* our panel's hotkey: virtual-key code ... */
    unsigned panel_key_mods;   /* ... and modifiers (core::ui::kMod* bits) */
    int panel_scale_percent;   /* 0: auto; otherwise 100..300 */
    unsigned screenshot_key_vk;   /* screenshot hotkey: virtual-key code ... */
    unsigned screenshot_key_mods; /* ... and modifiers */
    unsigned report_key_vk;       /* problem report hotkey (R2b.1): virtual-key code ... */
    unsigned report_key_mods;     /* ... and modifiers */
    int frame_labels;          /* nonzero: framed pictures in label colours (layout check) */
    int loading_stage_ms;      /* each loading stage shown at least this long (0: as fast as the game loads) */
    int perf_overlay;          /* nonzero: the performance overlay while playing (R2b.1-3, core/ui/perf_view.h) */
    int renderer;              /* index into core_renderer_names: SDL's renderer for the output (diagnosis) */
    int windowed;              /* R2e: nonzero: a window that can be moved and sized; 0: covering the monitor */
    int window_w, window_h;    /* the window's client size in pixels; 0: automatic */
    int window_x, window_y;    /* its top left corner (outer); CORE_WINDOW_CENTER: centred on its monitor */
    int window_lock_cursor;    /* nonzero: windowed, the cursor is kept inside the window while it is active */
};

#define CORE_WINDOW_CENTER (-2147483647 - 1)

/* SDL renderer names for core_config.renderer (index 0, direct3d11, is the default). */
extern const char *const core_renderer_names[6];

/* Registers the core's settings, loads the file next to module (an HMODULE) and logs the result. Kernel32 only:
   safe from DllMain. */
void core_config_load(void *module);

/* The current settings (defaults before core_config_load; changes from the panel are applied on the game thread). */
const struct core_config *core_config_get(void);

/* From DllMain, after core_config_load: the real DirectDraw behind the logging wrapper whatever the file says (an exe
   the game layer does not know; R2d). Not saved to the file. */
void core_config_force_system_backend(void);

#ifdef __cplusplus
}
#endif
