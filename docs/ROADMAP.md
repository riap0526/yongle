# Roadmap

Back to the [README](../README.md). Plans change with what players report; ideas are welcome as a feature request.

## Done

- **v0.1.0**: the game in a borderless full-screen window or a resizable window at any resolution, laid out for the screen; display scale and *Auto*; frames at the monitor's refresh rate; full-screen pictures and the China map at their own size; pixel-smooth scrolling; steady game speed; the in-game Display options; the settings panel; screenshots and problem reports.
- **v0.1.1**: full screen covers the whole screen on GOG installs.
- **v0.1.2**: Direct3D 9 when the Steam overlay is in the game, screenshots without freezes, no more jumping city when the computer cannot keep up, more diagnostics in the log.
- **v0.1.3**: smoother frame pacing and a higher frame rate on both renderers (Yongle's own work and the graphics driver run on other processor cores, while the game keeps the single core its compatibility settings give it); `renderer = auto` back to Direct3D 11 for everyone, since Steam's FPS counter no longer makes it stop for seconds at a time; screenshots no longer slow the game down; display options for the loading screens, the menus and the China map's backdrop; output scaling (sharp, nearest, smooth) switched at once from Display options; music and sounds pause while you are switched away; ambient sounds at a steady rate whatever the resolution; clicks on the sidebar or the top bar no longer reach the city.

## In progress: v0.2.0

In this order; v0.2.0 is released when all five are done.

1. **Yongle's files where you can always write them**: if the game folder cannot be written to without administrator rights (for example under Program Files), settings, log and screenshots go to `Documents\My Games\Yongle` instead of Windows' VirtualStore, and stay in one place whether or not the game runs as administrator. Games installed elsewhere keep them in the game folder, as now.
2. **Intro movie setting**: play or skip the intro movie from Yongle's settings, without changing the game's own files.
3. **City zoom** (mouse wheel, centered on the cursor).
4. **Export of the whole city** as one large image.
5. **Original-style settings screens**: the settings drawn with the game's own art, grouped into the game's option dialogs.

## Planned

- **Key rebinding** in a game-style dialog.
- **The unused ambient sounds**: the game ships 24 ambient sounds (birds, wind, insects, chimes) in `Audio\Ambient\Layer1` that it never plays; letting them play.

## Longer term

- **Localization**: UTF-8 translation files and a text engine with open-source fonts, starting with Traditional Chinese. The format will be open and documented, so the community can add **language packs** for other languages without changing the game's files.
- Raising the original's limits and fixing its bugs; better support for custom scenarios.
- Smoother movement of people and animations between simulation steps.
- **Zeus & Poseidon** support with the same engine.
- Eventually, a **reimplementation** that no longer needs the original executable.
