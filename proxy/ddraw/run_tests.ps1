# Runs the offline tests in build\<game> (none of them is the game; build.ps1 -Game <game> first). Each test needs a
# different display back end, so the game's settings file next to ddraw.dll is rewritten for it and restored to the
# default at the end. The tests below are the core's; the game's own tests are in src\games\<game>\tests.ps1 (run
# after these, with the same Run function).
#   -Game     the game (a folder in src\games\ with a game.cmake); default emperor
#   test_entry  start-up path (static import, DPI before main)  -- any back end
#   test_wrap   logging wrapper around the real ddraw           -- backend = system
#   test_emu    our emulated DirectDraw and virtual display     -- backend = emulated, once with present = original
#               (frames shown on the calling thread) and once with present = display (output thread); covers the
#               screen ~1 s each; the panel must open only with present = display (hotkey Insert, and once
#               with ui.panel_key = Ctrl+F12); F11 screenshots and the F12 problem report
#   test_hotkey core/ui/hotkey: the hotkey syntax (no DLL)
#   test_options core/options: the options window with a fake painter (no DLL)
#   test_ini    core/settings/ini_text: changing values in the ini, keeping everything else (no DLL)
#   test_settings the settings model: load, requests, callbacks, saving (no DLL; rewrites the ini next to it,
#               which is put back to the default at the end)
#   test_scroll core/scroll: cruise speed, easing, stop (no DLL; ~4 s)
#   test_camera core/scroll/camera: smooth scrolling's camera on a fake game (carries, edges, speed, keys, a fault)
#               (no DLL; writes test_camera.log; ~2 s)
#   test_drag   core/scroll/drag: middle-button drag scrolling on a fake game and system (click, drag, scales,
#               whole tiles, buttons, ends, the setting, the edge width, a fault) (no DLL; writes test_drag.log)
#   test_picture core/scroll/picture: smooth scrolling's picture on a fake game (the buffer, the extent, a clip past
#               the bottom, conversions, the drag, a fault) (no DLL; writes test_picture.log)
#   test_zoom   core/scroll/zoom: the city's zoom on a fake game (the wheel, the middle button, the map's limit,
#               the zoomed drawing, a scaling that fails) (no DLL; writes test_zoom.log)
#   test_logical core/display/logical_size: display scale rules (no DLL)
#   test_moved_rect core/display/moved_rect: a rectangle shown elsewhere, every buffer between no-access pages
#   test_city_layout core/display/city_layout: spare rows, the top bar's tiling, the sidebar filler's inset panel,
#               mirrored tiles and moved rows (no DLL)
#   test_scaled_rect core/display/scaled_rect: the China map's scaling, mouse mapping and backdrop
#               (no DLL)
#   test_common_sizes core/display/common_sizes: the panel's common sizes for a screen (no DLL)
#   test_city_zoom core/display/city_zoom and ShrinkRgb565: the zoom limit per map and screen, shrinking
#               (no DLL)
#   test_city_picture the city picture's strips and its PNG written a row at a time, read back (no DLL; writes and
#               deletes test_city_picture_*.png)
#   test_city_picture_core core/capture/city_picture: the city and minimap pictures on a fake game (the refusals, the
#               strips, the put-back order, a fault, the minimap's two key colors, file names) (no DLL; writes
#               test_city_picture_core.log)
#   test_framing core/display/framing: the framed full-screen picture geometry (no DLL)
#   test_surface_memory core/ddraw/surface_memory: spare rows with a canary below each emulated surface, guard
#               pages around it (no DLL)
#   test_perf   core/diag/perf: phases, samples per thread, averages, history, the rate log window (no DLL; ~1 s)
#   test_report core/diag/report: frame check counts, window event log, report numbers (no DLL)
#   test_exe_identity core/platform/exe_identity: SHA-256, the exe log line, the untested-exe notice (no DLL)
#   test_guard  core/diag/guard: the exception log and feature guards (no DLL; writes test_guard.log)
#   test_env    core/diag/environment: modules, processor time per thread, priorities, affinity and its changes,
#               where our files are (no DLL; ~4 s)
#   test_stall  core/diag/stall: long passes sampled with their call stack (no DLL)
#   test_autorun core/diag/autorun: the smoke test's script and steps on a fake game and clock (no DLL)
#   test_notice core/ui/notice: our messages' queue for the game's notice boxes on a fake game layer (no DLL)
#   test_present_mode core/diag/present_mode: mode changes, errors counted, giving up, the once-a-second line, the
#               not-Direct3D-11 path (no DLL; writes test_present_mode.log)
#   test_affinity core/platform/affinity: scenarios on one processor in child processes, the last one
#               test_emu.exe with the DLL started on one processor (writes test_affinity_*.log; ~15 s)
#   test_screenshot core/capture/screenshot: the writer's queue limit, PNG level and processor time (no DLL; writes
#               and deletes shots\t8_*.png)
#   test_log    core/log: a log over 4 MB is renamed at start-up (no DLL; writes and deletes test_log*.log)
#   test_files  core/platform/files: where our files go, fake and real probes (no DLL; makes and removes
#               test_files_tmp\)
# -Interactive: also the checks that need real input (test_emu: the screenshot keys with present=display, the frozen
# cursor). They press real keys, take the foreground and move the cursor: run it with nobody at the computer. Without
# it those checks are SKIPPED and listed at the end.
param([string]$Game = 'emperor', [switch]$Interactive)
$ErrorActionPreference = 'Stop'
if ($Interactive) { $env:YONGLE_TEST_INTERACTIVE = '1' } else { Remove-Item Env:YONGLE_TEST_INTERACTIVE -ErrorAction SilentlyContinue }
$skipped = @()
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$gameDir = Join-Path $repo "src\games\$Game"
if (-not (Test-Path (Join-Path $gameDir 'game.cmake'))) { throw "No game '$Game' (src\games\$Game\game.cmake)" }
$dir = Resolve-Path (Join-Path $repo "build\$Game")
# The game's settings file: the one .ini in its folder (GAME_INI_SOURCE in its game.cmake).
$default = @(Get-ChildItem -Path $gameDir -Filter '*.ini' -File)
if ($default.Count -ne 1) { throw "Expected one settings file (*.ini) in $gameDir, found $($default.Count)" }
$default = $default[0].FullName
$ini = Join-Path $dir (Split-Path $default -Leaf)
$log = Join-Path $dir 'ddraw_proxy.log'
$results = @()

