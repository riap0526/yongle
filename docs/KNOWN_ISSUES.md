# Known issues

Back to the [README](../README.md). If you run into something not listed here, please see [Reporting problems](../README.md#reporting-problems).

## Performance

- **Laptops and slower computers: lower the resolution first.** Raise the display scale (Options → Display options, or the settings panel with Insert), or choose *Auto* (1080 pixels high on taller screens). The game then draws fewer pixels; this helps more than any other setting.
- **Steam's in-game FPS counter stutters with Direct3D 11.** With the counter on, the game stops for about 2.5 seconds at a time; the Steam overlay itself, with the counter off, runs smoothly. Because the counter cannot be detected, Yongle plays it safe: when the Steam overlay is in the game (for example when you start Emperor through Steam as a non-Steam game), it uses Direct3D 9, which runs at about 25 % fewer frames per second. **If you do not use Steam's FPS counter**, set `renderer = direct3d11` in the `[display]` section of `emperor_engine.ini` and restart the game to get that performance back.
- **Stutters with Direct3D 11 on some systems.** If the game stutters badly although the log shows nothing unusual, try Direct3D 9: set `renderer = direct3d` in the `[display]` section of `emperor_engine.ini` and restart the game. Please also report it, with the log.
- **The frame rate drops for a second or two while a screenshot is saved** (F11, F12). The GOG release runs the whole game on one processor core (a Windows compatibility setting it installs), so saving the picture takes time from the game. The game does not freeze.

## Display

- **Flashing black blocks** have been seen occasionally after playing for a while on one system (AMD graphics), so far only with Direct3D 11. Screenshots do not show them, and switching windows (Alt+Tab) clears them. The cause is not known yet. If you see them, press F12, then send the log and the `shots\` folder; trying `renderer = direct3d` (see above) and telling us whether they still appear helps too.
- The map and campaign editors and the multiplayer screens run at 1024×768 (centered), not at your resolution.
- The campaign map screen has not been tested in play yet.
- Small cosmetic quirks of the original are kept for now (e.g. text sitting slightly high in some dialog buttons).

## Installation

- **Antivirus or SmartScreen warnings:** `ddraw.dll` is not code-signed. You can check the zip against the SHA256 given on the release page.

## Tested so far

- Two machines: AMD graphics (2560×1440, 200 Hz) and a laptop with Intel integrated graphics (2560×1600, 120 Hz), Windows 11; Windows display scaling and desktop resolution changes while the game runs.
- **Not tested yet:** NVIDIA graphics, real 4K monitors, and moving the window between several monitors. Reports are welcome.

## The original game

- In heavily modified saves with many monuments, opening the Monuments panel can crash the game (this happens without Yongle too).
