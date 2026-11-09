# Roadmap

Back to the [README](../README.md). Plans change with what players report; ideas are welcome as a feature request.

## Done

- **v0.1.0**: the game in a borderless full-screen window or a resizable window at any resolution, laid out for the screen; display scale and *Auto*; frames at the monitor's refresh rate; full-screen pictures and the China map at their own size; pixel-smooth scrolling; steady game speed; the in-game Display options; the settings panel; screenshots and problem reports.
- **v0.1.1**: full screen covers the whole screen on GOG installs.
- **v0.1.2**: Direct3D 9 when the Steam overlay is in the game, screenshots without freezes, no more jumping city when the computer cannot keep up, more diagnostics in the log.
- **v0.1.3**: smoother frame pacing and a higher frame rate on both renderers (Yongle's own work and the graphics driver run on other processor cores, while the game keeps the single core its compatibility settings give it); `renderer = auto` back to Direct3D 11 for everyone, since Steam's FPS counter no longer makes it stop for seconds at a time; screenshots no longer slow the game down; display options for the loading screens, the menus and the China map's backdrop; output scaling (sharp, nearest, smooth) switched at once from Display options; music and sounds pause while you are switched away; ambient sounds at a steady rate whatever the resolution; clicks on the sidebar or the top bar no longer reach the city.

- **v0.2.0**: Options in the game's own style (the game's options and Yongle's in one window, F6); key rebinding with two keys per action; Esc closes windows and opens a Pause menu; city zoom with the mouse wheel; full city and minimap screenshots; the display scale for every screen; master volume; the game's unused nature sounds; Yongle's files where it can always write them; skipping the intro movie; full screen no longer pushed up by the compatibility setting of GOG installs.

## Planned

The order is not decided yet.

- **Drag scrolling** with the middle mouse button.
- **Feng shui preview colors** that are easier to tell apart for color-blind players.
- **Load and save windows of Yongle's own**, larger, with a preview of the saved city.
- **Raising the original's limits.**
- **Gameplay quality-of-life options** in the Options window, chosen with players' feedback.
- **Zeus & Poseidon** support with the same engine.

See [CHANGELOG.md](../CHANGELOG.md) for each release in detail.

## Longer term

- **Localization**: UTF-8 translation files and a text engine with open-source fonts, starting with Traditional Chinese. The format will be open and documented, so the community can add **language packs** for other languages without changing the game's files.
- Fixing the original's bugs; better support for custom scenarios.
- Smoother movement of people and animations between simulation steps.
- Eventually, a **reimplementation** that no longer needs the original executable.
