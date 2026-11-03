# FAQ

Back to the [README](../README.md).

## Where are Yongle's files?

It depends on whether Windows lets a standard user (not an administrator) write to the game folder:

- **Yes** (most installs outside Program Files, e.g. `D:\Games\Emperor`): in the **game folder**, next to `ddraw.dll`.
- **No** (e.g. under `C:\Program Files (x86)`): in **`Documents\My Games\Yongle`** (your Documents folder, wherever Windows keeps it). If Yongle cannot write there either, for example because Windows Security's *Controlled folder access* blocks `Emperor.exe`, they are in `%LOCALAPPDATA%\Yongle` instead.

The answer is the same whether or not the game runs as administrator, so there is always one set of files. The settings panel (Insert) shows the folder at the bottom, with an **Open folder** button; the log's `files:` lines say why it was chosen.

The files:

- `ddraw_proxy.log`: the log. Over 4 MB it is renamed to `ddraw_proxy.old.log` at the next start; it can be deleted at any time.
- `emperor_engine.ini`: the settings. When a new version of Yongle first starts, it brings the file's descriptions up to date (your values are kept) and keeps the file as it was in `emperor_engine.ini.bak`; the same backup is written when old settings are cleaned up. If the file is missing, it is made again, with the values of `emperor_engine.ini.bak` when there is one, otherwise at the defaults.
- `menu_background_auto.png`: the last city you played, for the menu backdrop.
- `shots\`: screenshots (F11) and problem reports (F12).

To uninstall, delete `ddraw.dll`, and if you like `ddraw.syms` and these files (or the whole `Documents\My Games\Yongle` folder) too. Nothing else was changed.

## I updated from 0.1.3 or earlier and my game is under Program Files

At the first start Yongle copies your settings to `Documents\My Games\Yongle`: the newest `emperor_engine.ini` of the game folder and of Windows' VirtualStore (`%LOCALAPPDATA%\VirtualStore\Program Files (x86)\` followed by the game folder's path, where earlier versions' files went when the game did not run as administrator). From then on the copy in `Documents\My Games\Yongle` is the one used; editing the one in the game folder has no effect. Older logs and screenshots are left where they were.

## How do I skip the intro movie?

Set **Intro movie** to `skip` in the settings panel (Insert; in the Display section), or `intro_movie = skip` in the `[display]` section of `emperor_engine.ini`. It takes effect the next time the game starts. Yongle does not change the game's own `Emperor.ini`: without Yongle the game does what that file says, and if it already has `PlayIntroMovie=no` in its `[Config]` section, the movie is skipped either way.

## How do I switch the renderer?

Yongle uses Direct3D 11 by default (`renderer = auto`; Direct3D 9 only if Direct3D 11 cannot start). If you see stutters or a picture that looks wrong, try Direct3D 9: set `renderer = direct3d` in the `[display]` section of `emperor_engine.ini` and restart the game. `renderer = direct3d11` forces Direct3D 11. If Direct3D 9 helps, please report it with the log (see [Reporting problems](../README.md#reporting-problems)).
