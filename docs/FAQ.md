# FAQ

Back to the [README](../README.md).

## Which files does Yongle create in the game folder?

- `ddraw_proxy.log`: the log. Over 4 MB it is renamed to `ddraw_proxy.old.log` at the next start; it can be deleted at any time.
- `emperor_engine.ini`: the settings. A backup `emperor_engine.ini.bak` is written when old settings are cleaned up.
- `menu_background_auto.png`: the last city you played, for the menu backdrop.
- `shots\`: screenshots (F11) and problem reports (F12).

To uninstall, delete `ddraw.dll`, and if you like these files and `ddraw.syms` too. Nothing else was changed.

## I can't find the log or the screenshots, or editing the ini has no effect

If the game is under `C:\Program Files (x86)` and does not run as administrator, Windows redirects Yongle's files to `%LOCALAPPDATA%\VirtualStore\Program Files (x86)\` followed by the game folder's own path. Look there, or check the `env: files:` line near the start of the log, which says where they really are. Edit the ini in VirtualStore, or use the settings panel (Insert). A default GOG Galaxy install runs the game as administrator and is not affected. Don't switch between running as administrator and not: each way has its own files. Details: [Known issues](KNOWN_ISSUES.md#installation). A later version will keep Yongle's files in your user folder instead.

## How do I skip the intro movie?

The game itself can do this. In `Emperor.ini` in the game folder, set `PlayIntroMovie=no` in the `[Config]` section. Yongle does not change this file.

## How do I switch the renderer?

Yongle uses Direct3D 11 by default (`renderer = auto`; Direct3D 9 only if Direct3D 11 cannot start). If you see stutters or a picture that looks wrong, try Direct3D 9: set `renderer = direct3d` in the `[display]` section of `emperor_engine.ini` and restart the game. `renderer = direct3d11` forces Direct3D 11. If Direct3D 9 helps, please report it with the log (see [Reporting problems](../README.md#reporting-problems)).
