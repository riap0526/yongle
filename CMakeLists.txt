# Emperor engine DLL (32-bit). Configure with the llvm-mingw toolchain:
#   cmake -S . -B build/engine -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=tools/cmake/llvm-mingw-i686.cmake
# or simply run proxy/ddraw/build.ps1.
cmake_minimum_required(VERSION 3.25)
# Yongle: the public name (v0.1.0 onwards). The version is shown in the log, the panel and the DLL's properties.
project(emperor_engine VERSION 0.1.0 LANGUAGES C CXX RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# SDL3 3.4.18, static (Zlib; docs/third_party.md). Built once by tools/build_sdl3.ps1 into build/sdl3-i686-install.
set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/sdl3-i686-install/lib/cmake/SDL3" CACHE PATH "SDL3 CMake package")
find_package(SDL3 3.4.18 EXACT REQUIRED CONFIG COMPONENTS SDL3-static)

# MinHook 1.3.4 (third_party/minhook, BSD-2-Clause; docs/third_party.md). Only the x86 sources.
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_compile_options(minhook PRIVATE -w)

# Dear ImGui v1.92.9b (third_party/imgui, MIT; docs/third_party.md): the core and the SDL_Renderer3 backend only.
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

# ddraw.dll: C proxy entry point (proxy/ddraw) + core (src/core).
add_library(ddraw SHARED
    proxy/ddraw/ddraw.c
    proxy/ddraw/ddraw.def
    src/core/log.cpp
    src/core/config.cpp
    src/core/startup.cpp
    src/core/hook/hook.cpp
    src/core/platform/dpi.cpp
    src/core/platform/timing.cpp
    src/core/display/virtual_display.cpp
    src/core/display/logical_size.cpp
    src/core/display/framing.cpp
    src/core/display/moved_rect.cpp
    src/core/display/scaled_rect.cpp
    src/core/display/common_sizes.cpp
    src/core/capture/screenshot.cpp
    src/core/diag/guard.cpp
    src/core/diag/report.cpp
    src/core/diag/perf.cpp
    src/core/assets/sg3.cpp
    src/core/assets/image_file.cpp
    src/core/output/sdl_output.cpp
    src/core/ddraw/ddraw_log.cpp
    src/core/ddraw/ddraw_wrap.cpp
    src/core/ddraw/ddraw_emu.cpp
    src/core/ddraw/surface_memory.cpp
    src/core/scroll/smooth_scroll.cpp
    src/core/ui/panel.cpp
    src/core/ui/hotkey.cpp
    src/core/ui/settings_view.cpp
    src/core/ui/perf_view.cpp
    src/core/settings/settings.cpp
    src/core/settings/ini_text.cpp
    src/games/emperor/emperor.cpp
    src/games/emperor/scroll.cpp
    src/games/emperor/resolution.cpp
    src/games/emperor/fullscreen.cpp
    src/games/emperor/text.cpp
    src/games/emperor/ui_fixes.cpp
    src/games/emperor/china_map.cpp
    src/games/emperor/display_options.cpp
)
# Augustus code (AGPL-3.0): third_party/augustus/README.md. Compiled as part of the DLL.
add_library(augustus_core STATIC third_party/augustus/src/core/speed.c)
target_include_directories(augustus_core PUBLIC third_party/augustus/src)
set_target_properties(ddraw PROPERTIES PREFIX "" OUTPUT_NAME "ddraw")
target_include_directories(ddraw PRIVATE src)
target_compile_options(ddraw PRIVATE
    -Wall -Wextra
    # DWARF for our own sources (does not change the generated code): ddraw.dbg.dll keeps it, ddraw.syms is made
    # from it (tools/build/make_syms.py), and ddraw.dll is stripped.
    -g
    # No C++ exception may cross into the game (CLAUDE.md).
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)
# Static C++ runtime: the DLL depends only on system DLLs (and the UCRT API set).
target_link_options(ddraw PRIVATE -static -Wl,--subsystem,windows)
target_link_libraries(ddraw PRIVATE minhook augustus_core imgui SDL3::SDL3-static dxguid user32 gdi32 winmm)
# Debug information (docs/hooks.md): ddraw.dbg.dll keeps the DWARF (for llvm-symbolizer), ddraw.syms is the table the
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
target_compile_definitions(ddraw PRIVATE YONGLE_VERSION="${PROJECT_VERSION}")
configure_file(proxy/ddraw/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
target_sources(ddraw PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
# Default settings file, copied next to the DLL (copy it into testbed\ together with ddraw.dll).
configure_file(proxy/ddraw/emperor_engine.ini ${CMAKE_CURRENT_BINARY_DIR}/emperor_engine.ini COPYONLY)

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

# Offline test of core/display/logical_size (the display scale rules).
add_executable(test_logical proxy/ddraw/test_logical.cpp src/core/display/logical_size.cpp)
target_include_directories(test_logical PRIVATE src)
target_link_options(test_logical PRIVATE -static)

# Offline test of core/display/common_sizes (the panel's list of common sizes; R2b.1-9).
add_executable(test_common_sizes proxy/ddraw/test_common_sizes.cpp src/core/display/common_sizes.cpp)
target_include_directories(test_common_sizes PRIVATE src)
target_link_options(test_common_sizes PRIVATE -static)

# Offline test of core/display/framing (the framed full-screen picture geometry, fullscreen_art.md 7.5).
add_executable(test_framing proxy/ddraw/test_framing.cpp src/core/display/framing.cpp)
target_include_directories(test_framing PRIVATE src)
target_link_options(test_framing PRIVATE -static)

# Offline test of core/display/moved_rect (a rectangle shown elsewhere; guard pages around every buffer).
add_executable(test_moved_rect proxy/ddraw/test_moved_rect.cpp src/core/display/moved_rect.cpp)
target_include_directories(test_moved_rect PRIVATE src)
target_link_options(test_moved_rect PRIVATE -static)

# Offline test of core/display/scaled_rect (the China map's scaling, mouse mapping and backdrop; R2c).
add_executable(test_scaled_rect proxy/ddraw/test_scaled_rect.cpp src/core/display/scaled_rect.cpp)
target_include_directories(test_scaled_rect PRIVATE src)
target_link_options(test_scaled_rect PRIVATE -static)

# Offline test of core/ddraw/surface_memory (spare rows with a canary, guard pages; R2b.1).
add_executable(test_surface_memory proxy/ddraw/test_surface_memory.cpp src/core/ddraw/surface_memory.cpp
    src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_surface_memory PRIVATE src)
target_link_options(test_surface_memory PRIVATE -static)

# Offline test of core/diag/report (frame check, window events, report numbers; R2b.1).
add_executable(test_report proxy/ddraw/test_report.cpp src/core/diag/report.cpp src/core/log.cpp)
target_include_directories(test_report PRIVATE src)
target_link_options(test_report PRIVATE -static)

# Offline test of core/diag/perf (where the time goes; R2b.1-3).
add_executable(test_perf proxy/ddraw/test_perf.cpp src/core/diag/perf.cpp)
target_include_directories(test_perf PRIVATE src)
target_link_options(test_perf PRIVATE -static)

# Offline test of core/diag/guard (exception log, feature guards).
add_executable(test_guard proxy/ddraw/test_guard.cpp src/core/diag/guard.cpp src/core/log.cpp)
target_include_directories(test_guard PRIVATE src)
target_link_options(test_guard PRIVATE -static)
target_compile_options(test_guard PRIVATE -g)
add_custom_command(TARGET test_guard POST_BUILD
    COMMAND ${PYTHON_EXE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/build/make_syms.py ${LLVM_BIN}/llvm-objdump.exe
            $<TARGET_FILE:test_guard> ${CMAKE_CURRENT_BINARY_DIR}/test_guard.syms
    VERBATIM)

# Offline test of the failure fallbacks of M4, R1 and P2: the real detours of games/emperor on a copy of the game's
# address range with fake game functions (docs/archive/resolution_ui_r2b.md §1). The parts of the DLL they call that
# are not under test (output, panel, virtual display) are stubbed in the test.
add_executable(test_restore proxy/ddraw/test_restore.cpp
    src/games/emperor/scroll.cpp src/games/emperor/resolution.cpp src/games/emperor/emperor.cpp
    src/games/emperor/ui_fixes.cpp src/games/emperor/china_map.cpp src/games/emperor/display_options.cpp
    src/core/scroll/smooth_scroll.cpp src/core/diag/guard.cpp src/core/diag/report.cpp src/core/diag/perf.cpp
    src/core/log.cpp
    src/core/display/logical_size.cpp
    src/core/display/moved_rect.cpp src/core/display/scaled_rect.cpp src/core/ddraw/surface_memory.cpp
    src/core/assets/sg3.cpp src/core/config.cpp src/core/settings/settings.cpp src/core/settings/ini_text.cpp
    src/core/ui/hotkey.cpp src/core/hook/hook.cpp)
target_include_directories(test_restore PRIVATE src)
target_compile_options(test_restore PRIVATE -fno-exceptions)
target_link_options(test_restore PRIVATE -static)
target_link_libraries(test_restore PRIVATE minhook augustus_core imgui winmm)

# Offline test of core/assets/sg3 on the player's own image pack (compared with tools/verify/sg3_hash.py).
add_executable(test_sg3 proxy/ddraw/test_sg3.cpp src/core/assets/sg3.cpp src/core/log.cpp)
target_include_directories(test_sg3 PRIVATE src)
target_link_options(test_sg3 PRIVATE -static)

# Offline test of core/settings/ini_text (changing values in emperor_engine.ini without touching anything else).
add_executable(test_ini proxy/ddraw/test_ini.cpp src/core/settings/ini_text.cpp)
target_include_directories(test_ini PRIVATE src)
target_link_options(test_ini PRIVATE -static)

# Offline test of the settings model (core/settings + core/config): load, requests, callbacks, saving.
add_executable(test_settings proxy/ddraw/test_settings.cpp src/core/config.cpp src/core/log.cpp src/core/diag/guard.cpp
    src/core/settings/settings.cpp src/core/settings/ini_text.cpp src/core/ui/hotkey.cpp)
target_include_directories(test_settings PRIVATE src)
target_link_options(test_settings PRIVATE -static)

# Offline test of core/ui/hotkey (the panel hotkey syntax).
add_executable(test_hotkey proxy/ddraw/test_hotkey.cpp src/core/ui/hotkey.cpp)
target_include_directories(test_hotkey PRIVATE src)
target_link_options(test_hotkey PRIVATE -static)

# Timer probe: timeGetTime/Sleep granularity and an emulation of Emperor's 50 ms present gate (docs/performance.md).
# A development tool, not in the public source export.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/timer_probe/timer_probe.cpp)
    add_executable(timer_probe tools/timer_probe/timer_probe.cpp)
    target_link_options(timer_probe PRIVATE -static)
    target_link_libraries(timer_probe PRIVATE winmm)
endif()
