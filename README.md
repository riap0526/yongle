# Yongle

**Modern display and quality-of-life improvements for *Emperor: Rise of the Middle Kingdom* (2002).**

| Platform | Latest release |
| -------- | -------------- |
| Windows 10 / 11 | [![Download](https://img.shields.io/github/v/release/riap0526/yongle?label=Download&color=0a7bbb)](https://github.com/riap0526/yongle/releases/latest) |

On the release page, download **`yongle-<version>.zip`**, not "Source code (zip)": the source code has no `ddraw.dll`. Then see [Installation](#installation).

Yongle lets the original game run at your monitor's resolution and refresh rate, fits the interface to wide and high-resolution screens, adds smooth scrolling and city zoom, and keeps the game speed steady in big cities. Its settings and key bindings are in an Options window that looks like the game's own dialogs. It is named after the Yongle Emperor of the Ming dynasty, who built the Forbidden City — in the tradition of [Julius](https://github.com/bvschaik/julius), [Augustus](https://github.com/Keriew/augustus) and [Akhenaten](https://github.com/dalerank/Akhenaten) for the other Impressions city builders.

> **Status: early (v0.2.0).** Tested on the GOG release of Emperor (version 1.0.1.0) on Windows 11. Expect rough edges; please report what you find (see [Reporting problems](#reporting-problems)).

![A city at 2560×1440 with Yongle (display scale 100 %)](screenshots/city-1440p.webp)
*A city at 2560×1440 with Yongle, display scale 100 %. Click any screenshot for full size.*

<details>
<summary>For comparison: the original game at 1024×768</summary>

![The original game at 1024×768](screenshots/compare-1024.webp)

</details>

## Features and Roadmap

See **[docs/FEATURES.md](docs/FEATURES.md)** for details and screenshots of each feature.

See **[docs/ROADMAP.md](docs/ROADMAP.md)** for what is planned next and what comes later.

See **[CHANGELOG.md](CHANGELOG.md)** for what changed in each release.

- **No display mode changes:** the game runs in a borderless full-screen window at your desktop resolution, scaled up sharply.
- **Smooth frame rate** at your monitor's refresh rate. The game's own timing doesn't change.
- **Any resolution:** the sidebar, top bar, city and dialogs fit your screen, and a display scale (100–250 % or *Auto*) makes every screen bigger.
- **Menus, loading screens and the empire map** are shown in a frame, over a blurred picture of your city.
- **Windowed mode** (F5 or Alt+Enter). You can resize the window, and its size and position are remembered.
- **Smooth scrolling** of the city, at the same average speed as the original.
- **City zoom** with the mouse wheel (50–150 %, less on small maps), toward the cursor. The middle mouse button goes back to 100 %.
- **Options window** (F6) with the game's settings and Yongle's together.
- **Key rebinding** for almost every key, with two keys per action.
- **Esc closes windows** and opens a **Pause menu** in the city.
- **Steady game speed** at any resolution, and a steady 100 % speed.
- **Sound:** a master volume, music and sounds that pause while you're switched away, ambient sounds at a steady rate, and the game's unused nature sounds.
- **Screenshots** (F11), **full city and minimap screenshots** (F10, Shift+F10) and **problem reports** (F12).

## How it works

Yongle is a single `ddraw.dll` in the game folder. It replaces the game's DirectDraw with its own (Direct3D 11 or 9 through SDL3) and changes the original `Emperor.exe` in memory only. The game's files and your saves are never modified. If one of Yongle's features runs into an error, it turns itself off and the game carries on as the original would. Delete `ddraw.dll` and the game is exactly as before. A full reimplementation that no longer needs `Emperor.exe` is a long-term goal.

**The original art stays as it is:** everything on screen is drawn from the game's own images. There are no plans for AI-generated or AI-upscaled backgrounds, artwork or other images.

## Requirements

- *Emperor: Rise of the Middle Kingdom*, version 1.0.1.0. Developed and tested with the **GOG release** (`Emperor.exe` SHA256 `6373328bfc5c4886d9abc9544eb706e89e7d465b18176ea8205fe27aaee53c0e`). Use an unmodified `Emperor.exe`: on an unknown build Yongle makes none of its game changes and the game runs as the original, only shown in Yongle's window; an exe changed by another tool (a widescreen patch, say) may not work or may cause problems (see the [FAQ](docs/FAQ.md#which-versions-of-emperor-does-yongle-work-with)).
- Windows 10 or 11 and a graphics card with Direct3D 11.

## Installation

1. Download `yongle-<version>.zip` from the [latest release](https://github.com/riap0526/yongle/releases/latest) (not "Source code (zip)") and unpack it. `ddraw.dll` is not code-signed, so antivirus or SmartScreen may warn about it; you can check the zip against the SHA256 on the release page.
2. Copy `ddraw.dll`, `ddraw.syms` and `emperor_engine.ini` into the game folder (the folder with `Emperor.exe`). If the folder already has a `ddraw.dll` from another tool (cnc-ddraw, dgVoodoo and so on), back it up first: only one can be used.
3. Start the game as usual.

**Upgrade:** replace `ddraw.dll` and `ddraw.syms` and keep your `emperor_engine.ini`; Yongle brings it up to date at the first start.

**Uninstall:** delete `ddraw.dll`. Which other files Yongle creates, where they are (the game folder, or `Documents\My Games\Yongle` when the game is under Program Files), skipping the intro movie and switching the renderer: see the **[FAQ](docs/FAQ.md)**.

## Keys

Yongle's keys (the defaults):

| Key             | Action                                                                                        |
| --------------- | --------------------------------------------------------------------------------------------- |
| Esc             | Close a window or go back. In the city: drop the building tool, or open the Pause menu. On the main menu: quit (asks first) |
| F6              | Open / close Options                                                                          |
| F5 or Alt+Enter | Switch between full screen and a window                                                       |
| F10             | Full city screenshot to `shots\`                                                              |
| Shift+F10       | Minimap screenshot to `shots\`                                                                |
| F11             | Screenshot to `shots\`                                                                        |
| F12             | Problem report (when something looks wrong)                                                   |
| Insert          | Open / close the debug panel                                                                  |
| Mouse wheel     | Over the city: zoom in and out. Over the sidebar: the building category, as in the original   |
| Middle button   | Over the city: zoom back to 100 %                                                             |

The game's own keys work as in the original. Almost all of them, and Yongle's keys above, can be changed in **Options > Hotkeys**, with a second key for each action. Not offered: the multiplayer and developer keys of the game, keys used for typing text, and Esc, Enter, the mouse wheel and the middle button (F5 and Alt+Enter cannot be changed either).

## Settings

Most settings are in **Options** (on the main menu, in the city's Options menu, or F6), together with the game's own options. The **debug panel** (Insert) has the settings for testing and for fixing the display when something goes wrong. All of Yongle's settings are also in `emperor_engine.ini`, which explains each one; a missing file or setting means the default.

## Known issues

Questions about where Yongle's files are, the intro movie or the renderer? See the **[FAQ](docs/FAQ.md)** first.

The full list is in **[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)**. The main points:

- On laptops and slower computers, **lowering the resolution** (a higher display scale, or *Auto*) helps performance most.
- Game under `C:\Program Files (x86)`: Yongle's files are in **`Documents\My Games\Yongle`**, not in the game folder (see the [FAQ](docs/FAQ.md)).
- Tested on AMD graphics and Intel integrated graphics so far; NVIDIA, real 4K monitors and several monitors not yet.

## Reporting problems

Please open an issue with the **Bug report** form. In short: delete `ddraw_proxy.log` in the game folder, reproduce the problem and press **F12** the moment you see it, then send the new `ddraw_proxy.log`, the `shots\report*` pictures and your `emperor_engine.ini`, with what you did, what you saw, your screen resolution, Windows version and graphics card.

## Building and Contributing

See **[docs/BUILDING.md](docs/BUILDING.md)** for the tools, the build steps, the offline tests and the code layout.

See **[CONTRIBUTING.md](CONTRIBUTING.md)** for how to propose changes and the rules contributions follow.

## Credits

- **[Augustus](https://github.com/Keriew/augustus)** (AGPL-3.0): its speed easing code (`core/speed.c`) is used for smooth scrolling (see `third_party/augustus/`), and its display scale, game speed, configuration, hotkey and pause menu designs were the models for ours.
- **[Julius](https://github.com/bvschaik/julius)**: studied for its rendering and timing.
- **[Akhenaten](https://github.com/dalerank/Akhenaten)**: its description of the `.sg3` image format was the reference for reading the game's own images.
- **[OpenRCT2](https://github.com/OpenRCT2/OpenRCT2)**: the approach of starting inside the original executable.
- **[zeus-qol](https://github.com/andrehamon/zeus-qol)**: studied for how its Zeus improvements are structured, its map scaling and its city image export (no code used).
- Libraries: [SDL3](https://github.com/libsdl-org/SDL) (zlib), [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause), miniz (public domain, the copy in SDL). Their licenses are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- *Emperor: Rise of the Middle Kingdom* was made by BreakAway Games and Impressions Games and published by Sierra.

## Disclaimer

Yongle is an unofficial fan project. It is not affiliated with or endorsed by the owners of *Emperor: Rise of the Middle Kingdom*. You need your own copy of the game; Yongle contains none of its files.

Developed with AI assistance; every stage is tested by a human in the game before release. Use it at your own risk and keep backups of your saves.

## License

Yongle is licensed under the **GNU Affero General Public License v3.0** — see [LICENSE](LICENSE). Third-party components keep their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
