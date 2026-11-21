// Texts for players (a text check scans every string literal here for both games).
#include "core/options/menu_text.h"

namespace core::options::menu_text {

const char *const kMenuReplayMap = "Replay map";             // Emperor's "Replay mission", Zeus's "Replay episode"
const char *const kMenuExitToMainMenu = "Exit to main menu"; // Emperor's "New game", Zeus's "New adventure"
const char *const kPauseTitle = "Pause";
const char *const kPauseContinue = "Continue";
const char *const kPauseOptions = "Options";
const char *const kLeaveGame = "Do you wish to leave the current game?";

const char *const kRestartTip = "Takes effect after the game is restarted.";
const char *const kMenuGeneral = "General Settings";
const char *const kMenuVideo = "Video";
const char *const kMenuAudio = "Audio";
const char *const kMainMenuOptions = "Options";
const char *const kMenuHotkeys = "Hotkeys";
const char *const kMenuCityScreenshot = "Save city screenshot";       // 136 in Emperor's menu font
const char *const kMenuMinimapScreenshot = "Save minimap screenshot"; // 165
const char *const kCategoryMessages = "Messages";
const char *const kCategoryDifficulty = "Difficulty";
const char *const kMenuDifficulty = "Difficulty";
const char *const kMenuAutoDefendOn = "Auto Defend - ON"; // as the game's "Autosave - ON" and Zeus's own
const char *const kMenuAutoDefendOff = "Auto Defend - OFF";
// Short enough for the games' widest notice box (Zeus's: 460 pixels).
const char *const kAutoDefendLocked = "Auto defend can't be changed during an attack.";

const char *const kHotkeysTitle = "Hotkeys";
const char *const kColumnAction = "Action";
const char *const kColumnKey = "Hotkey";
const char *const kColumnAlternative = "Alternative";
const char *const kPressKey = "Press a key";
const char *const kKeyNotChanged = "Not changed.";
const char *const kKeyNotUsable = "That key cannot be used here; try another one.";
const char *const kKeyInUse = "%s is already the key for \"%s\"; try another one.";
const char *const kKeyReserved = "The game keeps this key for itself; choose another one.";
const char *const kKeyClearHint = "Backspace: no key.";
const char *const kKeyTip = "Click to change; right-click to remove.";
const char *const kKeepTitle = "Keep this setting?";
const char *const kKeepCountdown = "The previous setting comes back in %d s.";
const char *const kKeep = "Keep";
const char *const kRevert = "Revert";
const char *const kOverlayName = "%s overlay";

namespace {
const char *const kKeySections[] = {"Arrow keys", "Global hotkeys", "City hotkeys", "Building menus",
                                    "Overlays",   "Bookmarks",      "Army groups"};
} // namespace

const char *KeySection(int section)
{
    return section >= 0 && section < static_cast<int>(sizeof(kKeySections) / sizeof(kKeySections[0]))
               ? kKeySections[section]
               : "";
}

const char *const kKeyNoteWindow =
    "{product} also uses this key to switch between window and full screen; that no longer works with it.";
const char *const kKeyNoteResolution = "{product} also uses this key to change the resolution; that no longer works with it.";

} // namespace core::options::menu_text
