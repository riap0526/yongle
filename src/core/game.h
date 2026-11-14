/* What the core needs to know about the game this DLL is built for: names, the settings file, the game's own size,
   and the game layer's entry points. The core never includes a game layer; the game layer fills a
   core_game_info and the entry point (proxy/ddraw/ddraw.c) hands it to the core with core_game_set before anything
   else, from DllMain. C interface (the entry point is C). */
#pragma once

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct core_game_info {
    const char *name;          /* the game's folder in src/games, e.g. "emperor" */
    const char *product;       /* the public name, e.g. "Yongle": the log, the panel, the options window */
    const char *version;       /* e.g. "0.2.0" (with a test build's suffix) */
    const wchar_t *folder;     /* our files' folder when the game folder is protected: Documents\My Games\<folder> */
    const wchar_t *ini_file;   /* our settings file, e.g. L"emperor_engine.ini" (next to the game's exe) */
    const char *ini_file_utf8; /* the same name for logs and messages */
    int native_width;          /* the game's own (largest) size, e.g. 1024 x 768: the smallest layout and the fixed */
    int native_height;         /* resolution's default */
    /* The game layer's entry points (core/startup.h runs them):
       register_settings: from DllMain, before the settings file is read (kernel32 only);
       attach: at the exe's entry point, after the settings are read: checks the exe and installs the game's hooks;
       editor_exe: nonzero when the process is a separate editor that must run as without the DLL (may be NULL). */
    void (*register_settings)(void);
    void (*attach)(void);
    int (*editor_exe)(void);
    /* The exe this version is made and tested with (core/platform/exe_identity.h): its SHA-256 (64 lowercase hex
       digits) and how players know it, e.g. "English GOG Emperor.exe 1.0.1.0". Another exe gets a log line and a
       notice; NULL: not checked. */
    const char *exe_sha256;
    const char *exe_label;
} core_game_info;

/* Provided by the game layer (src/games/<game>/game.cpp); only the entry point calls it. */
const core_game_info *game_info(void);

/* The entry point hands the game's info to the core (from DllMain, first). The pointer must stay valid. */
void core_game_set(const core_game_info *info);

/* The game's info; never NULL. Before core_game_set (the offline tests that do not set one): neutral values
   ("engine", engine.ini, 1024 x 768, no entry points). */
const core_game_info *core_game(void);

#ifdef __cplusplus
}
#endif
