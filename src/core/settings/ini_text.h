// emperor_engine.ini as text: reading "key = value" lines and changing values in place.
//
// The file format (since M3): UTF-8, optional BOM; "[section]" lines; "key = value" lines; ';' or '#' starts a
// comment anywhere on a line; section and key names are case-insensitive; a later line for the same key wins.
// Changing values keeps everything else exactly as the user wrote it: comments (also one after a value), blank
// lines, unknown sections and keys, the spelling of the key, the spacing around '=', the line ends (CRLF or LF) and
// the BOM. A key that is not in the file yet is added at the end of its section (the section is added at the end of
// the file when missing).
#pragma once

#include <string>
#include <vector>

namespace core::settings {

// One "key = value" line as read.
struct IniLine {
    std::string section; // as written, without brackets
    std::string key;     // as written
    std::string value;   // trimmed, without the comment
    int line;            // 1-based
};

// All key lines and the lines that are neither a setting, a section, a comment nor blank (as line numbers).
void ReadIni(const std::string &text, std::vector<IniLine> *keys, std::vector<int> *invalid);

struct IniChange {
    std::string section;
    std::string key;
    std::string value;
};

// Returns text with every line of each change's key (in its section) set to the new value, or the key added.
std::string UpdateIni(const std::string &text, const std::vector<IniChange> &changes);

struct IniKey {
    std::string section;
    std::string key;
};

// Returns text without every line of the given keys (in their sections), each with the comment lines directly above
// it (up to a blank line, a section or another key: the description our file template puts there) and the blank
// line after it when that would follow another blank line, a section header or the start of the file. Everything else stays exactly as it was.
std::string RemoveIniKeys(const std::string &text, const std::vector<IniKey> &keys);

// The file as this version's template would have it: the template's text with the values of text filled in.
//  - Every key of the template takes the value text gives it (same section, names case-insensitive; the last line
//    wins, as when reading), as written (not checked: a wrong value stays visible); a key text does not have keeps
//    the template's value (its default). Comments after a value in text are not kept.
//  - Keys of text that the template does not have, and lines that are not settings, are kept as written at the end
//    of their section, under "; Not described above (kept as written):"; sections the template does not have go to
//    the end of the file, under "; Sections not described above (kept as written):".
//  - text's line ends (CRLF when it has any) and BOM are kept.
// Filling the result again gives the same text, so a file is rewritten only once per template.
struct FilledIni {
    std::string text;
    int kept = 0;  // template keys whose value came from text
    int added = 0; // template keys text did not have (at their defaults)
    int extra = 0; // lines of text kept as written
};
FilledIni FillTemplate(const std::string &tmpl, const std::string &text);

} // namespace core::settings
