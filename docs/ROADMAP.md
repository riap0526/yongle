# Roadmap

Back to the [README](../README.md). Plans change with what players report; ideas are welcome as a feature request.

## Done

- **v0.1.0**: the game in a borderless full-screen window or a resizable window at any resolution, laid out for the screen; display scale and *Auto*; frames at the monitor's refresh rate; full-screen pictures and the China map at their own size; pixel-smooth scrolling; steady game speed; the in-game Display options; the settings panel; screenshots and problem reports.
- **v0.1.1**: full screen covers the whole screen on GOG installs.
- **v0.1.2**: Direct3D 9 when the Steam overlay is in the game, screenshots without freezes, no more jumping city when the computer cannot keep up, more diagnostics in the log.

## In progress: performance and feedback

- Smoother frame pacing with both Direct3D 11 and Direct3D 9.
- Display options for the loading screens, the menus and the China map's backdrop.
- Switching the output scaling (sharp, nearest, smooth) at once from Display options.
- Sound: pausing the music when you switch away from the game; the ambient sounds.

## Planned

- **City zoom** (mouse wheel, centered on the cursor), then **export of the whole city as one large image**.
- **Original-style settings screens**: the settings drawn with the game's own art, grouped into the game's option dialogs.
- **Key rebinding** in a game-style dialog.

## Longer term

- **Localization**: UTF-8 translation files and a text engine with open-source fonts, starting with Traditional Chinese. The format will be open and documented, so the community can add **language packs** for other languages without changing the game's files.
- Raising the original's limits and fixing its bugs; better support for custom scenarios.
- Smoother movement of people and animations between simulation steps.
- **Zeus & Poseidon** support with the same engine.
- Eventually, a **reimplementation** that no longer needs the original executable.
