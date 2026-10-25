# Builds the 32-bit proxy ddraw.dll (C entry point + C++20 core) with CMake and llvm-mingw.
# Output: build\engine\ddraw.dll and build\engine\test_load.exe. Copy ddraw.dll into testbed\ manually;
# never into original\.
param(
    [string]$Toolchain = 'C:\tools\llvm-mingw-20261006-ucrt-x86_64',
    [string]$CMake = 'C:\tools\cmake-4.4.4-windows-x86_64\bin\cmake.exe'
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$out = Join-Path $root 'build\engine'
$env:PATH = (Join-Path $Toolchain 'bin') + ';' + $env:PATH

& $CMake -S $root -B $out -G 'MinGW Makefiles' `
    "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $root 'tools\cmake\llvm-mingw-i686.cmake')" `
    "-DLLVM_MINGW_ROOT=$($Toolchain -replace '\\','/')"
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
& $CMake --build $out -j 8
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

# Show machine type, exports and imports so the export table can be checked.
& (Join-Path $Toolchain 'bin\llvm-objdump.exe') -p (Join-Path $out 'ddraw.dll') |
    Select-String -Pattern 'file format|Export Table|^\s+\d+\s+0x|DLL Name'
