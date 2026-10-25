# Building Yongle from source

Yongle is C++20 (with a small C entry point), built as a 32-bit DLL with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on Windows. Back to the [README](README.md).

## Tools

The versions used for the releases (the scripts take the paths as parameters, so other locations work):

| Tool | Version | Notes |
|---|---|---|
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) | `20261006`, ucrt, x86_64 host | the i686 compilers are used |
| [CMake](https://cmake.org/download/) | 4.4.4 | |
| Python | 3.x | makes the symbol table `ddraw.syms` |
| [SDL3](https://github.com/libsdl-org/SDL/releases) | 3.4.18 (source release) | built once as a static 32-bit library |

MinHook, Dear ImGui and the Augustus files are included in `third_party/`.

## Steps

In PowerShell, in the repository folder:

```powershell
# 1. Build SDL3 once (static, 32-bit) into build\sdl3-i686-install
powershell -File tools\build_sdl3.ps1 -Source <SDL3-3.4.18 folder> -Toolchain <llvm-mingw folder> -CMake <cmake.exe>

# 2. Build the DLL and the offline tests into build\engine
powershell -File proxy\ddraw\build.ps1 -Toolchain <llvm-mingw folder> -CMake <cmake.exe>

# 3. Run the offline tests (they do not need the game)
powershell -File proxy\ddraw\run_tests.ps1
```

Step 1 is only needed once (or after deleting `build\`).

## Output

In `build\engine\`:

- `ddraw.dll` — the DLL to copy into the game folder;
- `ddraw.syms` — its symbol table: with it next to the DLL, the exception log shows function names and source lines;
- `emperor_engine.ini` — the default settings file;
- `ddraw.dbg.dll` — the same DLL with debug information (for `llvm-symbolizer`; not for the game folder);
- `test_*.exe` — the offline tests run by `run_tests.ps1`.

## Code layout

- `proxy/` — the DLL's entry point (C), its export and the offline tests.
- `src/core/` — the game-independent engine: hooks, the DirectDraw emulation and output (SDL3), settings, the panel (Dear ImGui), diagnostics. **No game addresses here**, so other games can be added later.
- `src/games/emperor/` — everything specific to Emperor: addresses, structures and the hooks on the game's functions. Every hook checks the game's bytes before installing and falls back to the original behaviour on failure.
- `third_party/` — vendored libraries (licenses in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

The design notes and reverse-engineering records are kept in Traditional Chinese for now and are not published yet; they will follow once translated. Comments in the code refer to them (`docs/...`).
