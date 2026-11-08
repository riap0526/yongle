// The options window's texts for players (docs/resolution_ui/original_settings.md §14.3): for each setting a short
// description (the tooltip of its name) and, for choices, one description per choice (the tooltip of the value).
// The registry's own help (core/settings) stays as it is for the panel and the settings file; these are shorter,
// because each choice is explained only when it is the one shown. American English; checked for internal notes by
// tools/verify/ini_template_check.py.
#pragma once

namespace emperor::options_text {

struct ChoiceText {
    const char *spell; // the ini spelling; "*": any other value (e.g. a resolution's sizes)
    const char *help;
};

struct Text {
    const char *section;
    const char *key;
    const char *summary;         // the name's tooltip (nullptr: the registry's help)
    const ChoiceText *choices;   // nullptr: none (the values explain themselves)
    int choiceCount;
    bool noRestartMark = false;  // a restart item that needs no mark (it only ever acts at start-up)
    const char *label = nullptr; // the row's name (nullptr: the registry's); section "game": the game's own options
};

const Text *Find(const char *section, const char *key);
const char *ChoiceHelp(const Text &t, const char *spell); // "" when none

// The red first line of the tooltips of a setting marked "*".
extern const char *const kRestartTip;

// The game's Options menu (in the city) and the main menu's button that open the window (options_entry).
extern const char *const kMenuGeneral, *const kMenuVideo, *const kMenuAudio, *const kMainMenuOptions,
    *const kMenuHotkeys;
extern const char *const kCategoryMessages; // the options window's General > Messages (the message list opens it)

// The Hotkeys window and "keep this setting?" (core/options Window::Texts; %s / %d as there).
extern const char *const kHotkeysTitle, *const kColumnAction, *const kColumnKey, *const kPressKey,
    *const kKeyNotChanged, *const kKeyNotUsable, *const kKeyInUse, *const kKeepTitle, *const kKeepCountdown,
    *const kKeep, *const kRevert;

} // namespace emperor::options_text
