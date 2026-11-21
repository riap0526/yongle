// The options window's rows made from our settings (core/settings): a row per setting, its name and descriptions from
// core/options/option_text, its value read and written through the registry. The game layer lists which settings go
// on which page (its BuildPages, with Add / Toggle / ShownIf / Header / Resolve); nothing here knows a game.
//
// A row's value lives in a context kept here until the next Clear (the window reads and writes through it while it
// is open), so Clear only when the pages are built again.
//
// Threads: the game thread (the window takes input there, where core::settings::RunPending runs).
#pragma once

#include <vector>

#include "core/options/window.h"

namespace core::options::setting_rows {

void Clear(); // before the pages are built again (the rows' contexts go)

// A dry run (the game layer builds its pages once at attach): Add and Toggle only collect the settings they would
// list, for the debug panel (core::ui::SetListedElsewhere); nothing is read and no row is made.
void SetDry(bool dry);
bool Dry();
const std::vector<int> &Listed(); // the settings' indices collected since the last SetDry(true)

// A row for section.key, appended to rows; nothing when the setting does not exist. Bool: a checkbox; Int: a slider
// (an auto word below its min); Choice: the values offered (and the current one even if not); Resolution: scaled, the
// game's own size, the common sizes for the screen and a size typed into the file; Hotkey: a Key row (two keys).
void Add(std::vector<Row> &rows, const char *section, const char *key);
// The last row (a slider) gets the on / off setting section.key as its checkbox: a check-slider row (a volume and its
// switch).
void Toggle(std::vector<Row> &rows, const char *section, const char *key);
// The last row shown only while the row `key` (same page) has `value` (its ini spelling): for items whose condition
// the registry does not hold.
void ShownIf(std::vector<Row> &rows, const char *key, const char *value);
void Header(std::vector<Row> &rows, const char *title);
// After a page is built: each shownIf's value as the other row's choice index.
void Resolve(Page &page);

} // namespace core::options::setting_rows
