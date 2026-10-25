# Runs the offline tests in build\engine (none of them is the game). Each test needs a different display back end,
# so emperor_engine.ini next to ddraw.dll is rewritten for it and restored to the default at the end.
#   test_entry  start-up path (static import, DPI before main)  -- any back end
#   test_wrap   logging wrapper around the real ddraw           -- backend = system
#   test_emu    our emulated DirectDraw and virtual display     -- backend = emulated, once with present = original
#               (frames shown on the calling thread) and once with present = display (output thread); covers the
#               screen ~1 s each; the panel must open only with present = display (hotkey Insert, and once
#               with ui.panel_key = Ctrl+F12); F11 screenshots and the F12 problem report
#   test_hotkey core/ui/hotkey: the hotkey syntax (no DLL)
#   test_ini    core/settings/ini_text: changing values in the ini, keeping everything else (no DLL)
#   test_settings the settings model: load, requests, callbacks, saving (no DLL; rewrites the ini next to it,
#               which is put back to the default at the end)
#   test_scroll core/scroll: cruise speed, easing, stop (no DLL; ~4 s)
#   test_logical core/display/logical_size: display scale rules (no DLL)
#   test_moved_rect core/display/moved_rect: a rectangle shown elsewhere, every buffer between no-access pages
#   test_scaled_rect core/display/scaled_rect: the China map's scaling, mouse mapping and backdrop (R2c)
#               (no DLL)
#   test_common_sizes core/display/common_sizes: the panel's common sizes for a screen (no DLL)
#   test_framing core/display/framing: the framed full-screen picture geometry (no DLL)
#   test_surface_memory core/ddraw/surface_memory: spare rows with a canary below each emulated surface, guard
#               pages around it (no DLL)
#   test_perf   core/diag/perf: phases, samples per thread, averages, history, the rate log window (no DLL; ~1 s)
#   test_report core/diag/report: frame check counts, window event log, report numbers (no DLL)
#   test_guard  core/diag/guard: the exception log and feature guards (no DLL; writes test_guard.log)
#   test_restore the failure fallbacks of M4, R1 and P2: real detours on a copy of the game's address range with
#               fake game functions that fault on purpose; one child process per scenario (no DLL)
#   test_sg3    core/assets/sg3 on testbed\DATA\China_Interface.sg3, if present (no DLL; nothing written)
$ErrorActionPreference = 'Stop'
$dir = Resolve-Path (Join-Path $PSScriptRoot '..\..\build\engine')
$ini = Join-Path $dir 'emperor_engine.ini'
$default = Join-Path $PSScriptRoot 'emperor_engine.ini'
$log = Join-Path $dir 'ddraw_proxy.log'
$results = @()

function Run($name, $backend, [string[]]$testArgs, $present = 'display', $extra = '') {
    Set-Content -Path $ini -Value "[display]`r`nbackend = $backend`r`npresent = $present`r`n$extra" -Encoding ascii
    Push-Location $dir
    try {
        $out = & ".\$name.exe" @testArgs
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $out | Where-Object { $_ -match 'FAILED|RESULT' } | ForEach-Object { "  $_" }
    $out | Where-Object { $_ -match 'BltFast cost' } | ForEach-Object { "  $_" }
    $script:results += [pscustomobject]@{ Test = $name; Backend = "$backend/$present"; Exit = $code }
}

Remove-Item $log -ErrorAction SilentlyContinue
Run 'test_entry' 'emulated' @()
Run 'test_wrap' 'system' @('.\ddraw.dll')
Run 'test_emu' 'emulated' @('.\ddraw.dll', 'nopanel') 'original'
Run 'test_emu' 'emulated' @('.\ddraw.dll', 'panel') 'display'
Run 'test_emu' 'emulated' @('.\ddraw.dll', 'panel-ctrlf12') 'display' "[ui]`r`npanel_key = Ctrl+F12"
Run 'test_hotkey' 'emulated' @()
Run 'test_ini' 'emulated' @()
Run 'test_settings' 'emulated' @()
Run 'test_scroll' 'emulated' @()
Run 'test_logical' 'emulated' @()
Run 'test_common_sizes' 'emulated' @()
Run 'test_framing' 'emulated' @()
Run 'test_moved_rect' 'emulated' @()
Run 'test_scaled_rect' 'emulated' @()
Run 'test_surface_memory' 'emulated' @()
Run 'test_perf' 'emulated' @()
Run 'test_report' 'emulated' @()
Run 'test_guard' 'emulated' @()
# The sidebar edge scenario of test_restore reads the player's own China_Interface pack when the testbed has it.
$pack = Join-Path $PSScriptRoot '..\..\testbed\DATA\China_Interface.sg3'
if (Test-Path $pack) { $env:TEST_RESTORE_PACK = (Resolve-Path $pack).Path }
Run 'test_restore' 'emulated' @()
Remove-Item Env:TEST_RESTORE_PACK -ErrorAction SilentlyContinue
# The player's own China_Interface pack, when the testbed has it: the dialog panel tiles must decode exactly as the
# Python reader decodes them (hash from tools/verify/sg3_hash.py, GOG Emperor 1.0.1.0).
$pack = Join-Path $PSScriptRoot '..\..\testbed\DATA\China_Interface.sg3'
if (Test-Path $pack) { Run 'test_sg3' 'emulated' @((Resolve-Path $pack).Path, '348', '144', 'f2d0332b') }
Copy-Item $default $ini -Force
$results | Format-Table -AutoSize
if ($results | Where-Object { $_.Exit -ne 0 }) { exit 1 }
