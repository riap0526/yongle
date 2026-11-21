// The texts for players of our pause menu, the options window's frame (Hotkeys, "keep this setting?"), the menu
// items both games add or rename and the panel's notes on our own keys (shared, so the two games say the same).
// Moved from Yongle's games/emperor/options_text for Pericles.
#pragma once

namespace core::options::menu_text {

// The File menu's renamed items (also the titles of their confirmations) and the pause menu.
extern const char *const kMenuReplayMap, *const kMenuExitToMainMenu;
extern const char *const kPauseTitle, *const kPauseContinue, *const kPauseOptions;
// The text of the confirmation before Replay map / Exit to main menu, for a game whose texts have none (Zeus; Emperor
// uses its own text 5, 129, which says the same).
extern const char *const kLeaveGame;

// The red first line of the tooltips of a setting marked "*".
extern const char *const kRestartTip;

// The game's Options menu and the main menu's button that open the window; the File menu's screenshot items.
extern const char *const kMenuGeneral, *const kMenuVideo, *const kMenuAudio, *const kMainMenuOptions, *const kMenuHotkeys;
extern const char *const kMenuCityScreenshot, *const kMenuMinimapScreenshot;
extern const char *const kCategoryMessages; // the options window's General > Messages (the message list opens it)
// The options window's General > Difficulty (the Options menus' Difficulty opens it); the Options menu's Difficulty
// and Auto defend ON / OFF for a game whose texts have none (Emperor; Zeus uses its own); the notice when auto defend
// cannot be changed now.
extern const char *const kCategoryDifficulty, *const kMenuDifficulty, *const kMenuAutoDefendOn,
    *const kMenuAutoDefendOff, *const kAutoDefendLocked;

// The Hotkeys window and "keep this setting?" (core/options Window::Texts; %s / %d as there).
extern const char *const kHotkeysTitle, *const kColumnAction, *const kColumnKey, *const kColumnAlternative,
    *const kPressKey, *const kKeyNotChanged, *const kKeyNotUsable, *const kKeyInUse, *const kKeyReserved,
    *const kKeyClearHint, *const kKeyTip, *const kKeepTitle, *const kKeepCountdown, *const kKeep, *const kRevert;
extern const char *const kOverlayName; // printf format: an overlay's name in the Hotkeys window
// The Hotkeys window's section titles (core::input::keys::Section; "" for another number).
const char *KeySection(int section);

// The debug panel's note when one of our key settings is set to a key that stays ours ({product}: the game's product,
// filled in by core/options/hotkeys_page).
extern const char *const kKeyNoteWindow, *const kKeyNoteResolution;

} // namespace core::options::menu_text
