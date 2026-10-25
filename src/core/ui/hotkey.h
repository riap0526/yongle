// Hotkeys written as text in emperor_engine.ini, e.g. "Insert", "Ctrl+F12", "Shift+Alt+P".
//
// Syntax: zero or more modifiers (Ctrl or Control, Shift, Alt), each followed by '+', then one key; case does not
// matter, spaces around '+' are allowed. Keys: F1..F24, Insert (Ins), Delete (Del), Home, End, PageUp (PgUp),
// PageDown (PgDn), Pause, ScrollLock, Tab, Space, Backquote (`), A..Z, 0..9, Numpad0..Numpad9, NumpadAdd,
// NumpadSubtract, NumpadMultiply, NumpadDivide, NumpadDecimal. Escape is not allowed (it always closes the panel).
// A hotkey matches a key press only with exactly its modifiers (Insert does not match Ctrl+Insert).
// Nothing here knows a game; which keys a game itself uses is documented per game.
#pragma once

#include <cstddef>

namespace core::ui {

enum : unsigned { kModCtrl = 1, kModShift = 2, kModAlt = 4 };

struct Hotkey {
    unsigned vk;   // Windows virtual-key code
    unsigned mods; // kMod* bits
};

// Parses [b, e); false (out unchanged) when the text is not a valid hotkey.
bool ParseHotkey(const char *b, const char *e, Hotkey *out);

// The hotkey in the canonical spelling ("Ctrl+F12", "Insert") for the log and the panel.
void FormatHotkey(const Hotkey &key, char *out, size_t size);

} // namespace core::ui
