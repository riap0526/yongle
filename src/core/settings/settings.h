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
    Hotkey,     // core/ui/hotkey.h syntax
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
};

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
