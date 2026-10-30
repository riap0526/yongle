# Your own menu background

Back to the [README](../README.md).

Yongle shows the game's menu pictures (the main menu and its sub-menus, briefings, high scores, victory and defeat) at their own size in a frame. Around them, by default, is a blurred and darkened picture of **the last city you played**: Yongle saves it as `menu_background_auto.png` in the game folder whenever you leave a city (back to the menu, or File → Exit game). On the very first start, before any city has been seen, the game's parchment is shown instead.

## Using a picture of your own

1. Put the picture in the game folder (next to `Emperor.exe`) and name it **`menu_background.png`** — or **`menu_background.bmp`**.
2. Start the game. The picture is used instead of your last city.

- **Any size works.** The picture is scaled to cover the whole screen, keeping its proportions (cropped at the edges, never stretched), then blurred and darkened exactly like the city picture, so the menu stays readable.
- If both files exist, the `.png` is used.
- To go back to your last city, delete (or rename) the file.

## Settings

The menu frame is the setting `[display] menu_screens` in `emperor_engine.ini` (also in the settings panel, Insert):

| Value | Around the menus |
|---|---|
| `native_city` (default) | own size; your picture or your last city, blurred and darkened |
| `native` | own size, on the game's parchment |
| `framed` | as large as the screen allows, on parchment |
| `black` | the whole 1024×768 picture scaled to fit, black bars at the sides |

Please only use pictures you have the right to use.
