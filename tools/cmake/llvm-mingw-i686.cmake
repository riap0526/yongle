# CMake toolchain: 32-bit Windows (i686) with the portable llvm-mingw toolchain.
# Override the toolchain location with -DLLVM_MINGW_ROOT=<dir> if it moves.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

if(NOT LLVM_MINGW_ROOT)
    set(LLVM_MINGW_ROOT "C:/tools/llvm-mingw-20261006-ucrt-x86_64")
endif()
set(_bin "${LLVM_MINGW_ROOT}/bin")

set(CMAKE_C_COMPILER   "${_bin}/i686-w64-mingw32-clang.exe")
set(CMAKE_CXX_COMPILER "${_bin}/i686-w64-mingw32-clang++.exe")
set(CMAKE_RC_COMPILER  "${_bin}/i686-w64-mingw32-windres.exe")
set(CMAKE_AR           "${_bin}/llvm-ar.exe")
set(CMAKE_RANLIB       "${_bin}/llvm-ranlib.exe")
set(CMAKE_MAKE_PROGRAM "${_bin}/mingw32-make.exe" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/i686-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
