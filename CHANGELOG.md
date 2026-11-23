# Changelog

All notable changes to Yongle. Downloads, with the SHA256 of each zip, are on the [Releases](https://github.com/riap0526/yongle/releases) page. To upgrade, replace `ddraw.dll` and `ddraw.syms` in the game folder and keep your `emperor_engine.ini`: new settings take their defaults when they are missing (from 0.2.0 on, the file's descriptions are also brought up to date at the first start, with your values kept).

## 0.2.1 (2026-11-23)

### Added

- **Drag scrolling.** Hold the middle mouse button over the city and move the mouse: the city follows it. The cursor is hidden while you drag and comes back where you pressed. A click without moving still goes back to 100 % zoom. It can be turned off in Options.
- **Difficulty and auto defend.** The game has both, but never lets you change them. While you play a city alone, they are now in Options > General > Difficulty and in the Options menu. Auto defend sends your soldiers out by themselves when enemies come. The game has no switch for it: turn it off before removing Yongle.
- **Edge scrolling width.** How close to the edge of the screen the cursor has to be to scroll the city, from 1 pixel (as before) to 20, or 0 for none. In Options.
- **A black frame for the menus.** Options > Video > Menu screens has a new choice, *Framed*: the menus, briefings and other full-screen pictures in a fret border with black around them.

### Changed

- **New scroll speeds.** With smooth scrolling, each scroll speed setting is now the same on every computer, and slower than before. If you used 30 %, try 50 %.
- **Nature sounds follow the game's 3D sound design.** Each comes down from above, moves around you and rises away, as the game's files describe, now in stereo.
- **Esc on the end-of-mission screens** now does nothing, instead of asking whether to quit the game. Videos still skip with Esc.
- **Options window buttons.** The button under the mouse turns darker, like the game's own buttons, instead of getting a thick frame. Value boxes now reach the arrows.
- **Screenshots from the city view only.** The city and minimap screenshot keys work only when no window or message is open. Otherwise a message tells you why.
- **A notice for other versions of the game.** With any `Emperor.exe` other than the English GOG release 1.0.1.0, "Untested Emperor.exe" shows for 20 seconds at the start.
- **No error sound for Alt with a letter.** Alt with a letter that has no action no longer makes the Windows error sound.

### Fixed

- Yongle's messages (for example "City screenshot saved") were lost when a window or message was open at that moment. They now appear when you are back in the city.
- Loading a saved game kept the zoom you had before. It now goes back to the starting zoom (Options > Video > City).
- At every zoom level, the view now stops at the map's east and south edges in the same place as the original game.
- Holding a list's scroll arrow scrolled far too fast on modern computers. It now scrolls one line every 50 ms, as on computers of the time.
- In the Exit game, Replay map and Exit to main menu boxes, the title and text now sit in the middle of the box.
- In the Pause menu, the button under the mouse now turns darker like the game's own buttons, instead of getting a thick frame.
- New players on the GOG version started at the install's speeds instead of 80 % game speed and 50 % scroll speed. They now get these.
- A new resolution chosen on a menu screen (for example with Shift+F7) now applies at once, not only after you go to another screen.
- A fixed resolution with an odd width (for example 1365×768) drew the picture slanted. The width is now made one pixel smaller.
- Below 786 pixels high (for example 1366×768), the empire map sat in the top left corner next to the city. It is now centered, framed and shrunk to fit.
- Esc with a sidebar window open sometimes opened the help for a sidebar button.
- The Options window could stay on the screen after closing the mission goals or a similar screen.
- Changing the resolution or window mode made the back of the mission goals black. The help and About screens now close first.
- After a pause, the city sometimes played many ambient sounds at once.
- With `backend = system`, Alt+1 to Alt+5 save the game's own BMP screenshot again (the game runs at its own sizes there).

### For bug reports

- At every start, the log records which `Emperor.exe` runs: its size, time stamp and SHA-256.

### Upgrade notes

- `menu_screens = parchment` is now called `panel` (the look is the same). Your file is changed at the first start; the file as it was is kept as `emperor_engine.ini.bak`.
- `framed` in `menu_screens` is now the new black frame. A file from 0.1.2 or earlier that still has `framed` there gets the new look, not `panel`.
- New settings: `drag_scroll` and `edge_scroll_width` in `[gameplay]`. They take their defaults when they are missing.

## 0.2.0 (2026-11-09)

### Added

- **Options window.** A new settings window that looks like the game's own dialogs. It has the game's settings (game speed, scroll speed, autosave, messages, volumes) and Yongle's settings together, on General, Video and Audio pages. Open it with F6, from the Options menu in the city, or from the main menu (Options replaces Visit Website). If you change the resolution or display scale, the game asks "Keep this setting?" and switches back after 15 seconds if you don't answer. The game's own settings are still saved in the game's files.
- **Key rebinding** in Options > Hotkeys. Almost every key can be changed, with two keys per action. Ctrl, Shift, Alt and the mouse side buttons work too. The default keys are the same as the original game. The "Game paused" message shows your pause key.
- **Esc closes windows and opens a Pause menu.** Esc now closes any open window, like a right click. This includes confirmation boxes, which before could only be closed with the mouse. In the city, Esc cancels the building tool or the selected company first; if there is nothing to cancel, it opens the Pause menu (Continue, Replay map, Load game, Save game, Delete game, Options, Exit to main menu, Exit game). In the city, Esc no longer asks if you want to quit: **to quit, use Exit game** in the Pause menu or the File menu. On the menu screens (campaigns, Open Play, high scores and so on), Esc goes back, like a right click. On the main menu, Esc asks before quitting instead of quitting right away.
- **Full city and minimap screenshots.** F10 saves the whole city as one PNG, at the game's normal size and without the interface. Shift+F10 saves the minimap. Both are also in the File menu. A big city gives a picture of about 9000×4500 pixels.
- **City zoom with the mouse wheel**, from 50 % to 150 %, toward the cursor. A "Zoom" message shows the current level. The middle mouse button goes back to 100 %. Over the sidebar, the wheel still changes the building category. The starting zoom can be set in Options > Video. Saved games are not affected.
- **Unused nature sounds, on by default.** The game comes with recordings of birds, wind, crickets, frogs and wind chimes that it never actually played. Yongle now plays them now and then in the city, moving slowly around you. To turn them off, use Options > Audio > Unused nature sounds, or set `nature_sounds = off` in `[audio]`.
- **Master volume and mute** for all music and sounds (Options > Audio). The game's own volume settings are kept.
- **Skip the intro movie** (Options > General Settings).

### Changed

- **The display scale applies to every screen**, not just the city: menus, briefings and loading screens get bigger too. *Menu screens* now only sets the background around them (city, parchment or original), and so does *Loading screens* (framed or original).
- **Yongle's messages use the game's message boxes** at the top of the city, with errors in red like the game's.
- **New default speeds for new players**: 80 % game speed and 30 % scroll speed (the original defaults are 90 % and 60 %). If you have played before, your settings stay the same.
- **Yongle's files go to Documents when needed.** If the game folder needs administrator rights to write to (for example under `C:\Program Files (x86)`), Yongle's settings, log and screenshots now go to `Documents\My Games\Yongle` instead of Windows' VirtualStore. Your settings are copied there at the first start.
- **`emperor_engine.ini` is updated when you upgrade.** It gets the new version's descriptions and keeps your values. The old file is saved as `emperor_engine.ini.bak`. If the file is missing, it is created again.
- **The settings panel is now a debug panel** (Insert), for testing and for fixing the display if something goes wrong. All other settings are in Options.
- **Ctrl+arrow keys are free for your own bindings.** In the original game they scroll like the arrow keys alone; now they do nothing until you bind them in Options > Hotkeys. F6, F7 and F8 are no longer passed to the game.
- **No new ambient sounds while Yongle's windows are open**, the same as with the game's own dialogs.

### Fixed

- Full screen is no longer pushed up by the compatibility setting that GOG installs. Since 0.1.1, Yongle moved the window back each time; now it doesn't get moved at all.
- Alt+1 to Alt+5 crashed the game above 1024×768, because they also try to save a BMP screenshot the game can't make at larger sizes. The BMP is no longer saved. Calling back your companies works as before.
- The city no longer jumps forward for a moment after you close a window or the Pause menu.

### For bug reports

- The log now records how Windows shows the game's picture, and F12 reports include the latest changes, to help track down the rare black squares.

### Upgrade notes

- In `emperor_engine.ini`, `[display]` is now `[video]` and `[sound]` is now `[audio]`. The China map is now called the empire map (`empire_map` and so on), and some values of `menu_screens` and `loading_screen` were renamed. Your file is converted at the first start and keeps your values.
- Keys are in the new `[keys]` section. Each action can have two keys, separated by a comma.
- `present = original` is no longer offered in Options or the debug panel, because neither works with it. If your file has it, it becomes `display`. You can still set it by hand for troubleshooting.

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
