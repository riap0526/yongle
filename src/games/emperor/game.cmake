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
    src/games/emperor/china_map.cpp
    src/games/emperor/display_options.cpp
    src/games/emperor/options_window.cpp
    src/games/emperor/options_text.cpp
    src/games/emperor/options_original.cpp
    src/games/emperor/options_entry.cpp
    src/games/emperor/sound.cpp
    src/games/emperor/master_volume.cpp
    src/games/emperor/intro.cpp
    src/games/emperor/zoom.cpp
    src/games/emperor/city_picture.cpp
    src/games/emperor/notice.cpp
    src/games/emperor/keys.cpp
    src/games/emperor/game_menu.cpp
    src/games/emperor/nature_sounds.cpp
    src/games/emperor/ambient_rate.cpp
)

# The offline tests of this game layer (run by src/games/emperor/tests.ps1 through proxy/ddraw/run_tests.ps1).
# They are defined by game_add_tests() below, called from the root after the shared libraries exist.
macro(game_add_tests)
    # games/emperor/ambient_rate (no game, no DLL).
    add_executable(test_ambient src/games/emperor/test_ambient.cpp src/games/emperor/ambient_rate.cpp)
    target_include_directories(test_ambient PRIVATE src)
    target_link_options(test_ambient PRIVATE -static)

    # The failure fallbacks of scrolling, resolution and panel pause: the real detours of games/emperor on a copy of the game's address
    # range with fake game functions. The parts of the DLL they call that are not under test (output, panel,
    # virtual display) are stubbed in the test.
    add_executable(test_restore src/games/emperor/test_restore.cpp
        src/games/emperor/scroll.cpp src/games/emperor/resolution.cpp src/games/emperor/emperor.cpp
        src/games/emperor/ui_fixes.cpp src/games/emperor/china_map.cpp src/games/emperor/display_options.cpp
        src/games/emperor/options_window.cpp src/games/emperor/options_text.cpp
        src/games/emperor/options_original.cpp src/games/emperor/options_entry.cpp src/core/options/window.cpp
        src/core/options/menu.cpp src/games/emperor/game_menu.cpp src/games/emperor/nature_sounds.cpp
        src/core/display/common_sizes.cpp
        src/games/emperor/sound.cpp src/games/emperor/master_volume.cpp src/games/emperor/ambient_rate.cpp
        src/games/emperor/intro.cpp
        src/games/emperor/zoom.cpp src/core/display/city_zoom.cpp
        src/games/emperor/city_picture.cpp src/games/emperor/notice.cpp src/games/emperor/keys.cpp
        src/core/display/city_strips.cpp
        src/core/capture/big_picture.cpp
        src/core/capture/png_stream.cpp
        src/core/scroll/smooth_scroll.cpp src/core/diag/guard.cpp src/core/diag/report.cpp src/core/diag/perf.cpp
        src/core/diag/environment.cpp src/core/diag/stall.cpp src/core/platform/affinity.cpp src/core/log.cpp
        src/core/diag/present_mode.cpp src/core/diag/present_mode_tracker.cpp src/core/diag/screen_programs.cpp
        src/core/platform/parallel.cpp src/core/display/logical_size.cpp
        src/core/display/moved_rect.cpp src/core/display/scaled_rect.cpp src/core/ddraw/surface_memory.cpp
        src/core/assets/sg3.cpp src/core/config.cpp src/core/settings/settings.cpp src/core/settings/ini_text.cpp
        src/core/ui/hotkey.cpp src/core/hook/hook.cpp src/core/platform/files.cpp src/core/game.cpp)
    target_include_directories(test_restore PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_options(test_restore PRIVATE -fno-exceptions)
    target_link_options(test_restore PRIVATE -static)
    target_link_libraries(test_restore PRIVATE minhook augustus_core imgui png_encode SDL3::SDL3-static winmm)
endmacro()
