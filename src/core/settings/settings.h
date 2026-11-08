// The settings model (docs/resolution_ui/settings.md §5.1): a data-driven table of items, separate from any UI.
//
// Each item declares where it lives in emperor_engine.ini, its type and range, its default, a name and help text
// (UTF-8) and how a change takes effect. The model loads the file, checks every value, keeps the current values,
// applies changes through callbacks the owners register, and writes changed values back to the file without
// touching anything else in it (core/settings/ini_text.h; written to a temporary file first, then swapped in).
//
// Values are kept as their canonical ini text ("on", "125", "auto", "1920x1080", "Insert"); typed views such as
// core_config (core/config.h) are updated by the owners' callbacks.
//
// Threads: loading happens once at start-up (DllMain: kernel32 only). Afterwards changes are only requested
// (Request, from any thread, e.g. the panel on the output thread) and carried out on the game thread (RunPending,
// woken through SetWakeup), where the owners' callbacks may touch the game. Reading values (Get) is safe from any
// thread. Nothing here knows a game.
#pragma once

#include <cstddef>

namespace core::settings {

enum class Type {
    Bool,       // on | off
    Int,        // min..max in steps of step, or autoWord (stored as that word)
    Choice,     // one of choices[]
    Resolution, // WxH (each side 320..16384) | scaled
    Hotkey,     // core/ui/hotkey.h syntax: one key, or two separated by a comma (the second the alternative); empty: none
};

enum class Effect {
    Live,     // applied at once
    Relayout, // applied at once, but the screen is laid out again: the panel asks to keep it (reverts after 15 s)
    Restart,  // saved; takes effect the next time the game starts
};

struct Item {
    const char *section;
    const char *key;
    const char *label; // UTF-8
    const char *help;  // UTF-8
    Type type;
    const char *defaultValue; // canonical text
    Effect effect;
    bool inPanel;  // shown in the panel
    bool editable; // can be changed from the panel (false: shown read-only)
    int min, max, step;           // Int
    const char *autoWord;         // Int: an extra word meaning "automatic" (nullptr: none)
    const char *const *choices;   // Choice: the ini spellings
    const char *const *choiceLabels; // Choice: shown in the panel (nullptr: the ini spellings)
    int choiceCount;
    const char *unit = nullptr; // Int: shown after the number in the panel (e.g. "%")
    int (*maxNow)() = nullptr;  // Int: the largest value that has an effect now (the panel's slider ends there; the
                                // file still takes up to max), or nullptr: max
    const char *shownIfKey = nullptr;   // the panel shows the item only while this item of the same section ...
    const char *shownIfValue = nullptr; // ... has this value (e.g. a scale only for "scaled")
    unsigned unlisted = 0; // Choice: bit k set = choices[k] is valid in the file but not offered in the panel or the
                           // options window (e.g. present = original, for troubleshooting only)
};

// Choice: whether choices[k] is offered in the panel and the options window.
inline bool ChoiceListed(const Item &item, int k)
{
    return k < 0 || k >= 32 || (item.unlisted & (1u << k)) == 0;
}

constexpr size_t kValueSize = 40;

// Start-up, before Load. Returns the item's index.
int Register(const Item &item);
int Find(const char *section, const char *key); // -1 when unknown
int Count();
const Item &Get(int index);

// A setting that no longer exists (start-up, before Load). Load removes its lines from the file, each with the
// description directly above it (core/settings/ini_text.h RemoveIniKeys), after keeping the file as it was in
// emperor_engine.ini.bak; migrate (optional) carries its value over to current settings that the file does not set
// itself: it fills out[] (at most max) and returns how many. Other unknown lines are only logged: they may be typing
// mistakes or settings of a newer version, which are not ours to delete.
struct Migration {
    const char *section;
    const char *key;
    const char *value;
};
using Migrate = int (*)(const char *oldValue, Migration *out, int max);
void Retire(const char *section, const char *key, Migrate migrate);

// A value of a Choice item that was renamed (start-up, before Load): Load takes the old spelling as the new one and
// writes the new one into the file in its place (after keeping the file as it was in emperor_engine.ini.bak), so the
// file, the panel and the log all use the current name.
void RenameValue(const char *section, const char *key, const char *oldValue, const char *newValue);

// A setting that moved to another section or key (start-up, before Load). Load takes the old line's value for the
// new setting (with the new setting's renamed values, see RenameValue) unless the file also sets the new one, and
// rewrites the file with the new line in place of the old one (after keeping the file as it was in
// emperor_engine.ini.bak). MoveSection moves every line of a section whose key is a setting of the new section;
// MoveKey is checked first, Retire before both.
void MoveKey(const char *oldSection, const char *oldKey, const char *newSection, const char *newKey);
void MoveSection(const char *oldSection, const char *newSection);
// A value changed only when its line is moved (an upgrade from a file of an older version), not when the file
// sets the new setting to it itself: e.g. present = original from [display] becomes display, while [video]
// present = original written later stays (for troubleshooting).
void RenameMovedValue(const char *newSection, const char *newKey, const char *oldValue, const char *newValue);

// The text of the settings file this version ships (start-up, before Load; the game layer embeds it). Load then
// rewrites an existing file whose text differs from it filled with the file's values (core/settings/ini_text.h
// FillTemplate): descriptions of this version, settings it lacks at their defaults, the values as they were, lines
// it does not know kept as written; the file as it was is kept in emperor_engine.ini.bak. A missing file is made
// again from the template, with the values of emperor_engine.ini.bak when there is one (the .bak is left as it is),
// else at the defaults. Without a template (e.g. offline tests) the descriptions in the file are never changed and a
// missing file is not made.
void SetTemplate(const char *text);

// Called when the item's value changed (game thread; also once per item after Load, with initial = true). Returns
// false when the change could not take effect now (the panel then says a restart is needed). Up to 4 callbacks per
// item (e.g. core_config's typed view and the owner), called in the order they were added.
using Apply = bool (*)(int index, const char *value, bool initial);
void OnChange(int index, Apply apply);

// The canonical form of text for the item, or false when it is not a valid value.
bool Normalize(int index, const char *text, char *out, size_t size);

// Loads the file (kernel32 only) and logs every unknown or invalid line; missing items keep their defaults. The
// path is kept for saving. Then calls every callback with initial = true.
void Load(const wchar_t *path);
const wchar_t *Path();

// The current value (canonical text), from any thread.
void Value(int index, char *out, size_t size);
// The value at start-up (for Restart items: what is in effect until the game restarts).
void StartupValue(int index, char *out, size_t size);
// True when a change of this item waits for a restart (Restart items changed since start-up, or a Live item whose
// callback could not apply it now).
bool RestartPending(int index);

// Any thread: asks for a new value. save = the value is confirmed and the file is written afterwards; without it
// the value takes effect but the file keeps the last confirmed one (a re-layout waiting for "keep"), also when
// another item is saved meanwhile. Invalid values are logged and dropped.
void Request(int index, const char *value, bool save);
// Any thread: confirms every current value and writes the file (the panel's "keep these settings").
void RequestSave();

// Game thread: carries out the requests. Called through the wakeup.
void RunPending();
// How the game thread is woken for RunPending (core/display posts a message to the game window).
void SetWakeup(void (*wake)());

// The result of the last save, for the panel ("" before the first one).
void LastSaveResult(char *out, size_t size);

// One line for the log: "display.backend=emulated display.scaling=sharp_bilinear ..."
void Describe(char *out, size_t size);

} // namespace core::settings
