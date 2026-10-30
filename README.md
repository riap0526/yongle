# Yongle

**Modern display and quality-of-life improvements for *Emperor: Rise of the Middle Kingdom* (2002), without touching the game's files.**

Yongle runs the original game at your monitor's resolution and refresh rate, lays its interface out for wide and high-resolution screens, scrolls the city smoothly and keeps the game speed steady in large cities. It is named after the Yongle Emperor of the Ming dynasty, who built the Forbidden City — in the tradition of [Julius](https://github.com/bvschaik/julius), [Augustus](https://github.com/Keriew/augustus) and [Akhenaten](https://github.com/dalerank/Akhenaten) for the other Impressions city builders.

> **Status: early (v0.1.2).** Tested on the GOG release of Emperor (version 1.0.1.0) on Windows 11. Expect rough edges; please report what you find (see [Reporting problems](#reporting-problems)).

![A city at 2560×1440 with Yongle (display scale 100 %)](screenshots/city-1440p.webp)
*A city at 2560×1440 with Yongle, display scale 100 %. Click any screenshot for full size.*

<details>
<summary>For comparison: the original game at 1024×768</summary>

![The original game at 1024×768](screenshots/compare-1024.webp)

</details>

## How it works

Yongle is a single `ddraw.dll` placed in the game folder. Windows loads it instead of the system's DirectDraw, and from there it **hooks** the original `Emperor.exe` in memory: it provides its own DirectDraw implementation (drawn with SDL3 and Direct3D 11, or Direct3D 9) and changes or replaces individual game functions while the game runs.

- **Your game stays clean.** `Emperor.exe`, the data files and your save games are never modified. Delete `ddraw.dll` and the game is exactly as before.
- Yongle's settings live in its own file, `emperor_engine.ini`. The game's own `Emperor.inf` only ever receives values the original game understands.
- Every hook first checks that the game's code is the expected version; if something does not match, or a feature fails at run time, that feature switches itself off, writes the reason to the log, and the game carries on as the original would.

This follows the [OpenRCT2](https://github.com/OpenRCT2/OpenRCT2) approach: start inside the original executable and replace its parts step by step, keeping the game playable at every step. A full **reimplementation** that no longer needs `Emperor.exe` (as Julius/Augustus did for Caesar III) is a **long-term goal**, not the current state.

**The original art stays as it is.** Yongle aims to preserve the game's own art direction and atmosphere: everything on screen is drawn from the game's own images, at their own size or scaled sharply. There are no plans for AI-generated or AI-upscaled backgrounds, artwork or other images.

## Features

### Display

- **No more display mode changes.** The desktop resolution is never touched; the game is shown in a borderless full-screen window on your monitor, scaled sharply (nearest pixels with a one-pixel blend at their edges).

- **Smooth frame rate.** Frames follow your monitor's refresh rate instead of the original's ~20 per second. The game's own timing (simulation, animations) is unchanged.

- **Any resolution, laid out for your screen.** The game draws at your native resolution divided by a display scale (100–250 % in 5 % steps, or *Auto*: 1080 pixels high on taller screens, e.g. 1920×1080 on a 2560×1440 monitor). The sidebar and top bar extend to the screen's edges, the city fills the rest, and dialogs are centered.

- **Full-screen pictures** (main menu, loading screens) are shown at their own size in a frame instead of being stretched. Around the menus: a blurred, darkened picture of the last city you played — or your own picture (see [docs/MENU_BACKGROUND.md](docs/MENU_BACKGROUND.md)).

  ![The main menu over a blurred picture of the last city](screenshots/main-menu.webp)

- **China map** at its own size or enlarged, centered over a blurred backdrop of itself.

  ![The China map at its own size](screenshots/china-map.webp)

- **Windowed mode**: F5 or Alt+Enter. The window can be moved, resized and maximized; the game is laid out again for its new size, and its size and position are remembered. The cursor can optionally be kept inside the window.

- **In-game Display options** (Options → Display options): window mode, the game's own 1024×768, the display scale, 100 %, Auto. Changes apply at once; OK saves them. More display settings will move here from the settings panel.

  ![The game's Display options dialog with Yongle's settings](screenshots/display-options.webp)

- Where a building's information window opens: original, centered, or next to the cursor (default).

### Gameplay feel

- **Pixel-smooth scrolling** of the city at the game's own average speed, with easing.
- **Steady game speed.** In the original, a large city at a high resolution runs slower, because the simulation waits for drawing. Yongle catches up on the missed simulation steps (as Augustus does), so every resolution runs at the speed you set.
- **Predictable 100 % speed.** The original's 100 % runs as fast as the computer can draw; by default Yongle caps it to a steady pace one step above 90 % (the original behavior is a setting).
- **The city keeps running while you scroll** (the original pauses it; a setting).

### Tools

- **Settings panel** (Insert) for everything above, saved to `emperor_engine.ini`. Most settings apply immediately.

  ![The settings panel](screenshots/settings-panel.webp)

- **F11** saves a screenshot of the game's picture to `shots\`.

- **F12** writes a problem report (log mark, screenshots, statistics) for bug reports.

- An exception log with function names and source lines (`ddraw_proxy.log`, using `ddraw.syms`).

- The game's separate editor (`EmperorEdit.exe`, the Campaign Creator) runs unchanged.

## Requirements

- *Emperor: Rise of the Middle Kingdom*, version 1.0.1.0. Developed and tested with the **GOG release** (`Emperor.exe` SHA256 `6373328bfc5c4886d9abc9544eb706e89e7d465b18176ea8205fe27aaee53c0e`). Other copies of 1.0.1.0 may work; on an unknown build the hooks do not install and the game runs as the original.
- Windows 10 or 11 and a graphics card with Direct3D 11.

## Installation

1. Download the latest release (`yongle-<version>.zip`) and unpack it. `ddraw.dll` is not code-signed, so antivirus or SmartScreen may warn about it; you can check the zip against the SHA256 on the release page.
2. Copy `ddraw.dll`, `ddraw.syms` and `emperor_engine.ini` into the game folder (the folder with `Emperor.exe`). If the folder already has a `ddraw.dll` from another tool (cnc-ddraw, dgVoodoo and so on), back it up first: only one can be used.
3. Start the game as usual.

**Uninstall:** delete `ddraw.dll` (and, if you like, `ddraw.syms`, `emperor_engine.ini`, `ddraw_proxy.log`, `menu_background_auto.png` and the `shots\` folder). Nothing else was changed.

Files Yongle creates in the game folder: `ddraw_proxy.log` (log; over 4 MB it is renamed to `ddraw_proxy.old.log` at the next start, and it can be deleted at any time), `emperor_engine.ini` (settings; a backup `emperor_engine.ini.bak` when old settings are cleaned up), `menu_background_auto.png` (the last city, for the menu backdrop) and `shots\` (F11 / F12).

## Keys

| Key             | Action                                                                                                |
| --------------- | ----------------------------------------------------------------------------------------------------- |
| Insert          | Open / close the settings panel (the game pauses while it is open)                                    |
| F5 or Alt+Enter | Switch between full screen and a window                                                               |
| F11             | Screenshot to `shots\`                                                                                |
| F12             | Problem report (when something looks wrong)                                                           |
| Shift+F7        | Cycle through a few resolutions until the game ends (for trying them out; the setting is not changed) |

The panel, screenshot and report keys can be changed in `emperor_engine.ini`.

## Settings

All settings are in `emperor_engine.ini` next to `ddraw.dll`; the file explains each one, and the panel (Insert) shows the same settings with descriptions. A missing file or setting means the default. The defaults: full screen at your native resolution (display scale 100 %), smooth scrolling on, steady game speed on, 100 % speed capped, the city running while scrolling.

## Known issues

The full list is in **[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)**. The main points:

- On laptops and slower computers, **lowering the resolution** (a higher display scale, or *Auto*) helps performance most.
- **Steam's FPS counter** makes Direct3D 11 stutter, so when the Steam overlay is in the game Yongle plays it safe and uses Direct3D 9 (about 25 % fewer frames per second). If you do not use the FPS counter, `renderer = direct3d11` in `emperor_engine.ini` gets that performance back; if Direct3D 11 stutters on your system, `renderer = direct3d` switches to Direct3D 9 by hand.
- Occasional **flashing black blocks** (seen on one AMD system, Direct3D 11 only); the cause is not known yet.
- Tested on AMD graphics and Intel integrated graphics so far; NVIDIA, real 4K monitors and several monitors not yet.

## Roadmap

See **[docs/ROADMAP.md](docs/ROADMAP.md)** for what is planned next and what comes later.

## Building from source

See **[docs/BUILDING.md](docs/BUILDING.md)** for the tools, the build steps, the offline tests and the code layout.

## Reporting problems

Please open an issue with the **Bug report** form (it walks you through the same steps we use in our own tests). In short:

- close the game and delete `ddraw_proxy.log` in the game folder (it collects several sessions);
- reproduce the problem and press **F12** the moment you see it (it saves screenshots to `shots\` and marks the log);
- send the new `ddraw_proxy.log`, the `shots\report*` pictures and, if you changed settings, `emperor_engine.ini`;
- say what you did and what you saw, and give your screen resolution, Windows version and graphics card.

## Contributing

Contributions are welcome. For anything beyond a small fix, please open an issue first to discuss it.

- Keep the two layers apart: nothing game-specific (addresses, structures) in `src/core/`.
- Every hook must check the game's bytes before installing and fall back to the original behavior on failure.
- Never add files from the game (art, text, data) or derived from them to the repository.
- No AI-generated or AI-upscaled images (backgrounds, artwork, interface graphics): the project keeps the original art style, so such contributions will not be accepted.
- Run the offline tests before submitting.
- By contributing you agree that your contribution is licensed under the AGPL-3.0.

## Credits

**Projects and libraries**

- **[Augustus](https://github.com/Keriew/augustus)** (AGPL-3.0): its speed easing code (`core/speed.c`) is used for smooth scrolling (see `third_party/augustus/`), and its display scale, game speed and configuration designs were the models for ours.
- **[Julius](https://github.com/bvschaik/julius)**: studied for its rendering and timing.
- **[Akhenaten](https://github.com/dalerank/Akhenaten)**: its description of the `.sg3` image format was the reference for reading the game's own images.
- **[OpenRCT2](https://github.com/OpenRCT2/OpenRCT2)**: the approach of starting inside the original executable.
- **[zeus-qol](https://github.com/andrehamon/zeus-qol)**: studied for how its Zeus improvements are structured (no code used).
- Libraries: [SDL3](https://github.com/libsdl-org/SDL) (zlib), [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause), miniz (public domain, the copy in SDL). Their licenses are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- *Emperor: Rise of the Middle Kingdom* was made by BreakAway Games and Impressions Games and published by Sierra.

## Disclaimer

Yongle is an unofficial fan project. It is not affiliated with or endorsed by the owners of *Emperor: Rise of the Middle Kingdom*. You need your own copy of the game; Yongle contains none of its files.

**This project is developed with AI assistance** (Anthropic's Claude writes much of the code and analysis). Every stage is tested by a human in the actual game before it is accepted, with a check that the original game still starts and loads its saves without the DLL. Still, use it at your own risk and keep backups of your saves.

## License

Yongle is licensed under the **GNU Affero General Public License v3.0** — see [LICENSE](LICENSE). Third-party components keep their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
