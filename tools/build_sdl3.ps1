# Builds SDL3 3.4.18 as a 32-bit static library with llvm-mingw and installs it into build\sdl3-i686-install,
# where CMakeLists.txt looks for it (SDL3_DIR). Only needed once, or after deleting build\.
# The source is the verified release tarball (docs/third_party.md: SHA256 and GPG signature checked), extracted to
# C:\tools\src\SDL3-3.4.18.
param(
    [string]$Source = 'C:\tools\src\SDL3-3.4.18',
    [string]$Toolchain = 'C:\tools\llvm-mingw-20261006-ucrt-x86_64',
    [string]$CMake = 'C:\tools\cmake-4.4.4-windows-x86_64\bin\cmake.exe'
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $root 'build\sdl3-i686'
$install = Join-Path $root 'build\sdl3-i686-install'
$env:PATH = (Join-Path $Toolchain 'bin') + ';' + $env:PATH

& $CMake -S $Source -B $build -G 'MinGW Makefiles' `
    "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $root 'tools\cmake\llvm-mingw-i686.cmake')" `
    "-DLLVM_MINGW_ROOT=$($Toolchain -replace '\\','/')" `
    -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF `
    "-DCMAKE_INSTALL_PREFIX=$install"
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
& $CMake --build $build -j 8
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
& $CMake --install $build
if ($LASTEXITCODE -ne 0) { throw "install failed ($LASTEXITCODE)" }
