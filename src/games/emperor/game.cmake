# Emperor: Rise of the Middle Kingdom -- the game's part of the build (included by the root CMakeLists.txt when
# ENGINE_GAME is emperor). Everything here is about this game only: its public name and version, its settings file,
# its game layer and the offline tests of that layer.

# The public name and version: shown in the log, the panel and the DLL's properties (version.rc.in). Test builds
# carry a suffix (e.g. "-dev") so their logs are not taken for a release; empty for a release.
set(GAME_PROJECT emperor_engine)
set(GAME_VERSION 0.2.0)
set(GAME_VERSION_SUFFIX "-dev")
set(GAME_PRODUCT "Yongle")
set(GAME_DESCRIPTION "Yongle: modern display and quality-of-life layer for Emperor: Rise of the Middle Kingdom")

# The settings file this version ships (copied next to the DLL and embedded in it), and the namespace of the
# generated header that holds it (games/emperor/emperor.cpp: core::settings::SetTemplate).
set(GAME_INI_SOURCE src/games/emperor/emperor_engine.ini)
set(GAME_NAMESPACE emperor)

# The game layer, linked into ddraw.dll after the core.
set(GAME_SOURCES
    src/games/emperor/game.cpp
    src/games/emperor/emperor.cpp
    src/games/emperor/scroll.cpp
    src/games/emperor/resolution.cpp
    src/games/emperor/fullscreen.cpp
    src/games/emperor/text.cpp
    src/games/emperor/ui_fixes.cpp
    src/games/emperor/scroll_arrows.cpp
    src/games/emperor/china_map.cpp
    src/games/emperor/display_options.cpp
    src/games/emperor/options_window.cpp
    src/games/emperor/options_text.cpp
    src/games/emperor/options_original.cpp
    src/games/emperor/options_entry.cpp
    src/games/emperor/sound.cpp
    src/games/emperor/intro.cpp
    src/games/emperor/zoom.cpp
    src/games/emperor/city_picture.cpp
    src/games/emperor/notice.cpp
    src/games/emperor/keys.cpp
    src/games/emperor/game_menu.cpp
    src/games/emperor/smoke.cpp
)

# The offline tests of this game layer (run by src/games/emperor/tests.ps1 through proxy/ddraw/run_tests.ps1).
# They are defined by game_add_tests() below, called from the root after the shared libraries exist.
macro(game_add_tests)
    # The failure fallbacks of scrolling, resolution and panel pause: the real detours of games/emperor on a copy of the game's address
    # range with fake game functions. The parts of the DLL they call that are not under test (output, panel,
    # virtual display) are stubbed in the test.
    add_executable(test_restore src/games/emperor/test_restore.cpp
        src/games/emperor/scroll.cpp src/games/emperor/resolution.cpp src/games/emperor/emperor.cpp
        src/games/emperor/ui_fixes.cpp src/games/emperor/china_map.cpp src/games/emperor/display_options.cpp
        src/games/emperor/scroll_arrows.cpp src/core/ui/scroll_repeat.cpp
        src/games/emperor/options_window.cpp src/games/emperor/options_text.cpp
        src/games/emperor/options_original.cpp src/games/emperor/options_entry.cpp src/core/options/window.cpp
        src/core/options/menu.cpp src/core/options/pause_menu.cpp src/core/options/menu_text.cpp src/core/options/option_text.cpp
        src/core/options/game_options.cpp src/core/options/display_dialog.cpp
        src/core/options/setting_rows.cpp src/core/options/hotkeys_page.cpp src/core/options/options_host.cpp
        src/games/emperor/game_menu.cpp
        src/core/display/common_sizes.cpp src/core/display/framing.cpp
        src/games/emperor/sound.cpp src/core/sound/sound.cpp src/core/sound/master_volume.cpp
        src/core/sound/ambient_rate.cpp src/core/sound/layer1.cpp
        src/games/emperor/intro.cpp src/core/settings/intro_movie.cpp src/games/emperor/smoke.cpp
        src/games/emperor/zoom.cpp src/core/display/city_zoom.cpp
        src/games/emperor/city_picture.cpp src/games/emperor/notice.cpp src/games/emperor/keys.cpp src/core/input/keys.cpp
        src/core/ui/notice.cpp src/core/ui/notice_text.cpp src/core/diag/state_line.cpp
        src/core/display/city_strips.cpp
        src/core/capture/big_picture.cpp
        src/core/capture/png_stream.cpp
        src/core/capture/city_picture.cpp
        src/core/scroll/smooth_scroll.cpp src/core/scroll/camera.cpp src/core/scroll/picture.cpp
        src/core/scroll/drag.cpp src/core/scroll/drag_system.cpp
        src/core/scroll/zoom.cpp
        src/core/diag/guard.cpp src/core/diag/report.cpp
        src/core/diag/perf.cpp
        src/core/diag/rate_log.cpp src/core/sim/pace.cpp
        src/core/diag/environment.cpp src/core/diag/stall.cpp src/core/platform/affinity.cpp src/core/log.cpp
        src/core/diag/present_mode.cpp src/core/diag/present_mode_tracker.cpp src/core/diag/screen_programs.cpp
        src/core/platform/parallel.cpp src/core/display/logical_size.cpp src/core/display/resolution_switch.cpp
        src/core/sim/panel_pause.cpp
        src/core/display/moved_rect.cpp src/core/display/scaled_rect.cpp src/core/display/map_screen.cpp
        src/core/ddraw/surface_memory.cpp
        src/core/display/city_layout.cpp src/core/display/window_move.cpp src/core/ui/info_window.cpp
        src/core/assets/sg3.cpp src/core/config.cpp src/core/settings/settings.cpp src/core/settings/ini_text.cpp
        src/core/ui/hotkey.cpp src/core/hook/hook.cpp src/core/platform/files.cpp src/core/game.cpp)
    target_include_directories(test_restore PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_options(test_restore PRIVATE -fno-exceptions)
    target_link_options(test_restore PRIVATE -static)
    target_link_libraries(test_restore PRIVATE minhook augustus_core imgui png_encode SDL3::SDL3-static winmm)
endmacro()
