// Offline test of core/input/keys (not the game): the key bindings on a fake game and a fake system.
// Each game's own actions are tested in its test_restore (the key
// scenarios); this covers what the game layer chooses: the game's own posted F6/F7 passed or not (Pericles / Yongle),
// the options and Esc callbacks, Alt characters, a fault in the game's description. Runs itself twice (the setting of
// postedModeKeysToGame is fixed per run, as in the games).
#include "core/diag/guard.h"
#include "core/display/virtual_display.h"
#include "core/input/keys.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace k = core::input::keys;

// What the bindings sent to the game (the real SendToGame is in virtual_display.cpp, not linked).
struct Sent {
    UINT m;
    WPARAM w;
    int ctrl;
};
Sent g_sent[16];
int g_sentCount = 0;
volatile int g_ctrl = 0;

namespace core::display {
LRESULT SendToGame(UINT m, WPARAM w, LPARAM)
{
    if (g_sentCount < 16) {
        g_sent[g_sentCount++] = Sent{m, w, g_ctrl};
    }
    return 0;
}
void SetKeyTranslator(KeyTranslator) {}
void SetKeyStateFilter(KeyStateFilter) {}
} // namespace core::display

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

bool LogContains(const char *text)
{
    FILE *f = std::fopen("test_keys.log", "rb");
    if (!f) {
        return false;
    }
    static char buf[1 << 16];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return std::strstr(buf, text) != nullptr;
}

using core::settings::Effect;
using core::settings::Item;
using core::settings::Type;

