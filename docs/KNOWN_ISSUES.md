# Known issues

Back to the [README](../README.md). If you run into something not listed here, please see [Reporting problems](../README.md#reporting-problems).

## Performance

- **Laptops and slower computers: lower the resolution first.** Raise the display scale (Options → Display options, or the settings panel with Insert), or choose *Auto* (1080 pixels high on taller screens). The game then draws fewer pixels; this helps more than any other setting.

## Troubleshooting

- **Stutters or a picture that looks wrong:** try Direct3D 9: set `renderer = direct3d` in the `[display]` section of `emperor_engine.ini` and restart the game. Please also report it, with the log.
- **Crashes or sound problems after updating to 0.1.3:** Yongle now runs its own work (putting the picture on the screen, saving screenshots) and the graphics driver's on the other processor cores, while the game itself stays on the one core the GOG release gives it. To rule this out, set `cpu_affinity = original` in the `[compat]` section of `emperor_engine.ini` and restart the game. Please also report it, with the log.

## Display

- The map and campaign editors and the multiplayer screens run at 1024×768 (centered), not at your resolution.
- The campaign map screen has not been tested in play yet.
- Small cosmetic quirks of the original are kept for now (e.g. text sitting slightly high in some dialog buttons).

## Installation

- **Antivirus or SmartScreen warnings:** `ddraw.dll` is not code-signed. You can check the zip against the SHA256 given on the release page.
- **Game in `C:\Program Files (x86)` and not run as administrator: Yongle's files end up in VirtualStore.** Windows then redirects what is written to the game folder (`emperor_engine.ini`, `ddraw_proxy.log`, `shots\`, `menu_background_auto.png`) to a copy for your user under `%LOCALAPPDATA%\VirtualStore\Program Files (x86)\` followed by the game folder's own path. If you cannot find the log or the screenshots in the game folder, look there; the `env: files:` line near the start of the log says where they really are.
  - Once that copy of `emperor_engine.ini` exists, editing the one in the game folder has no effect: edit the copy in VirtualStore, or use the settings panel (Insert).
  - GOG Galaxy sets the game to run as administrator, so a default GOG Galaxy install is not affected. The retail release, a copy installed elsewhere under Program Files, or a GOG install whose *Run this program as an administrator* setting was turned off are.
  - Do not switch between running as administrator and not: each way has its own settings, log and screenshots.
  - A later version will keep Yongle's files in your user folder instead.

## Tested so far

- Two machines: AMD graphics (2560×1440, 200 Hz) and a laptop with Intel integrated graphics (2560×1600, 120 Hz), Windows 11; Windows display scaling and desktop resolution changes while the game runs.
- **Not tested yet:** NVIDIA graphics, real 4K monitors, and moving the window between several monitors. Reports are welcome.

## The original game

- In heavily modified saves with many monuments, opening the Monuments panel can crash the game (this happens without Yongle too).
