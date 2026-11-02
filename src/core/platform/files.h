// Where our own files go: the log, the settings file, screenshots and the menu backdrop (docs/testing/v0_2_0_files.md).
//
// A 32-bit program without a manifest that writes to a folder a standard user cannot write to (C:\Program Files (x86))
// is redirected by Windows to %LOCALAPPDATA%\VirtualStore -- but only when it does not run as administrator, so the
// two ways of running had separate files. The rule, the same whether or not the process is elevated:
//   - a standard user can create files in the game folder (checked against the folder's security, with the
//     non-elevated token when elevated): the game folder, as before;
//   - otherwise <Documents>\My Games\<app>\ (Documents wherever the user moved it), or %LOCALAPPDATA%\<app>\ when
//     Documents is not available or not writable (Controlled folder access), or the game folder when neither works.
// The app name comes from the entry point; nothing here knows a game. The game's own files (art, data) are read from
// the game folder as before.
#pragma once

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

enum core_files_where {
    CORE_FILES_GAME_FOLDER = 0, /* next to the DLL (also before core_files_decide) */
    CORE_FILES_DOCUMENTS = 1,   /* <Documents>\My Games\<app>\ */
    CORE_FILES_LOCAL_APPDATA = 2 /* %LOCALAPPDATA%\<app>\ */
};

/* Once, not under the loader lock (shell32, AccessCheck): decides the folder and creates it. safe_only: under the
   loader lock -- only the game folder, nothing of shell32. Nothing is logged here (the log is not open yet); see
   core_files_log. */
void core_files_decide(const wchar_t *app, int safe_only);

/* The folder with a trailing backslash. Before core_files_decide: the folder of the module holding this code. */
const wchar_t *core_files_folder(void);
const char *core_files_folder_utf8(void);
enum core_files_where core_files_where_now(void);

/* The folder of the module holding this code (the DLL: the game folder), with a trailing backslash. */
const wchar_t *core_files_module_folder(void);

/* From the settings (core/config), before its file is read: when our folder is not the game folder and has no file
   called name yet, the newest one of the game folder's own copy, its VirtualStore copy and the other candidate folder
   is copied over; the others stay where they are. Logged. */
void core_files_bring_over(const wchar_t *name);

/* After the log is open: the decision, its reasons and what was copied, one line each. */
void core_files_log(void);

/* Opens the folder in Explorer, on a thread of its own (any thread; logged). */
void core_files_open_folder(void);

#ifdef __cplusplus
}

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace core::platform::files {

// What Decide asks the system; the offline test passes fakes.
struct Probe {
    // A standard user can create files and folders in folder. false with *why set when it cannot be told.
    bool (*userCanWrite)(const std::wstring &folder, std::string *why);
    // FOLDERID_Documents (which = 0) or FOLDERID_LocalAppData (which = 1), without a trailing backslash.
    bool (*knownFolder)(int which, std::wstring *path, std::string *why);
    // Creates folder (and its parents below the known folder) and writes and deletes a test file in it.
    bool (*usable)(const std::wstring &folder, std::string *why);
};

struct Decision {
    std::wstring folder; // with a trailing backslash
    core_files_where where = CORE_FILES_GAME_FOLDER;
    std::vector<std::string> notes; // reasons, in order, for the log
};

// gameFolder with a trailing backslash.
Decision Decide(const std::wstring &gameFolder, const wchar_t *app, const Probe &probe);

// The real probes.
Probe SystemProbe();

// One settings file that could be copied over.
struct Candidate {
    std::wstring path;
    std::string label; // "game folder", "VirtualStore", ...
    bool exists = false;
    FILETIME written{};
};

// The newest existing candidate, or -1.
int Newest(const std::vector<Candidate> &candidates);

} // namespace core::platform::files
#endif
