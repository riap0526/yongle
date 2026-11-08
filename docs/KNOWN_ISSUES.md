# Known issues

Back to the [README](../README.md). If you run into something not listed here, please see [Reporting problems](../README.md#reporting-problems).

## Performance

- **Laptops and slower computers: lower the resolution first.** Raise the display scale (Options → Video), or choose *Auto* (1080 pixels high on taller screens). The game then draws fewer pixels; this helps more than any other setting.
- **City zoom above 100 % is slower**: the game draws more of the city (at 150 %, more than twice as much). If the game slows down, zoom in a little.

## Troubleshooting

- **Stutters or a picture that looks wrong:** try Direct3D 9: set `renderer = direct3d` in the `[video]` section of `emperor_engine.ini` and restart the game. Please also report it, with the log.
- **Crashes or sound problems after updating to 0.1.3:** Yongle now runs its own work (putting the picture on the screen, saving screenshots) and the graphics driver's on the other processor cores, while the game itself stays on the one core the GOG release gives it. To rule this out, set `cpu_affinity = original` in the `[compat]` section of `emperor_engine.ini` and restart the game. Please also report it, with the log.

## Display

- The map and campaign editors and the multiplayer screens run at 1024×768 (centered), not at your resolution.
- City zoom is not available at the game's own 1024×768 or with its small tiles: the mouse wheel then changes the building category, as in the original.
- The campaign map screen has not been tested in play yet.
- Small cosmetic quirks of the original are kept for now (e.g. text sitting slightly high in some dialog buttons).

## Installation

- **Antivirus or SmartScreen warnings:** `ddraw.dll` is not code-signed. You can check the zip against the SHA256 given on the release page.
- **Windows Security's Controlled folder access:** when it protects Documents and has not allowed `Emperor.exe`, Yongle cannot write to `Documents\My Games\Yongle` (Windows may show a "blocked" notification). Yongle then keeps its files in `%LOCALAPPDATA%\Yongle` instead; the log's `files:` lines say so. To use Documents, allow `Emperor.exe` in Controlled folder access (*Allow an app through Controlled folder access*). See also [Where are Yongle's files?](FAQ.md#where-are-yongles-files)
- **Documents synced with OneDrive:** `Documents\My Games\Yongle` is synced too, log and screenshots included. Nothing is done about this yet; tell us if it is a problem.

## Tested so far

- Two machines: AMD graphics (2560×1440, 200 Hz) and a laptop with Intel integrated graphics (2560×1600, 120 Hz), Windows 11; Windows display scaling and desktop resolution changes while the game runs.
- **Not tested yet:** NVIDIA graphics, real 4K monitors, and moving the window between several monitors. Reports are welcome.

## The original game

- In heavily modified saves with many monuments, opening the Monuments panel can crash the game (this happens without Yongle too).
