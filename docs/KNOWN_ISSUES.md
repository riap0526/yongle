# Known issues

Back to the [README](../README.md). If you run into something not listed here, please see [Reporting problems](../README.md#reporting-problems).

## Performance

- **Laptops and slower computers: lower the resolution first.** Raise the display scale (Options → Display options, or the settings panel with Insert), or choose *Auto* (1080 pixels high on taller screens). The game then draws fewer pixels; this helps more than any other setting.
- **Stutters with Direct3D 11 on some systems.** If the game stutters badly although the log shows nothing unusual, try Direct3D 9: set `renderer = direct3d` in the `[display]` section of `emperor_engine.ini` and restart the game. Please also report it, with the log.
- **Troubleshooting crashes or sound problems after updating to 0.1.3**: Yongle now runs its own work (putting the picture on the screen, saving screenshots) and the graphics driver's on the other processor cores, while the game itself stays on the one core the GOG release gives it. To rule this out, set `cpu_affinity = original` in the `[compat]` section of `emperor_engine.ini` and restart the game. Please also report it, with the log.

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
