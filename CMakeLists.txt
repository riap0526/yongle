# The engine DLL (32-bit ddraw.dll) for one game. Configure with the llvm-mingw toolchain:
#   cmake -S . -B build/<game> -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=tools/cmake/llvm-mingw-i686.cmake
#         [-DENGINE_GAME=<game>]
# or simply run proxy/ddraw/build.ps1 [-Game <game>]. Each game is src/games/<game>/ with a game.cmake (its public
# name, version, settings file, game layer and the offline tests of that layer); the rest is shared. Without
# ENGINE_GAME the only game in src/games/ is used.
cmake_minimum_required(VERSION 3.25)

file(GLOB ENGINE_GAME_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/src/games ${CMAKE_CURRENT_SOURCE_DIR}/src/games/*/game.cmake)
list(TRANSFORM ENGINE_GAME_FILES REPLACE "/game.cmake$" "")
if(NOT ENGINE_GAME)
    list(LENGTH ENGINE_GAME_FILES ENGINE_GAME_COUNT)
    if(NOT ENGINE_GAME_COUNT EQUAL 1)
        message(FATAL_ERROR "Choose the game with -DENGINE_GAME=<game> (games here: ${ENGINE_GAME_FILES})")
    endif()
    set(ENGINE_GAME ${ENGINE_GAME_FILES})
endif()
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/games/${ENGINE_GAME}/game.cmake)
    message(FATAL_ERROR "No game \"${ENGINE_GAME}\" (games here: ${ENGINE_GAME_FILES})")
endif()
set(ENGINE_GAME ${ENGINE_GAME} CACHE STRING "The game this build is for (src/games/<game>/)" FORCE)
include(src/games/${ENGINE_GAME}/game.cmake)

project(${GAME_PROJECT} VERSION ${GAME_VERSION} LANGUAGES C CXX RC)
set(ENGINE_VERSION_STRING "${PROJECT_VERSION}${GAME_VERSION_SUFFIX}")
message(STATUS "Building ddraw.dll for ${ENGINE_GAME}: ${GAME_PRODUCT} ${ENGINE_VERSION_STRING}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# SDL3 3.4.18, static (Zlib). Built once by tools/build/build_sdl3.ps1 into build/sdl3-i686-install.
set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/sdl3-i686-install/lib/cmake/SDL3" CACHE PATH "SDL3 CMake package")
find_package(SDL3 3.4.18 EXACT REQUIRED CONFIG COMPONENTS SDL3-static)

# MinHook 1.3.4 (third_party/minhook, BSD-2-Clause). Only the x86 sources.
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_compile_options(minhook PRIVATE -w)

# Dear ImGui v1.92.9b (third_party/imgui, MIT): the core and the SDL_Renderer3 backend only.
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui)
# No demo window; no default IME functions (they would link imm32 and act on the game's window).
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS)
target_compile_options(imgui PRIVATE -w -fno-exceptions)
target_link_libraries(imgui PRIVATE SDL3::SDL3-static)

# The core (src/core): shared by every game. ddraw.dll is the C proxy entry point (proxy/ddraw), the core and the
# chosen game's layer (GAME_SOURCES from its game.cmake), in this order.
set(CORE_SOURCES
    src/core/game.cpp
    src/core/log.cpp
    src/core/config.cpp
    src/core/startup.cpp
    src/core/hook/hook.cpp
    src/core/platform/dpi.cpp
    src/core/platform/timing.cpp
    src/core/platform/files.cpp
    src/core/platform/affinity.cpp
    src/core/platform/parallel.cpp
    src/core/platform/exe_identity.cpp
    src/core/diag/rate_log.cpp
    src/core/sim/pace.cpp
    src/core/display/virtual_display.cpp
    src/core/display/logical_size.cpp
    src/core/display/resolution_switch.cpp
    src/core/sim/panel_pause.cpp
    src/core/display/framed_screens.cpp
    src/core/display/framing.cpp
    src/core/display/map_screen.cpp
    src/core/display/city_layout.cpp
    src/core/display/window_move.cpp
    src/core/ui/info_window.cpp
    src/core/ui/notice.cpp
    src/core/ui/notice_text.cpp
    src/core/display/moved_rect.cpp
    src/core/display/scaled_rect.cpp
    src/core/display/city_zoom.cpp
    src/core/display/city_strips.cpp
    src/core/display/common_sizes.cpp
    src/core/capture/screenshot.cpp
    src/core/capture/big_picture.cpp
    src/core/capture/png_stream.cpp
    src/core/capture/city_picture.cpp
    src/core/diag/guard.cpp
    src/core/diag/report.cpp
    src/core/diag/state_line.cpp
    src/core/diag/perf.cpp
    src/core/diag/environment.cpp
    src/core/diag/stall.cpp
    src/core/diag/autorun.cpp
    src/core/diag/present_mode.cpp
    src/core/diag/present_mode_tracker.cpp
    src/core/diag/screen_programs.cpp
    src/core/diag/window_background.cpp
    src/core/assets/sg3.cpp
    src/core/assets/image_file.cpp
    src/core/output/sdl_output.cpp
    src/core/ddraw/ddraw_log.cpp
    src/core/ddraw/ddraw_wrap.cpp
    src/core/ddraw/ddraw_emu.cpp
    src/core/ddraw/surface_memory.cpp
    src/core/scroll/smooth_scroll.cpp
    src/core/scroll/camera.cpp
    src/core/scroll/drag.cpp
    src/core/scroll/drag_system.cpp
    src/core/scroll/picture.cpp
    src/core/scroll/zoom.cpp
    src/core/sound/master_volume.cpp
    src/core/sound/sound.cpp
    src/core/sound/ambient_rate.cpp
    src/core/sound/layer1.cpp
    src/core/ui/panel.cpp
    src/core/ui/hotkey.cpp
    src/core/input/keys.cpp
    src/core/options/window.cpp
    src/core/options/menu.cpp
    src/core/options/pause_menu.cpp
    src/core/options/menu_text.cpp
    src/core/options/option_text.cpp
    src/core/options/game_options.cpp
    src/core/options/display_dialog.cpp
    src/core/options/setting_rows.cpp
    src/core/options/hotkeys_page.cpp
    src/core/options/options_host.cpp
    src/core/ui/scroll_repeat.cpp
    src/core/ui/settings_view.cpp
    src/core/ui/perf_view.cpp
    src/core/settings/settings.cpp
    src/core/settings/ini_text.cpp
    src/core/settings/intro_movie.cpp
)
add_library(ddraw SHARED proxy/ddraw/ddraw.c proxy/ddraw/ddraw.def ${CORE_SOURCES} ${GAME_SOURCES})
# Augustus code (AGPL-3.0): third_party/augustus/README.md. Compiled as part of the DLL.
add_library(augustus_core STATIC third_party/augustus/src/core/speed.c)
target_include_directories(augustus_core PUBLIC third_party/augustus/src)
# PNG encoding with a chosen level: miniz from SDL 3.4.18 (third_party/miniz, public domain).
add_library(png_encode STATIC src/core/capture/png_encode.c)
target_include_directories(png_encode PRIVATE src third_party/miniz)
target_link_libraries(png_encode PRIVATE SDL3::SDL3-static)
set_target_properties(ddraw PROPERTIES PREFIX "" OUTPUT_NAME "ddraw")
target_include_directories(ddraw PRIVATE src)
target_compile_options(ddraw PRIVATE
    -Wall -Wextra
    # DWARF for our own sources (does not change the generated code): ddraw.dbg.dll keeps it, ddraw.syms is made
    # from it (tools/build/make_syms.py), and ddraw.dll is stripped.
    -g
    # No C++ exception may cross into the game.
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)
# Static C++ runtime: the DLL depends only on system DLLs (and the UCRT API set).
target_link_options(ddraw PRIVATE -static -Wl,--subsystem,windows)
target_link_libraries(ddraw PRIVATE minhook augustus_core png_encode imgui SDL3::SDL3-static dxguid user32 gdi32 winmm)
# Debug information: ddraw.dbg.dll keeps the DWARF (for llvm-symbolizer), ddraw.syms is the table the
# exception log reads (function and source line for addresses in our DLL; tools/build/make_syms.py), and ddraw.dll
# itself is stripped of the debug sections (the code is the same).
find_program(PYTHON_EXE python REQUIRED)
set(LLVM_BIN "${LLVM_MINGW_ROOT}/bin")
add_custom_command(TARGET ddraw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ddraw> ${CMAKE_CURRENT_BINARY_DIR}/ddraw.dbg.dll
    COMMAND ${PYTHON_EXE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/build/make_syms.py ${LLVM_BIN}/llvm-objdump.exe
            ${CMAKE_CURRENT_BINARY_DIR}/ddraw.dbg.dll ${CMAKE_CURRENT_BINARY_DIR}/ddraw.syms
    COMMAND ${LLVM_BIN}/llvm-strip.exe --strip-debug $<TARGET_FILE:ddraw>
    VERBATIM)
# The version: in the code (log, panel) and as the DLL's version resource.
target_compile_definitions(ddraw PRIVATE ENGINE_VERSION="${ENGINE_VERSION_STRING}")
configure_file(proxy/ddraw/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
target_sources(ddraw PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
# The game's settings file (GAME_INI_SOURCE), copied next to the DLL (copy both into the game's folder)
# and embedded in the DLL (core::settings::SetTemplate), so an older file's descriptions are brought up to date at
# start-up; editing the file re-runs this (configure_file makes it a configure input).
get_filename_component(GAME_INI_NAME ${GAME_INI_SOURCE} NAME)
configure_file(${GAME_INI_SOURCE} ${CMAKE_CURRENT_BINARY_DIR}/${GAME_INI_NAME} COPYONLY)
file(READ ${GAME_INI_SOURCE} GAME_INI_TEMPLATE)
string(FIND "${GAME_INI_TEMPLATE}" ")engine_ini\"" GAME_INI_DELIM)
if(NOT GAME_INI_DELIM EQUAL -1)
    message(FATAL_ERROR "${GAME_INI_NAME} contains the raw string delimiter )engine_ini\"")
endif()
configure_file(tools/cmake/ini_template.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/ini_template.h @ONLY)
target_include_directories(ddraw PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Smoke test that loads a ddraw.dll by path and calls DirectDrawCreate (not the game).
add_executable(test_load proxy/ddraw/test_load.c)
target_link_options(test_load PRIVATE -static)

# Offline test of the wrapper: drives a ddraw.dll the way Emperor does, in windowed (DDSCL_NORMAL) mode.
add_executable(test_wrap proxy/ddraw/test_wrap.cpp)
target_link_options(test_wrap PRIVATE -static)
target_link_libraries(test_wrap PRIVATE dxguid)

# Offline test of the start-up path: imports DirectDrawCreate statically like Emperor.exe (through the
# toolchain's libddraw.a, which names DDRAW.dll), so the ddraw.dll in its directory loads before its entry point.
add_executable(test_entry proxy/ddraw/test_entry.cpp)
target_link_options(test_entry PRIVATE -static)
target_link_libraries(test_entry PRIVATE "-l:libddraw.a")
add_dependencies(test_entry ddraw)

# Offline test of the emulated DirectDraw (not the game): drives it the way Emperor does in fullscreen mode
# (mode set, primary + 16-bit back buffer, Lock/Blt/BltFast/colorfill, the virtual display hooks).
add_executable(test_emu proxy/ddraw/test_emu.cpp)
target_link_options(test_emu PRIVATE -static)
target_link_libraries(test_emu PRIVATE dxguid)

# Offline test of core/scroll (the smooth scroller on Augustus's speed easing).
add_executable(test_scroll proxy/ddraw/test_scroll.cpp src/core/scroll/smooth_scroll.cpp)
target_include_directories(test_scroll PRIVATE src)
target_link_options(test_scroll PRIVATE -static)
target_link_libraries(test_scroll PRIVATE augustus_core winmm)

# Offline test of core/scroll/camera: smooth scrolling's camera on a fake game (shared by both games).
add_executable(test_camera proxy/ddraw/test_camera.cpp src/core/scroll/camera.cpp src/core/scroll/smooth_scroll.cpp
    src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_camera PRIVATE src)
target_link_options(test_camera PRIVATE -static)
target_link_libraries(test_camera PRIVATE augustus_core winmm)

# Offline test of core/sound/ambient_rate: the ambient sounds' N for a rate per second (written for Yongle; in the core
# since Pericles uses it too).
add_executable(test_ambient proxy/ddraw/test_ambient.cpp src/core/sound/ambient_rate.cpp)
target_include_directories(test_ambient PRIVATE src)
target_link_options(test_ambient PRIVATE -static)

# Offline test of core/sound: the master volume and the pause while switched away on fake Miles functions and a fake
# game.
add_executable(test_sound proxy/ddraw/test_sound.cpp src/core/sound/sound.cpp src/core/sound/master_volume.cpp
    src/core/sound/ambient_rate.cpp src/core/sound/layer1.cpp src/core/config.cpp src/core/log.cpp
    src/core/diag/guard.cpp src/core/platform/files.cpp src/core/hook/hook.cpp
    src/core/settings/settings.cpp src/core/settings/ini_text.cpp src/core/ui/hotkey.cpp src/core/game.cpp)
target_include_directories(test_sound PRIVATE src)
target_link_options(test_sound PRIVATE -static)
target_link_libraries(test_sound PRIVATE minhook)

# Offline test of core/input/keys: the key bindings on a fake game and a fake system (the game's own posted F6/F7, the
# options and Esc callbacks, Alt characters, the editor's actions, a fault).
add_executable(test_keys proxy/ddraw/test_keys.cpp src/core/input/keys.cpp src/core/config.cpp src/core/log.cpp
    src/core/diag/guard.cpp src/core/platform/files.cpp src/core/hook/hook.cpp src/core/settings/settings.cpp
    src/core/settings/ini_text.cpp src/core/ui/hotkey.cpp src/core/game.cpp)
target_include_directories(test_keys PRIVATE src)
target_link_options(test_keys PRIVATE -static)
target_link_libraries(test_keys PRIVATE minhook)

# Offline test of core/sound/layer1: the layer-1 ambient sounds (the file's lines, the flight, the mix, the player on
# fake Miles functions and a fake game).
add_executable(test_layer1 proxy/ddraw/test_layer1.cpp src/core/sound/layer1.cpp src/core/sound/sound.cpp
    src/core/sound/master_volume.cpp src/core/sound/ambient_rate.cpp src/core/config.cpp src/core/log.cpp
    src/core/diag/guard.cpp src/core/platform/files.cpp src/core/hook/hook.cpp src/core/settings/settings.cpp
    src/core/settings/ini_text.cpp src/core/ui/hotkey.cpp src/core/game.cpp)
target_include_directories(test_layer1 PRIVATE src)
target_link_options(test_layer1 PRIVATE -static)
target_link_libraries(test_layer1 PRIVATE minhook)

# Offline test of core/scroll/drag: middle-button drag scrolling on a fake game and a fake system
# (shared by both games).
add_executable(test_drag proxy/ddraw/test_drag.cpp src/core/scroll/drag.cpp src/core/scroll/camera.cpp
    src/core/scroll/smooth_scroll.cpp src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_drag PRIVATE src)
target_link_options(test_drag PRIVATE -static)
target_link_libraries(test_drag PRIVATE augustus_core winmm)

# Offline test of core/scroll/picture: smooth scrolling's picture on a fake game.
add_executable(test_picture proxy/ddraw/test_picture.cpp src/core/scroll/picture.cpp src/core/scroll/camera.cpp
    src/core/scroll/smooth_scroll.cpp src/core/display/city_zoom.cpp src/core/diag/perf.cpp
    src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_picture PRIVATE src)
target_link_options(test_picture PRIVATE -static)
target_link_libraries(test_picture PRIVATE augustus_core winmm)

# Offline test of core/scroll/zoom: the city's zoom on a fake game.
add_executable(test_zoom proxy/ddraw/test_zoom.cpp src/core/scroll/zoom.cpp src/core/scroll/picture.cpp
    src/core/scroll/camera.cpp src/core/scroll/smooth_scroll.cpp src/core/display/city_zoom.cpp
    src/core/display/scaled_rect.cpp src/core/platform/parallel.cpp src/core/platform/affinity.cpp
    src/core/diag/environment.cpp src/core/platform/files.cpp src/core/game.cpp
    src/core/diag/perf.cpp src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_zoom PRIVATE src)
target_link_options(test_zoom PRIVATE -static)
target_link_libraries(test_zoom PRIVATE augustus_core winmm)

# Offline test of core/capture/city_picture: the city and minimap pictures on a fake game.
add_executable(test_city_picture_core proxy/ddraw/test_city_picture_core.cpp src/core/capture/city_picture.cpp
    src/core/capture/big_picture.cpp src/core/capture/png_stream.cpp src/core/display/city_strips.cpp
    src/core/ui/notice_text.cpp src/core/ui/hotkey.cpp src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_city_picture_core PRIVATE src)
target_link_options(test_city_picture_core PRIVATE -static)
target_link_libraries(test_city_picture_core PRIVATE png_encode)

# Offline test of core/display/logical_size (the display scale rules).
add_executable(test_logical proxy/ddraw/test_logical.cpp src/core/display/logical_size.cpp)
target_include_directories(test_logical PRIVATE src)
target_link_options(test_logical PRIVATE -static)

# Offline test of core/display/common_sizes (the panel's list of common sizes).
add_executable(test_common_sizes proxy/ddraw/test_common_sizes.cpp src/core/display/common_sizes.cpp)
target_include_directories(test_common_sizes PRIVATE src)
target_link_options(test_common_sizes PRIVATE -static)

# Offline test of core/display/framing (the framed full-screen picture geometry).
add_executable(test_framing proxy/ddraw/test_framing.cpp src/core/display/framing.cpp)
target_include_directories(test_framing PRIVATE src)
target_link_options(test_framing PRIVATE -static)

# Offline test of core/display/tile_layout: the shared tile walkers give the tiles, places and order of the copies they
# replaced (Yongle's options window painter, core/display/framed_screens).
add_executable(test_tile_layout proxy/ddraw/test_tile_layout.cpp src/core/display/framing.cpp)
target_include_directories(test_tile_layout PRIVATE src)
target_link_options(test_tile_layout PRIVATE -static)

# Offline test of core/display/moved_rect (a rectangle shown elsewhere; guard pages around every buffer).
add_executable(test_moved_rect proxy/ddraw/test_moved_rect.cpp src/core/display/moved_rect.cpp)
target_include_directories(test_moved_rect PRIVATE src)
target_link_options(test_moved_rect PRIVATE -static)

# Offline test of core/display/city_layout (spare rows, tiling, the sidebar filler's pixels).
add_executable(test_city_layout proxy/ddraw/test_city_layout.cpp src/core/display/city_layout.cpp
    src/core/ddraw/surface_memory.cpp src/core/log.cpp)
target_include_directories(test_city_layout PRIVATE src)
target_link_options(test_city_layout PRIVATE -static)

# Offline test of core/display/scaled_rect (the China map's scaling, mouse mapping and backdrop).
# Offline test of core/display/city_zoom and the shrinking in core/display/scaled_rect:
# the zoom limit for map and screen sizes, the cap, averaged and nearest shrinking.
add_executable(test_city_zoom proxy/ddraw/test_city_zoom.cpp src/core/display/city_zoom.cpp src/core/diag/environment.cpp
    src/core/platform/files.cpp
    src/core/display/scaled_rect.cpp src/core/platform/parallel.cpp src/core/platform/affinity.cpp
    src/core/diag/guard.cpp src/core/log.cpp src/core/game.cpp)
target_include_directories(test_city_zoom PRIVATE src)
target_link_options(test_city_zoom PRIVATE -static)

add_executable(test_scaled_rect proxy/ddraw/test_scaled_rect.cpp src/core/display/scaled_rect.cpp)
target_include_directories(test_scaled_rect PRIVATE src)
target_link_options(test_scaled_rect PRIVATE -static)

# Offline test of core/ddraw/surface_memory (spare rows with a canary, guard pages).
add_executable(test_surface_memory proxy/ddraw/test_surface_memory.cpp src/core/ddraw/surface_memory.cpp
    src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_surface_memory PRIVATE src)
target_link_options(test_surface_memory PRIVATE -static)

# Offline test of core/diag/report (frame check, window events, report numbers).
add_executable(test_report proxy/ddraw/test_report.cpp src/core/diag/report.cpp src/core/log.cpp)
target_include_directories(test_report PRIVATE src)
target_link_options(test_report PRIVATE -static)

# core/platform/exe_identity: SHA-256, the exe's log line and the notice for an untested exe (no game, no DLL).
add_executable(test_exe_identity proxy/ddraw/test_exe_identity.cpp src/core/platform/exe_identity.cpp
    src/core/log.cpp src/core/game.cpp)
target_include_directories(test_exe_identity PRIVATE src)
target_link_options(test_exe_identity PRIVATE -static)

# Offline test of core/diag/perf (where the time goes).
add_executable(test_perf proxy/ddraw/test_perf.cpp src/core/diag/perf.cpp)
target_include_directories(test_perf PRIVATE src)
target_link_options(test_perf PRIVATE -static)

# Offline test of core/platform/affinity: scenarios in child processes put on
# one processor; the last one starts test_emu.exe (the DLL) on one processor.
add_executable(test_affinity proxy/ddraw/test_affinity.cpp src/core/platform/affinity.cpp src/core/diag/guard.cpp
    src/core/log.cpp)
target_include_directories(test_affinity PRIVATE src)
target_link_options(test_affinity PRIVATE -static)
add_dependencies(test_affinity ddraw test_emu)

# Offline test of core/diag/environment: start-up lines, processor time, resources.
add_executable(test_env proxy/ddraw/test_env.cpp src/core/diag/environment.cpp src/core/log.cpp src/core/platform/files.cpp
    src/core/platform/affinity.cpp src/core/diag/guard.cpp src/core/game.cpp)
target_include_directories(test_env PRIVATE src)
target_link_options(test_env PRIVATE -static)

# Offline test of core/diag/guard (exception log, feature guards).
add_executable(test_stall proxy/ddraw/test_stall.cpp src/core/diag/stall.cpp src/core/diag/guard.cpp src/core/log.cpp
    src/core/platform/affinity.cpp)
target_include_directories(test_stall PRIVATE src)
target_link_options(test_stall PRIVATE -static)

# Offline test of core/diag/autorun (the unattended smoke test's script and steps, on a fake game and clock).
add_executable(test_autorun proxy/ddraw/test_autorun.cpp src/core/diag/autorun.cpp src/core/diag/guard.cpp
    src/core/log.cpp src/core/platform/affinity.cpp)
target_include_directories(test_autorun PRIVATE src)
target_link_options(test_autorun PRIVATE -static)

# Offline test of core/diag/present_mode (the bookkeeping, the report; the not-Direct3D-11 path), the screen
# programs list and the window background test color.
add_executable(test_present_mode proxy/ddraw/test_present_mode.cpp src/core/diag/present_mode.cpp
    src/core/diag/present_mode_tracker.cpp src/core/diag/screen_programs.cpp src/core/diag/window_background.cpp
    src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_present_mode PRIVATE src)
target_link_options(test_present_mode PRIVATE -static)
target_link_libraries(test_present_mode PRIVATE dxguid user32 gdi32)

# Offline test of the screenshot writer: the queue limit and the processor time per picture.
add_executable(test_screenshot proxy/ddraw/test_screenshot.cpp src/core/capture/screenshot.cpp src/core/platform/files.cpp
    src/core/capture/big_picture.cpp src/core/capture/png_stream.cpp
    src/core/diag/environment.cpp src/core/log.cpp src/core/platform/affinity.cpp src/core/diag/guard.cpp src/core/game.cpp)
target_include_directories(test_screenshot PRIVATE src)
target_link_options(test_screenshot PRIVATE -static)
target_link_libraries(test_screenshot PRIVATE png_encode SDL3::SDL3-static)

# Offline test of the city picture's parts: the strips, the PNG written a row at a
# time (read back with SDL).
add_executable(test_city_picture proxy/ddraw/test_city_picture.cpp src/core/display/city_strips.cpp
    src/core/capture/big_picture.cpp src/core/capture/png_stream.cpp src/core/log.cpp)
target_include_directories(test_city_picture PRIVATE src)
target_link_options(test_city_picture PRIVATE -static)
target_link_libraries(test_city_picture PRIVATE png_encode SDL3::SDL3-static)

# Offline test of the log's size limit at start-up.
add_executable(test_log proxy/ddraw/test_log.cpp src/core/log.cpp)
target_include_directories(test_log PRIVATE src)
target_link_options(test_log PRIVATE -static)

add_executable(test_guard proxy/ddraw/test_guard.cpp src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_guard PRIVATE src)
target_link_options(test_guard PRIVATE -static)
target_compile_options(test_guard PRIVATE -g)
add_custom_command(TARGET test_guard POST_BUILD
    COMMAND ${PYTHON_EXE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/build/make_syms.py ${LLVM_BIN}/llvm-objdump.exe
            $<TARGET_FILE:test_guard> ${CMAKE_CURRENT_BINARY_DIR}/test_guard.syms
    VERBATIM)

# Offline test of core/ui/notice: the queue of our messages for the game's notice boxes on a fake game layer
# (shared by both games).
add_executable(test_notice proxy/ddraw/test_notice.cpp src/core/ui/notice.cpp src/core/ui/notice_text.cpp
    src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_notice PRIVATE src)
target_link_options(test_notice PRIVATE -static)

# Offline test of core/options/pause_menu: the pause menu on a fake game layer (input, drawing, the pending item,
# confirmations, a fault in the game layer's callback).
add_executable(test_pause_menu proxy/ddraw/test_pause_menu.cpp src/core/options/pause_menu.cpp
    src/core/options/menu.cpp src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_pause_menu PRIVATE src)
target_link_options(test_pause_menu PRIVATE -static)

# Offline test of core/options/options_host: the options window in a game on a fake game layer (opening, drawing
# where allowed, Hotkeys over the window, Apply after it, the options key, a right click, Options from the pause menu,
# an error while drawing).
add_executable(test_options_host proxy/ddraw/test_options_host.cpp src/core/options/options_host.cpp
    src/core/options/window.cpp src/core/options/menu.cpp src/core/options/pause_menu.cpp src/core/options/menu_text.cpp
    src/core/ui/hotkey.cpp src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_options_host PRIVATE src)
target_link_options(test_options_host PRIVATE -static)

# Offline test of core/options/hotkeys_page: the Hotkeys window's page from a fake action table and the debug panel's
# note on a key in use.
add_executable(test_hotkeys_page proxy/ddraw/test_hotkeys_page.cpp src/core/options/hotkeys_page.cpp
    src/core/options/setting_rows.cpp src/core/options/option_text.cpp src/core/options/menu_text.cpp
    src/core/input/keys.cpp src/core/settings/settings.cpp src/core/settings/ini_text.cpp src/core/ui/hotkey.cpp
    src/core/diag/guard.cpp src/core/log.cpp src/core/platform/files.cpp src/core/game.cpp)
target_include_directories(test_hotkeys_page PRIVATE src)
target_link_options(test_hotkeys_page PRIVATE -static)

# Offline test of core/ui/scroll_repeat: held scroll arrows at 20 lines a second on a fake of the games' buttons
# function (a click one line, two scroll bars in a pass, types put back).
add_executable(test_scroll_repeat proxy/ddraw/test_scroll_repeat.cpp src/core/ui/scroll_repeat.cpp)
target_include_directories(test_scroll_repeat PRIVATE src)
target_link_options(test_scroll_repeat PRIVATE -static)

# The chosen game's own offline tests (game_add_tests in its game.cmake).
game_add_tests()

# Offline test of core/assets/sg3 on the player's own image pack (compared with a Python reader's hashes).
add_executable(test_sg3 proxy/ddraw/test_sg3.cpp src/core/assets/sg3.cpp src/core/log.cpp)
target_include_directories(test_sg3 PRIVATE src)
target_link_options(test_sg3 PRIVATE -static)

# Offline test of core/platform/files: where our files go, with fake and real probes.
add_executable(test_files proxy/ddraw/test_files.cpp src/core/platform/files.cpp src/core/log.cpp src/core/game.cpp)
target_include_directories(test_files PRIVATE src)
target_link_options(test_files PRIVATE -static)

# Offline test of core/settings/ini_text (changing values in the settings file without touching anything else).
add_executable(test_ini proxy/ddraw/test_ini.cpp src/core/settings/ini_text.cpp)
target_include_directories(test_ini PRIVATE src)
target_link_options(test_ini PRIVATE -static)

# Offline test of the settings model (core/settings + core/config): load, requests, callbacks, saving.
add_executable(test_settings proxy/ddraw/test_settings.cpp src/core/config.cpp src/core/log.cpp src/core/diag/guard.cpp
    src/core/platform/files.cpp
    src/core/settings/settings.cpp src/core/settings/ini_text.cpp src/core/ui/hotkey.cpp src/core/game.cpp)
target_include_directories(test_settings PRIVATE src)
target_link_options(test_settings PRIVATE -static)

# Offline test of core/ui/hotkey (the panel hotkey syntax).
add_executable(test_hotkey proxy/ddraw/test_hotkey.cpp src/core/ui/hotkey.cpp)
target_include_directories(test_hotkey PRIVATE src)
target_link_options(test_hotkey PRIVATE -static)

# Offline test of core/options (the options window: drafts, widgets, layout, scrolling, tooltip) with a fake painter.
add_executable(test_options proxy/ddraw/test_options.cpp src/core/options/window.cpp src/core/options/menu.cpp
    src/core/ui/hotkey.cpp)
target_include_directories(test_options PRIVATE src)
target_link_options(test_options PRIVATE -static)

# Offline test of core/options/game_options: the game's own options as rows on a fake game (ranges, defaults, the
# message kinds, Apply writing only what changed, the volumes set again) and a new player's starting speeds.
add_executable(test_game_options proxy/ddraw/test_game_options.cpp src/core/options/game_options.cpp
    src/core/options/option_text.cpp src/core/options/menu_text.cpp src/core/options/window.cpp
    src/core/options/menu.cpp src/core/ui/hotkey.cpp src/core/ui/notice.cpp src/core/ui/notice_text.cpp
    src/core/diag/guard.cpp src/core/log.cpp src/core/platform/files.cpp src/core/game.cpp)
target_include_directories(test_game_options PRIVATE src)
target_link_options(test_game_options PRIVATE -static)

# Offline test of core/options/display_dialog: the Display options dialog's scale steps and status lines, and on
# a fake game layer what it draws, what its buttons set and what Cancel or closing it puts back.
add_executable(test_display_dialog proxy/ddraw/test_display_dialog.cpp src/core/options/display_dialog.cpp
    src/core/display/logical_size.cpp src/core/config.cpp src/core/log.cpp src/core/diag/guard.cpp
    src/core/platform/files.cpp src/core/settings/settings.cpp src/core/settings/ini_text.cpp src/core/ui/hotkey.cpp
    src/core/game.cpp)
target_include_directories(test_display_dialog PRIVATE src)
target_link_options(test_display_dialog PRIVATE -static)

# Targets of local development tools, kept out of the public source: built only where the file exists.
include(${CMAKE_CURRENT_SOURCE_DIR}/local_targets.cmake OPTIONAL)
