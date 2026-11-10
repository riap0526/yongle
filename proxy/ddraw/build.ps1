# Builds the 32-bit proxy ddraw.dll for one game (C entry point + C++20 core + the game's layer, src/games/<game>/)
# with CMake and llvm-mingw.
# Output: build\<game>\ddraw.dll, ddraw.syms, the game's settings file and the offline tests. Copy ddraw.dll into the
# game's folder yourself; keep an unmodified copy of the game.
#   -Game     the game (a folder in src\games\ with a game.cmake); default emperor
param(
    [string]$Game = 'emperor',
    [string]$Toolchain = 'C:\tools\llvm-mingw-20261006-ucrt-x86_64',
    [string]$CMake = 'C:\tools\cmake-4.4.4-windows-x86_64\bin\cmake.exe'
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not (Test-Path (Join-Path $root "src\games\$Game\game.cmake"))) { throw "No game '$Game' (src\games\$Game\game.cmake)" }
$out = Join-Path $root "build\$Game"
$env:PATH = (Join-Path $Toolchain 'bin') + ';' + $env:PATH

& $CMake -S $root -B $out -G 'MinGW Makefiles' `
    "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $root 'tools\cmake\llvm-mingw-i686.cmake')" `
    "-DLLVM_MINGW_ROOT=$($Toolchain -replace '\\','/')" `
    "-DENGINE_GAME=$Game"
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
& $CMake --build $out -j 8
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

# Show machine type, exports and imports so the export table can be checked.
& (Join-Path $Toolchain 'bin\llvm-objdump.exe') -p (Join-Path $out 'ddraw.dll') |
    Select-String -Pattern 'file format|Export Table|^\s+\d+\s+0x|DLL Name'
