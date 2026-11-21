// The options window's texts for players: Yongle's own here (its game's files and folders, the empire map);
// the ones both games share are in core/options/option_text (the settings' descriptions and the game's own options)
// and core/options/menu_text (the menus, the Hotkeys window, "keep this setting?"), named here as before.
#pragma once

#include "core/options/menu_text.h"
#include "core/options/option_text.h"

namespace emperor::options_text {

using core::options::option_text::Choice;
using core::options::option_text::ChoiceText;
using core::options::option_text::Find; // Yongle's own table first (Register), then the shared one
using core::options::option_text::Text;

// Gives the core Yongle's own table (before the options window is built).
void Register();

extern const char *const kOptionsTitle;

// The debug panel's note for a key the game itself uses, chosen for one of our key settings (core::ui::SetGameKeyNote;
// registered by games/emperor/keys); nullptr when the game does not use it.
const char *GameKeyNote(unsigned vk, unsigned mods);

using core::options::menu_text::kCategoryDifficulty;
using core::options::menu_text::kCategoryMessages;
using core::options::menu_text::kMenuAutoDefendOff;
using core::options::menu_text::kMenuAutoDefendOn;
using core::options::menu_text::kMenuDifficulty;
using core::options::menu_text::kColumnAction;
using core::options::menu_text::kColumnAlternative;
using core::options::menu_text::kColumnKey;
using core::options::menu_text::kHotkeysTitle;
using core::options::menu_text::kKeep;
using core::options::menu_text::kKeepCountdown;
using core::options::menu_text::kKeepTitle;
using core::options::menu_text::kKeyClearHint;
using core::options::menu_text::kKeyInUse;
using core::options::menu_text::kKeyNotChanged;
using core::options::menu_text::kKeyNotUsable;
using core::options::menu_text::kKeyReserved;
using core::options::menu_text::kKeyTip;
using core::options::menu_text::kMainMenuOptions;
using core::options::menu_text::kMenuAudio;
using core::options::menu_text::kMenuCityScreenshot;
using core::options::menu_text::kMenuExitToMainMenu;
using core::options::menu_text::kMenuGeneral;
using core::options::menu_text::kMenuHotkeys;
using core::options::menu_text::kMenuMinimapScreenshot;
using core::options::menu_text::kMenuReplayMap;
using core::options::menu_text::kMenuVideo;
using core::options::menu_text::KeySection; // the Hotkeys window's section titles
using core::options::menu_text::kOverlayName;
using core::options::menu_text::kPauseContinue;
using core::options::menu_text::kPauseOptions;
using core::options::menu_text::kPauseTitle;
using core::options::menu_text::kPressKey;
using core::options::menu_text::kRestartTip;
using core::options::menu_text::kRevert;

} // namespace emperor::options_text