function Run($name, $backend, [string[]]$testArgs, $present = 'display', $extra = '') {
    Set-Content -Path $ini -Value "[video]`r`nbackend = $backend`r`npresent = $present`r`n$extra" -Encoding ascii
    Push-Location $dir
    try {
        $out = & ".\$name.exe" @testArgs
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $out | Where-Object { $_ -match 'FAILED|RESULT' } | ForEach-Object { "  $_" }
    $out | Where-Object { $_ -cmatch ' SKIPPED ' } | ForEach-Object { $script:skipped += "$name ($backend/$present): $($_ -replace '\s+SKIPPED.*$', '')" }
    $out | Where-Object { $_ -match 'BltFast cost' } | ForEach-Object { "  $_" }
    $script:results += [pscustomobject]@{ Test = $name; Backend = "$backend/$present"; Exit = $code }
}

Remove-Item $log -ErrorAction SilentlyContinue
Run 'test_entry' 'emulated' @()
Run 'test_wrap' 'system' @('.\ddraw.dll')
Run 'test_emu' 'emulated' @('.\ddraw.dll', 'nopanel') 'original'
Run 'test_emu' 'emulated' @('.\ddraw.dll', 'panel') 'display'
Run 'test_emu' 'emulated' @('.\ddraw.dll', 'panel-ctrlf12') 'display' "[ui]`r`npanel_key = Ctrl+F12"
Run 'test_files' 'emulated' @()
Run 'test_hotkey' 'emulated' @()
Run 'test_options' 'emulated' @()
Run 'test_ini' 'emulated' @()
Run 'test_settings' 'emulated' @()
Run 'test_scroll' 'emulated' @()
Run 'test_camera' 'emulated' @()
Run 'test_drag' 'emulated' @()
Run 'test_picture' 'emulated' @()
Run 'test_zoom' 'emulated' @()
Run 'test_logical' 'emulated' @()
Run 'test_common_sizes' 'emulated' @()
Run 'test_city_zoom' 'emulated' @()
Run 'test_city_picture' 'emulated' @()
Run 'test_city_picture_core' 'emulated' @()
Run 'test_framing' 'emulated' @()
Run 'test_moved_rect' 'emulated' @()
Run 'test_city_layout' 'emulated' @()
Run 'test_scaled_rect' 'emulated' @()
Run 'test_surface_memory' 'emulated' @()
Run 'test_perf' 'emulated' @()
Run 'test_report' 'emulated' @()
Run 'test_exe_identity' 'emulated' @()
Run 'test_guard' 'emulated' @()
Run 'test_env' 'emulated' @()
Run 'test_stall' 'emulated' @()
Run 'test_autorun' 'emulated' @()
Run 'test_notice' 'emulated' @()
Run 'test_present_mode' 'emulated' @()
Run 'test_affinity' 'emulated' @()
Run 'test_screenshot' 'emulated' @()
Run 'test_log' 'emulated' @()
# The game's own tests ($repo, $dir and Run are available to it).
$gameTests = Join-Path $gameDir 'tests.ps1'
if (Test-Path $gameTests) { . $gameTests }
Copy-Item $default $ini -Force
Remove-Item Env:YONGLE_TEST_INTERACTIVE -ErrorAction SilentlyContinue
$results | Format-Table -AutoSize
if ($skipped.Count -gt 0) {
    "Skipped (need real input; run_tests.ps1 -Interactive with nobody at the computer):"
    $skipped | ForEach-Object { "  $_" }
}
if ($results | Where-Object { $_.Exit -ne 0 }) { exit 1 }
