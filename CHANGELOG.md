# Changelog

All notable changes to Yongle. Downloads, with the SHA256 of each zip, are on the [Releases](https://github.com/riap0526/yongle/releases) page. To upgrade, replace `ddraw.dll` and `ddraw.syms` in the game folder and keep your `emperor_engine.ini`: new settings take their defaults when they are missing.

## 0.1.3 (2026-11-02)

### Changed

- **The music and the sounds pause when you switch away** (Alt+Tab, clicking another window, minimizing) and go on where they were when you come back. The original game stops them all and starts another song when you return; set `background_sound = original` in the new `[sound]` section to keep that.
- **Ambient sounds no longer depend on your resolution and computer.** The game plays a random city sound once in about 300 passes of its main loop, so a fast computer at 1024×768 was very noisy (about one per second) and a large resolution far quieter (about one in four seconds). Now it is about one every two seconds whatever the resolution (`[sound] ambient_sounds = 0.5`; also 0.25, 0.75, 1.0, or `original`).
- **Direct3D 11 is always the default** (`renderer = auto`); Steam's FPS counter no longer makes it stop for seconds at a time. Yongle's own work and the graphics driver now run on the other processor cores, while the game stays on the one core the GOG release gives it (`[compat] cpu_affinity`, for troubleshooting only).
- **Screenshots no longer slow the game down** (F11, F12).
- **Output scaling can be switched at once**: `nearest`, `linear` (new) or `sharp_bilinear`, in Display options and the settings panel. `renderer = direct3d` now has sharp bilinear too.
- **Menus and loading screens**: a new `original` look (the whole 1024×768 picture at its own size, black around); the blur and brightness of the city behind the menus can be set (`background_blur`, `background_brightness`). The screens before a mission and the editors have parchment around them instead of the city you played last, since the next mission is usually on another map (your own `menu_background` picture is still shown there).
- **China map**: a scale of your own (`china_map_scale`, in percent) and the city as you left it behind the map (`china_map_background = city`).

### Fixed

- **Clicks on the sidebar or the top bar no longer reach the city.** While holding a building or a road, a click on a sidebar button could also build right under the sidebar, or under the top bar (since 0.1.0, whenever the smooth-scrolled view had stopped between two tiles). The cursor's tile and the construction preview now stop at the edge of the city as you see it.

### Upgrade notes

- `china_map_integer` is gone: if you had it `on`, the China map (China map = scaled) now fills the map area; use China map: scale to set the size you want. The old line is removed from `emperor_engine.ini` at the first start (the file as it was is kept as `emperor_engine.ini.bak`).
- `framed` and `black` in `loading_screen` / `menu_screens` are now called `scaled` and `scaled_black`; your file is updated at the first start (also kept as `.bak`).

## 0.1.2 (2026-10-30)

### Changed

- **Direct3D 9 when the Steam overlay is in the game.** Steam's in-game FPS counter made Direct3D 11 stop for about 2.5 seconds at a time, so `renderer = auto` (new in `[display]`) picked Direct3D 9 when the Steam overlay was loaded, at about 25 % fewer frames per second. (0.1.3 removes the cause and always uses Direct3D 11.)
- **Screenshots no longer freeze the game** (F11, F12; worst on laptops). Pictures are saved in the background, one at a time, at most six waiting.
- **No more jumping city when the computer cannot keep up.** When the game falls far behind, Yongle stops catching up on all the missed simulation steps at once; the game runs slower for a moment instead of skipping ahead.
- The log is limited: over 4 MB it is renamed to `ddraw_proxy.old.log` at the next start.
- The README is shorter; the known issues, the roadmap, building and the menu background are in [docs/](docs/).

### Fixed

- **Error logs show Yongle's function names.** In 0.1.0 and 0.1.1 the symbol table `ddraw.syms` did not match the DLL, so crash reports only had addresses.
- In a window, the picture is laid out again once you let go of the window's edge, not while you drag it.

### For bug reports

- The log records what runs alongside the game (overlays and recorders, the power plan, how much processor time the game gets) and samples long freezes.

## 0.1.1 (2026-10-25)

### Fixed

- **Full screen did not cover the screen on GOG installs**: the game sat about 30 pixels too high (the top bar was cut off) and the Windows taskbar stayed on top of it. GOG installs Emperor with a Windows compatibility setting (`DWM8And16BitMitigation`) that moves the game window up each time the game changes its display mode; Yongle now places its window past that setting and puts it back whenever it is moved off the screen. The setting itself is left as GOG installed it.

### Changed

- The log notes when a Windows compatibility setting is active.

## 0.1.0 (2026-10-25)

First public release, for *Emperor: Rise of the Middle Kingdom* (GOG, version 1.0.1.0). Use 0.1.1 or later instead: on GOG installs, 0.1.0 does not cover the whole screen.

- The game at your monitor's resolution and refresh rate, with the interface laid out for wide and high-resolution screens (display scale 100–250 % or Auto).
- Pixel-smooth scrolling and a steady game speed in large cities.
- Menus and the China map at their own size over a blurred backdrop; windowed mode (F5 / Alt+Enter).
- Yongle's settings in the game's own Display options, plus a settings panel (Insert).
