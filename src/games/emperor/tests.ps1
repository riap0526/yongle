# Emperor's own offline tests, dot-sourced by proxy\ddraw\run_tests.ps1 -Game emperor after the core's tests ($repo,
# $dir and Run come from it; none of these is the game):
#   test_ambient games/emperor/ambient_rate: the ambient sounds' N for a rate per second, R -> original -> R (no DLL)
#   test_restore the failure fallbacks of the game layer: real detours on a copy of the game's address range with
#               fake game functions that fault on purpose; one child process per scenario (no DLL)
#   test_sg3    core/assets/sg3 on the player's own DATA\China_Interface.sg3, if present (no DLL;
#               nothing written)
Run 'test_ambient' 'emulated' @()
# The sidebar edge scenario of test_restore reads the player's own China_Interface pack when it is found.
$pack = Join-Path $repo 'game\DATA\China_Interface.sg3'
if (Test-Path $pack) { $env:TEST_RESTORE_PACK = (Resolve-Path $pack).Path }
Run 'test_restore' 'emulated' @()
Remove-Item Env:TEST_RESTORE_PACK -ErrorAction SilentlyContinue
# The same pack: the dialog panel tiles must decode exactly as the Python reader decodes them (hash from
# the Python reader, GOG Emperor 1.0.1.0).
if (Test-Path $pack) { Run 'test_sg3' 'emulated' @((Resolve-Path $pack).Path, '348', '144', 'f2d0332b') }