const k::Def kDefs[] = {
    {Item{"keys", "scroll_up", "Scroll up", "", Type::Hotkey, "Up", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::KeyDown, VK_UP, false, {"scroll_up", 0, 0, 0}, true},
    {Item{"keys", "open_options", "Open Options", "", Type::Hotkey, "F6", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::Options, 0, false, {"open_options", 1, 0, 0}},
    {Item{"keys", "pause", "Toggle pause", "", Type::Hotkey, "P", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::Char, 'p', false, {"pause", 2, 0, 0}},
    {Item{"keys", "rotate_building", "Rotate building", "", Type::Hotkey, "R", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::Rotate, 'R', false, {"rotate_building", 2, 0, 0}},
    {Item{"keys", "bookmark_set_1", "Set bookmark 1", "", Type::Hotkey, "Ctrl+F1", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::KeyDown, VK_F1, true, {"bookmark_set_1", 3, 0, 0}},
    {Item{"keys", "overlay_water", "Overlay: water", "", Type::Hotkey, "W", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::Call, 5, false, {"overlay_water", 4, 0, 0}},
};

bool g_city = true, g_editor = false, g_cityFaults = false;
int g_options = 0, g_escapes = 0;
bool g_escapeTakes = true;
unsigned g_mods = 0;

bool City()
{
    if (g_cityFaults) {
        *static_cast<volatile int *>(nullptr) = 1;
    }
    return g_city;
}
bool Editor()
{
    return g_editor;
}
bool Options()
{
    ++g_options;
    return true;
}
bool Escape(LPARAM)
{
    ++g_escapes;
    return g_escapeTakes;
}
unsigned g_called = 0;
bool g_callDoes = true;
bool CallAction(unsigned code)
{
    g_called = code;
    return g_callDoes;
}

const k::KeysSystem kSystem{[] { return g_mods; }, [](unsigned) { return false; }};

void Bind(const char *key, const char *v)
{
    core::settings::Request(core::settings::Find("keys", key), v, false);
    core::settings::RunPending();
}

void Run(bool passPosted)
{
    static const k::KeysGame game(kDefs, static_cast<int>(sizeof(kDefs) / sizeof(kDefs[0])), &City, &Editor, &g_ctrl,
                                  &Options, &Escape, &CallAction, passPosted);
    k::SetSystem(&kSystem);
    k::RegisterSettings(&game);
    k::Install();

    // A Send::Call action: the game layer's callback with its code, the key and its character eaten; its default key
    // is not one of the game's (bound elsewhere, it goes to the game); not on the editor's screens.
    g_sentCount = 0;
    Check(k::Translate(WM_KEYDOWN, 'W', 1) && g_called == 5 && g_sentCount == 0 && k::Translate(WM_CHAR, 'w', 1),
          "Call action: the callback gets its code, the key and its character eaten");
    g_callDoes = false;
    g_called = 0;
    Check(k::Translate(WM_KEYDOWN, 'W', 1) && g_called == 5 && LogContains("keys: overlay_water did nothing"),
          "Call action: the callback did nothing: logged, the key still taken");
    g_callDoes = true;
    Bind("overlay_water", "O");
    g_called = 0;
    Check(!k::Translate(WM_KEYDOWN, 'W', 1) && !k::Translate(WM_CHAR, 'w', 1) && g_called == 0,
          "Call action bound to O: W goes to the game (not a key of the game's own)");
    Check(k::Translate(WM_KEYDOWN, 'O', 1) && g_called == 5, "... O calls it");
    g_editor = true;
    g_city = false;
    g_called = 0;
    Check(!k::Translate(WM_KEYDOWN, 'O', 1) && g_called == 0, "Call action: not on the editor's screens");
    g_editor = false;
    g_city = true;
    Bind("overlay_water", "W");

    // The game's own posted display mode keys (lParam 0).
    g_sentCount = 0;
    g_options = 0;
    if (passPosted) {
        Check(!k::Translate(WM_KEYDOWN, VK_F6, 0) && !k::Translate(WM_KEYDOWN, VK_F7, 0) &&
                  !k::Translate(WM_KEYDOWN, VK_F8, 0) && g_options == 0 && g_sentCount == 0,
              "posted F6/F7/F8 (lParam 0) passed: the game's (Pericles until its own Display options buttons)");
    }
    else {
        Check(k::Translate(WM_KEYDOWN, VK_F6, 0) && g_options == 1 && k::Translate(WM_KEYDOWN, VK_F7, 0) &&
                  g_sentCount == 0,
              "posted F6/F7 not passed: F6 opens the options, F7 never reaches the game (Yongle)");
    }
    g_options = 0;
    Check(k::Translate(WM_KEYDOWN, VK_F6, 1) && g_options == 1, "F6 from the keyboard: the options window");
    Check(k::Translate(WM_KEYDOWN, VK_F7, 1) && k::Translate(WM_KEYDOWN, VK_F8, 1) && g_sentCount == 0,
          "F7, F8 from the keyboard: never the game's");

    // Esc: the game layer's callback decides.
    g_escapeTakes = true;
    Check(k::Translate(WM_KEYDOWN, VK_ESCAPE, 1) && g_escapes == 1 && k::Translate(WM_CHAR, 27, 1),
          "Esc taken by ours: its character too");
    g_escapeTakes = false;
    Check(!k::Translate(WM_KEYDOWN, VK_ESCAPE, 1) && g_escapes == 2 && !k::Translate(WM_CHAR, 27, 1),
          "Esc not taken: the key and its character go to the game");

    // Alt with a character: eaten (no game takes WM_SYSCHAR), Alt+Space not.
    g_mods = core::ui::kModAlt;
    Check(!k::Translate(WM_SYSKEYDOWN, 'Q', 1 << 29) && k::Translate(WM_SYSCHAR, 'q', 1 << 29),
          "Alt+Q: the key to the game, its character eaten");
    Check(!k::Translate(WM_SYSCHAR, ' ', 1 << 29), "Alt+Space: its character to Windows (the window menu)");
    g_mods = 0;

    // Ctrl actions: the game's key_ctrl for the call, put back.
    g_mods = core::ui::kModCtrl;
    g_sentCount = 0;
    g_ctrl = 0;
    Check(k::Translate(WM_KEYDOWN, VK_F1, 1) && g_sentCount == 1 && g_sent[0].w == VK_F1 && g_sent[0].ctrl == 1 &&
              g_ctrl == 0,
          "Ctrl+F1: F1 with key_ctrl 1, put back");
    g_sentCount = 0;
    Check(k::Translate(WM_KEYDOWN, VK_DOWN, 1) && g_sentCount == 0, "Ctrl+Down: kept from the game");
    g_mods = 0;

    // Other screens: only ours.
    g_city = false;
    g_sentCount = 0;
    g_options = 0;
    Check(!k::Translate(WM_KEYDOWN, 'P', 1) && !k::Translate(WM_CHAR, 'p', 1) && g_sentCount == 0,
          "not a city screen: P as it is");
    Check(k::Translate(WM_KEYDOWN, VK_F6, 1) && g_options == 1, "not a city screen: F6 still opens the options");
    // The editor's screens: only the actions marked for it.
    g_editor = true;
    Bind("scroll_up", "W");
    g_sentCount = 0;
    Check(k::Translate(WM_KEYDOWN, 'W', 1) && g_sentCount == 1 && g_sent[0].w == VK_UP, "editor: W scrolls up");
    Check(!k::Translate(WM_KEYDOWN, 'P', 1) && g_sentCount == 1, "editor: P is the editor's own");
    g_editor = false;
    g_city = true;

    // R: the poll says pressed once.
    g_sentCount = 0;
    Check(k::Translate(WM_KEYDOWN, 'R', 1) && k::KeyState('R', 0) == static_cast<SHORT>(0x8001) &&
              k::KeyState('R', 0) == 0,
          "R: pressed once");

    // A fault in the game's description: off, the keys the game's own from then on.
    g_cityFaults = true;
    Check(!k::Translate(WM_KEYDOWN, 'W', 1), "a fault: the key to the game");
    g_cityFaults = false;
    Check(LogContains("feature \"key bindings (core/input/keys)\" switched off"), "... the bindings off");
    Check(!k::Translate(WM_KEYDOWN, 'W', 1) && !k::Translate(WM_KEYDOWN, VK_F7, 1), "... every key the game's");
    Check(k::KeyState('R', static_cast<SHORT>(0x8000)) == static_cast<SHORT>(0x8000), "... R polled as it is");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 2) {
        DeleteFileA("test_keys.log");
        core_log_open(GetModuleHandleW(nullptr), "test_keys.log");
        core::diag::InstallExceptionLog();
        Run(std::strcmp(argv[1], "pass") == 0);
        return g_failures ? 1 : 0;
    }
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    int failed = 0;
    for (const char *mode : {"block", "pass"}) {
        std::printf("posted mode keys: %s\n", mode);
        std::fflush(stdout);
        char cmd[MAX_PATH + 16];
        std::snprintf(cmd, sizeof(cmd), "\"%s\" %s", self, mode);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        DWORD code = 99;
        if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 30000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        if (code != 0) {
            ++failed;
        }
    }
    std::printf("RESULT: %s (%d failure(s))\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
