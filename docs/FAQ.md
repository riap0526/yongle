# FAQ

Back to the [README](../README.md).

**[About Yongle](#about-yongle)**

- [Is there a macOS or Linux version?](#is-there-a-macos-or-linux-version)
- [Which versions of Emperor does Yongle work with?](#which-versions-of-emperor-does-yongle-work-with)
- [Can I play on an older version of Windows?](#can-i-play-on-an-older-version-of-windows)
- [Are my saved games safe? Can I go back to the original game?](#are-my-saved-games-safe-can-i-go-back-to-the-original-game)
- [Does Yongle change how the game plays?](#does-yongle-change-how-the-game-plays)
- [Do multiplayer and the Campaign Creator work?](#do-multiplayer-and-the-campaign-creator-work)
- [Can I use Yongle together with cnc-ddraw, dgVoodoo or another ddraw.dll?](#can-i-use-yongle-together-with-cnc-ddraw-dgvoodoo-or-another-ddrawdll)

**[Display and settings](#display-and-settings)**

- [The text is too small on my screen. Can it be larger?](#the-text-is-too-small-on-my-screen-can-it-be-larger)
- [The screen looks wrong after I changed a setting. How do I undo it?](#the-screen-looks-wrong-after-i-changed-a-setting-how-do-i-undo-it)
- [How do I skip the intro movie?](#how-do-i-skip-the-intro-movie)
- [Why Direct3D 11 by default?](#why-direct3d-11-by-default)
- [How do I switch the renderer?](#how-do-i-switch-the-renderer)

**[Files and the log](#files-and-the-log)**

- [Where are Yongle's files?](#where-are-yongles-files)
- [What is the log for, and what is in it?](#what-is-the-log-for-and-what-is-in-it)
- [I updated from 0.1.3 or earlier and my game is under Program Files](#i-updated-from-013-or-earlier-and-my-game-is-under-program-files)

## About Yongle

### Is there a macOS or Linux version?

Not for now. Yongle works inside the original Windows `Emperor.exe`, so it runs where that runs. Native macOS and Linux versions are a long-term goal, for after the reimplementation that no longer needs the original executable (subject to change: this is a goal, not a promise).

- **Linux:** a player reports that the game with Yongle works through **Proton** (for example by adding the GOG game to Steam as a non-Steam game, or with a launcher such as Heroic that uses Proton or Wine).
- **macOS:** no reports yet. Tools that run Windows games on a Mac in the same way as Proton, such as **CrossOver** (Wine-based), may work. If you try one, please tell us how it went.

### Which versions of Emperor does Yongle work with?

Yongle is made and tested with the **GOG release**, version 1.0.1.0 (the `Emperor.exe` SHA256 is in the [README](../README.md#requirements)). Other releases (the original CDs, other stores) and other language editions have not been tested. Before changing the game, Yongle checks that the parts of `Emperor.exe` it changes are the ones it knows; if they are not, none of its game changes are made, and the game runs as the original at its own 1024×768, only shown in Yongle's window (the log says so at the start). If you have another version, please tell us how it went, with the log.

**Use an unmodified `Emperor.exe`.** An exe changed by another tool, such as a widescreen patch, may stop Yongle from working, or cause problems: Yongle only checks the parts it changes itself, not everything such a tool may have changed. If you have used one, restore the original exe (for example by reinstalling or verifying the game in GOG Galaxy) before installing Yongle.

### Can I play on an older version of Windows?

Yongle is made for Windows 10 and 11 and tested on Windows 11. In theory it can also start on Windows 7 SP1 and 8.1, and on 32-bit Windows, but none of these have been tested:

- **Windows 7 and 8.1** need Microsoft's **Universal C Runtime** update (KB2999226) first; without it `ddraw.dll` cannot load and the game does not start (delete `ddraw.dll` to play without Yongle again).
- Some newer display features are only in Windows 10 and later; Yongle then uses the older way, or leaves them out.
- **32-bit Windows** gives the game less memory, so a full city screenshot of a very large city may not be possible (you get a message instead).

You are welcome to try. We may not be able to help if Yongle does not start on these versions; if it runs, problems in the game itself are welcome as usual (see [Reporting problems](../README.md#reporting-problems)).

### Are my saved games safe? Can I go back to the original game?

Yes. Yongle does not change the game's files or its formats: saved games, maps and campaigns (including custom ones) are the original game's, saves made with Yongle open in the game without it, and the other way round. To go back, delete `ddraw.dll`.

This may change in the future: options that change the game's rules or limits may need a save format of Yongle's own, as in Augustus. They would be optional, and the original saves, maps and campaigns would still load.

### Does Yongle change how the game plays?

Not so far: everything Yongle does today is about the display, the sound and the controls, and the city's simulation is the original game's. Later, guided by what players ask for, Yongle may add gameplay improvements and higher limits, closer to what Augustus does for Caesar 3 than to a strictly faithful remake. They would be options you choose, marked when they affect whether saves still work in the original game.

### Do multiplayer and the Campaign Creator work?

The Campaign Creator (`EmperorEdit.exe`) works as before. The multiplayer screens and the editors run at the game's own 1024×768, centered on the screen; multiplayer games themselves have not been tested yet. Support for these screens at your resolution may come later.

### Can I use Yongle together with cnc-ddraw, dgVoodoo or another `ddraw.dll`?

No: they are all a `ddraw.dll` in the game folder, and only one can be used. Yongle already does what they are usually used for with this game (a borderless full screen or a window at any resolution, with sharp scaling). Keep a backup of the other `ddraw.dll` if you want to switch back.

## Display and settings

### The text is too small on my screen. Can it be larger?

Yes: raise the **Display scale** (Options > Video), or choose *Auto* (1080 pixels high on taller screens). The whole game is drawn larger, text included, and every screen follows it. Sharper, scalable fonts may be looked at later, together with localization.

### The screen looks wrong after I changed a setting. How do I undo it?

- **Resolution and display scale** ask "Keep this setting?" after a change: choose Revert, or wait 15 seconds and the old value comes back.
- For the other screen settings, open the **debug panel** (Insert), which has the settings for fixing the display, and change it back there.
- If the game cannot be used at all, close it and change the setting in `emperor_engine.ini` (the file explains each one). To start again with every setting at its default, delete both `emperor_engine.ini` and `emperor_engine.ini.bak`. The game's own options (game speed, volumes and so on) are kept either way.

### How do I skip the intro movie?

Set **Intro movie** to `skip` in Options > General Settings, or `intro_movie = skip` in the `[ui]` section of `emperor_engine.ini`. It takes effect the next time the game starts. Yongle does not change the game's own `Emperor.ini`: without Yongle the game does what that file says, and if it already has `PlayIntroMovie=no` in its `[Config]` section, the movie is skipped either way.

### Why Direct3D 11 by default?

Both renderers draw practically the same picture, and on the computers we have tested they run at about the same speed. Direct3D 11 is the default because:

- It needs a little less CPU time per frame than Direct3D 9.
- The `sharp_bilinear` scaling option is done in one step in Direct3D 11 and in two in Direct3D 9 (the result looks almost the same).

Direct3D 9 stays available, and Yongle switches to it automatically if Direct3D 11 cannot start. If you see stutters, flickering or a picture that looks wrong, try Direct3D 9 (below) and please report it with the log.

### How do I switch the renderer?

Yongle uses Direct3D 11 by default (`renderer = auto`; Direct3D 9 only if Direct3D 11 cannot start). If you see stutters or a picture that looks wrong, try Direct3D 9: set **Renderer** to *Direct3D 9* in Options > Video > Display, and restart the game (the change takes effect at the next start). If you cannot change it in the game, set `renderer = direct3d` in the `[video]` section of `emperor_engine.ini` instead. *Direct3D 11* (`renderer = direct3d11`) forces Direct3D 11. If Direct3D 9 helps, please report it with the log (see [Reporting problems](../README.md#reporting-problems)).

## Files and the log

### Where are Yongle's files?

It depends on whether Windows lets a standard user (not an administrator) write to the game folder:

- **Yes** (most installs outside Program Files, e.g. `D:\Games\Emperor`): in the **game folder**, next to `ddraw.dll`. This includes games installed by GOG Galaxy in its default folder, `C:\Program Files (x86)\GOG Galaxy\Games` or `C:\Program Files\GOG Galaxy\Games`: GOG Galaxy lets standard users write there, even though it is under Program Files.
- **No** (e.g. a game copied or installed elsewhere under `C:\Program Files (x86)`): in **`Documents\My Games\Yongle`** (your Documents folder, wherever Windows keeps it). If Yongle cannot write there either, for example because Windows Security's *Controlled folder access* blocks `Emperor.exe`, they are in `%LOCALAPPDATA%\Yongle` instead.

The answer is the same whether or not the game runs as administrator, so there is always one set of files. The debug panel (Insert) shows the folder at the bottom, with an **Open folder** button; the log's `files:` lines say why it was chosen.

The files:

- `ddraw_proxy.log`: the log. Over 4 MB it is renamed to `ddraw_proxy.old.log` at the next start; it can be deleted at any time.
- `emperor_engine.ini`: the settings. When a new version of Yongle first starts, it brings the file's descriptions up to date (your values are kept) and keeps the file as it was in `emperor_engine.ini.bak`; the same backup is written when old settings are cleaned up. If the file is missing, it is made again, with the values of `emperor_engine.ini.bak` when there is one, otherwise at the defaults.
- `menu_background_auto.png`: the last city you played, for the menu backdrop.
- `shots\`: screenshots (F11), full city and minimap screenshots (F10, Shift+F10) and problem reports (F12).

To uninstall, delete `ddraw.dll`, and if you like `ddraw.syms` and these files (or the whole `Documents\My Games\Yongle` folder) too. Nothing else was changed.

### What is the log for, and what is in it?

`ddraw_proxy.log` is how we find and fix problems. Yongle changes how the game draws, scrolls and runs, and most problems only happen on some computers, so when you report one, the log tells us what Yongle did on yours: which renderer and resolution it used, which settings were in effect, how long each frame took, and the details of any error. Julius, Augustus and Akhenaten write a similar log file for the same reason.

- **Nothing is sent anywhere.** Yongle has no network code. The log stays on your computer, and only you decide whether to share it.
- **What it contains:** Yongle's settings, your screen resolution and refresh rate, the number of processors, the renderer and display driver, programs that load into the game (for example the Steam overlay), timings, and error details.
- **What it does not contain:** your name, email, accounts, computer name, IP address or saved games.
- **File paths:** the log shows where the game and Yongle's files are, for example `Documents\My Games\Yongle` inside your Windows user folder, whose name is usually your Windows user name, so the log can include it. If you'd rather not share it, replace it with something else before posting the log publicly; that does not affect what we can learn from it.
- You can delete the log at any time; Yongle starts a new one at the next start.

### I updated from 0.1.3 or earlier and my game is under Program Files

At the first start Yongle copies your settings to `Documents\My Games\Yongle`: the newest `emperor_engine.ini` of the game folder and of Windows' VirtualStore (`%LOCALAPPDATA%\VirtualStore\Program Files (x86)\` followed by the game folder's path, where earlier versions' files went when the game did not run as administrator). From then on the copy in `Documents\My Games\Yongle` is the one used; editing the one in the game folder has no effect. Older logs and screenshots are left where they were.
