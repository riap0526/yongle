// The options window's texts for players: for each setting a short description (the tooltip of its name) and, for
// choices, one description per choice (the tooltip of the value). The registry's own help (core/settings) stays as it
// is for the debug panel and the settings file; these are shorter, because each choice is explained only when it is
// the one shown. American English; a text check scans the string literals here.
//
// The texts both games share are here, so the two say the same; a game layer gives a table of its own (texts only it
// has, or its own wording where its game differs), searched first.
//
// Threads: read on the game thread; SetGameTexts once at attach.
#pragma once

namespace core::options::option_text {

struct ChoiceText {
    const char *spell; // the ini spelling; "*": any other value (e.g. a resolution's sizes)
    const char *help;  // '\n' starts a new line
    const char *shown = nullptr; // the value's name in the window (nullptr: made from the spelling, "Sharp bilinear")
    bool restart = false;        // this value takes effect after a restart (red first line in its tooltip)
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

// The game layer's own table (searched before the shared one); nullptr / 0: none.
void SetGameTexts(const Text *texts, int count);

const Text *Find(const char *section, const char *key); // the game's, else the shared one, else nullptr
const ChoiceText *Choice(const Text &t, const char *spell); // its own entry, else "*", else nullptr

} // namespace core::options::option_text
