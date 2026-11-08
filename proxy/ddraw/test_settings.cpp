// Offline test of the settings model (core/settings + core/config; no DLL, no game): loading emperor_engine.ini,
// checking values, applying requested changes through callbacks, Restart items, and saving changes back without
// touching anything else in the file (written to a temporary file first; a failed save leaves the file alone).
// The file used is emperor_engine.ini next to this exe (run_tests.ps1 puts the default back afterwards).
// A callback that faults (core/diag guard): the value goes back, the earlier callbacks get the old value again,
// nothing is saved, and later changes of that item are refused while the others still apply.
#include "core/config.h"
#include "core/diag/guard.h"
#include "core/log.h"
#include "core/settings/settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace cs = core::settings;

namespace {

int g_failures = 0;
wchar_t g_path[MAX_PATH];

void Check(bool ok, const char *what)
{
    std::printf("%-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

std::string ReadAll()
{
    std::string s;
    if (FILE *f = _wfopen(g_path, L"rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

void WriteAll(const std::string &s)
{
    FILE *f = _wfopen(g_path, L"wb");
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}

std::string Value(const char *section, const char *key)
{
    char v[cs::kValueSize];
    cs::Value(cs::Find(section, key), v, sizeof(v));
    return v;
}

int g_smoothCalls = 0;
std::string g_smoothLast;
bool OnSmooth(int, const char *v, bool initial)
{
    if (!initial) {
        ++g_smoothCalls;
        g_smoothLast = v;
    }
    return true;
}

bool FaultOn250(int, const char *v, bool initial)
{
    if (!initial && std::strcmp(v, "250") == 0) {
        *static_cast<volatile int *>(nullptr) = 1;
    }
    return true;
}

bool Refuse(int, const char *, bool initial)
{
    return initial; // "cannot change while the game runs"
}

} // namespace

int main()
{
    DWORD n = GetModuleFileNameW(nullptr, g_path, MAX_PATH);
    while (n > 0 && g_path[n - 1] != L'\\') --n;
    wcscpy(g_path + n, L"emperor_engine.ini");

    const std::string original = "; my settings\r\n"
                                 "[video]\r\n"
                                 "present = display\r\n"
                                 "scale = 125   ; I like this one\r\n"
                                 "resolution = scaled\r\n"
                                 "my_own_key = 7\r\n"
                                 "\r\n"
                                 "[gameplay]\r\n"
                                 "smooth_scroll = on\r\n"
                                 "\r\n"
                                 "[later]\r\n"
                                 "future = yes\r\n";
    WriteAll(original);
    core::diag::InstallExceptionLog();
    core_config_load(nullptr); // the file next to this exe
    const core_config *cfg = core_config_get();
    Check(cfg->present == CORE_PRESENT_DISPLAY && cfg->scale_percent == 125 && cfg->resolution == CORE_RESOLUTION_SCALED,
          "loaded: typed values (present, scale, resolution)");
    Check(cfg->panel_key_vk == 0x2D && cfg->panel_scale_percent == 0 && cfg->high_resolution_timer,
          "defaults for keys not in the file (panel_key Insert, panel_scale auto)");
    Check(cfg->renderer == 0 && Value("video", "renderer") == "auto", "renderer: auto by default (O1)");

    int smooth = cs::Find("gameplay", "smooth_scroll");
    cs::OnChange(smooth, &OnSmooth);
    cs::Request(smooth, "off", true);
    cs::RunPending();
    Check(g_smoothCalls == 1 && g_smoothLast == "off" && cfg->smooth_scroll == 0,
          "Live item: callback called, typed value updated");
    std::string want = original;
    want.replace(want.find("smooth_scroll = on"), 18, "smooth_scroll = off");
    Check(ReadAll() == want, "saved: only that value changed (comments, unknown keys kept)");
    Check(GetFileAttributesW((std::wstring(g_path) + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
          "no temporary file left behind");

    cs::Request(cs::Find("video", "scale"), "133", true); // not a step of 5
    cs::Request(cs::Find("video", "present"), "fast", true);
    cs::RunPending();
    Check(Value("video", "scale") == "125" && Value("video", "present") == "display" && ReadAll() == want,
          "invalid values dropped, file unchanged");

    cs::Request(cs::Find("video", "scale"), "150", true);
    cs::RunPending();
    want.replace(want.find("scale = 125   ;"), 15, "scale = 150   ;");
    Check(ReadAll() == want && cfg->scale_percent == 150, "inline comment after a changed value kept");

    int panelScale = cs::Find("ui", "panel_scale");
    cs::Request(panelScale, "120", true);
    cs::RunPending();
    want += "\r\n[ui]\r\npanel_scale = 120\r\n";
    Check(ReadAll() == want && cfg->panel_scale_percent == 120, "key of a missing section: section added at the end");

    int backend = cs::Find("video", "backend");
    cs::Request(backend, "system", true);
    cs::RunPending();
    Check(cs::RestartPending(backend) && cfg->backend == CORE_BACKEND_EMULATED && Value("video", "backend") == "system",
          "Restart item: saved and pending; the running value stays in effect");
    cs::Request(backend, "emulated", true);
    cs::RunPending();
    Check(!cs::RestartPending(backend), "Restart item back to its start-up value: not pending");

    int timer = cs::Find("timing", "high_resolution_timer");
    cs::OnChange(timer, &Refuse);
    cs::Request(timer, "off", true);
    cs::RunPending();
    Check(cs::RestartPending(timer), "Live item whose callback refuses: waits for a restart");

    // A re-layout waiting for "keep" (save = false) is not written when another item is saved in the same batch, and
    // a revert leaves the file as it was; "keep" (RequestSave) writes it. (Found in P2's in-game test: restore
    // defaults saved the unconfirmed resolution together with present.)
    {
        std::string before = ReadAll();
        int res = cs::Find("video", "resolution");
        cs::Request(res, "1920x1080", false);
        cs::Request(cs::Find("video", "present"), "cap60", true);
        cs::RunPending();
        std::string mid = ReadAll();
        Check(mid.find("resolution = scaled") != std::string::npos && mid.find("present = cap60") != std::string::npos &&
                  cfg->resolution_w == 1920,
              "unconfirmed re-layout value not saved with another item");
        cs::Request(res, "scaled", false); // revert
        cs::RunPending();
        Check(ReadAll() == mid && cfg->resolution == CORE_RESOLUTION_SCALED, "revert: file unchanged");
        cs::Request(res, "2560x1440", false);
        cs::RequestSave(); // keep
        cs::RunPending();
        Check(ReadAll().find("resolution = 2560x1440") != std::string::npos, "keep: the confirmed value is saved");
        cs::Request(res, "scaled", true);
        cs::Request(cs::Find("video", "present"), "display", true);
        cs::RunPending();
        Check(ReadAll() == before, "back to the earlier values: same file as before");
    }

    // Edited by hand while the game runs: a save keeps the user's newer lines and only writes what differs.
    std::string edited = ReadAll() + "; added by hand\r\n";
    WriteAll(edited);
    cs::Request(smooth, "on", true);
    cs::RunPending();
    std::string afterHand = edited;
    afterHand.replace(afterHand.find("smooth_scroll = off"), 19, "smooth_scroll = on");
    Check(ReadAll() == afterHand, "changes by hand since start-up kept");

    // Read-only file: the save fails, the file stays as it was.
    SetFileAttributesW(g_path, FILE_ATTRIBUTE_READONLY);
    cs::Request(smooth, "off", true);
    cs::RunPending();
    char result[160];
    cs::LastSaveResult(result, sizeof(result));
    Check(std::strstr(result, "could not save") != nullptr && ReadAll() == afterHand,
          "read-only file: save fails, reported, file unchanged");
    SetFileAttributesW(g_path, FILE_ATTRIBUTE_NORMAL);

    // A new file when there is none.
    DeleteFileW(g_path);
    cs::Request(panelScale, "200", true);
    cs::RunPending();
    std::string fresh = ReadAll();
    Check(fresh.find("[ui]\r\npanel_scale = 200\r\n") != std::string::npos &&
              fresh.find("smooth_scroll = off") != std::string::npos,
          "no file: created with the values that differ from the defaults");

    // A callback that faults: the value goes back, nothing is saved, the item can no longer be changed.
    cs::OnChange(panelScale, &FaultOn250);
    cs::Request(panelScale, "250", true);
    cs::RunPending();
    Check(Value("ui", "panel_scale") == "200" && cfg->panel_scale_percent == 200 && ReadAll() == fresh,
          "callback faults: old value back (typed view too), nothing saved");
    cs::Request(panelScale, "150", true);
    cs::Request(smooth, "on", true);
    cs::RunPending();
    Check(Value("ui", "panel_scale") == "200" && Value("gameplay", "smooth_scroll") == "on" && cfg->smooth_scroll,
          "afterwards that item is refused, other items still apply");

    // A renamed value (O2a: framed -> scaled): taken as its new name and written anew in its place, the comment after
    // it kept; the file as it was in emperor_engine.ini.bak.
    {
        static const char *const looks[] = {"scaled", "scaled_black"};
        cs::Register(cs::Item{"test", "look", "Look", "", cs::Type::Choice, "scaled", cs::Effect::Live, false, false, 0, 0,
                              0, nullptr, looks, nullptr, 2});
        cs::RenameValue("test", "look", "black", "scaled_black");
        const std::string old = "[test]\r\nlook = Black   ; mine\r\n";
        WriteAll(old);
        cs::Load(g_path);
        const std::wstring bakPath = std::wstring(g_path) + L".bak";
        std::string bak;
        if (FILE *f = _wfopen(bakPath.c_str(), L"rb")) {
            char buf[256];
            size_t got;
            while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) bak.append(buf, got);
            std::fclose(f);
        }
        Check(Value("test", "look") == "scaled_black" && ReadAll() == "[test]\r\nlook = scaled_black   ; mine\r\n" &&
                  bak == old,
              "renamed value: new name in effect and in the file, old file in .bak");
        DeleteFileW(bakPath.c_str());
        cs::Load(g_path);
        Check(Value("test", "look") == "scaled_black" &&
                  GetFileAttributesW(bakPath.c_str()) == INVALID_FILE_ATTRIBUTES,
              "renamed value: nothing to do the next time");
    }

    // A key setting with two keys (the second the alternative): canonical spelling, both in the typed view; the same
    // key twice is not a value.
    {
        const int panelKey = cs::Find("ui", "panel_key");
        cs::Request(panelKey, "insert ,ctrl+f12", true);
        cs::RunPending();
        Check(Value("ui", "panel_key") == "Insert, Ctrl+F12" && cfg->panel_key_vk == 0x2D && cfg->panel_key2_vk == 0x7B &&
                  cfg->panel_key2_mods == 1,
              "two keys: canonical, both in the typed view");
        cs::Request(panelKey, "Insert, Insert", true);
        cs::RunPending();
        Check(Value("ui", "panel_key") == "Insert, Ctrl+F12", "the same key twice: dropped");
        cs::Request(panelKey, "Insert", true);
        cs::RunPending();
        Check(Value("ui", "panel_key") == "Insert" && cfg->panel_key2_vk == 0, "back to one key: no alternative");
        cs::Request(panelKey, "", true);
        cs::RunPending();
        Check(Value("ui", "panel_key") == "" && cfg->panel_key_vk == 0, "no key at all: empty");
        cs::Request(panelKey, "Insert", true);
        cs::RunPending();
    }

    // Moved settings (2026-11-08: [display] -> [video], [sound] -> [audio], china_map -> empire_map): the old lines'
    // values taken for the new settings (with the new setting's renamed values), the old lines replaced in the file; a
    // new line already in the file wins over an old one; old unknown keys stay where they were.
    {
        static const char *const modes[] = {"scaled", "native"};
        cs::Register(cs::Item{"newsec", "width", "Width", "", cs::Type::Int, "10", cs::Effect::Live, false, false, 0, 100,
                              1, nullptr, nullptr, nullptr, 0});
        cs::Register(cs::Item{"newsec", "height", "Height", "", cs::Type::Int, "10", cs::Effect::Live, false, false, 0,
                              100, 1, nullptr, nullptr, nullptr, 0});
        cs::Register(cs::Item{"newsec", "map", "Map", "", cs::Type::Choice, "native", cs::Effect::Live, false, false, 0,
                              0, 0, nullptr, modes, nullptr, 2});
        cs::Register(cs::Item{"other", "intro", "Intro", "", cs::Type::Bool, "on", cs::Effect::Live, false, false, 0, 0,
                              0, nullptr, nullptr, nullptr, 0});
        cs::MoveSection("oldsec", "newsec");
        cs::MoveKey("oldsec", "old_map", "newsec", "map");
        cs::MoveKey("oldsec", "intro", "other", "intro");
        cs::RenameValue("newsec", "map", "original", "native");
        const std::string old = "[oldsec]\r\n"
                                "width = 42\r\n"
                                "height = 7\r\n"
                                "old_map = original\r\n"
                                "intro = off\r\n"
                                "mine = 1\r\n"
                                "\r\n"
                                "[newsec]\r\n"
                                "height = 9\r\n";
        WriteAll(old);
        cs::Load(g_path);
        const std::string got = ReadAll();
        Check(Value("newsec", "width") == "42" && Value("newsec", "height") == "9" && Value("newsec", "map") == "native" &&
                  Value("other", "intro") == "off",
              "moved: old values taken, a new line in the file wins, renamed value");
        Check(got.find("[oldsec]\r\nmine = 1\r\n") != std::string::npos && got.find("width = 42") != std::string::npos &&
                  got.find("map = native") != std::string::npos && got.find("[other]\r\nintro = off") != std::string::npos &&
                  got.find("old_map") == std::string::npos && got.find("height = 7") == std::string::npos &&
                  got.find("height = 9") != std::string::npos,
              "moved: the file has the new lines, the old ones gone, unknown keys kept");
        if (got.find("old_map") != std::string::npos || got.find("width = 42") == std::string::npos) {
            std::printf("  file:\n%s\n", got.c_str());
        }
        const std::wstring bakPath = std::wstring(g_path) + L".bak";
        DeleteFileW(bakPath.c_str());
        cs::Load(g_path);
        Check(ReadAll() == got && GetFileAttributesW(bakPath.c_str()) == INVALID_FILE_ATTRIBUTES &&
                  Value("newsec", "width") == "42",
              "moved: nothing to do the next time");

        // A value renamed only when its line is moved (present = original from an older file -> display): the same
        // value written in the new section itself stays.
        static const char *const looks2[] = {"own", "shown"};
        cs::Register(cs::Item{"newsec", "look2", "Look 2", "", cs::Type::Choice, "shown", cs::Effect::Live, false, false,
                              0, 0, 0, nullptr, looks2, nullptr, 2, nullptr, nullptr, nullptr, nullptr, 1u << 0});
        cs::RenameMovedValue("newsec", "look2", "own", "shown");
        WriteAll("[oldsec]\r\nlook2 = own\r\n");
        cs::Load(g_path);
        Check(Value("newsec", "look2") == "shown" && ReadAll().find("look2 = shown") != std::string::npos,
              "moved only: an older file's value renamed on the move");
        WriteAll("[newsec]\r\nlook2 = own\r\n");
        cs::Load(g_path);
        Check(Value("newsec", "look2") == "own" && ReadAll() == "[newsec]\r\nlook2 = own\r\n" &&
                  !cs::ChoiceListed(cs::Get(cs::Find("newsec", "look2")), 0) &&
                  cs::ChoiceListed(cs::Get(cs::Find("newsec", "look2")), 1),
              "moved only: set in the new section itself, it stays (a value not offered)");
        DeleteFileW(bakPath.c_str());
    }

    // An older file and this version's template: rewritten once with the template's descriptions, the values kept
    // (a renamed one under its new name), a removed setting gone, an unknown line kept; the old file in .bak.
    {
        cs::Retire("test", "gone", nullptr);
        static const char tmpl[] = "; Header of this version.\n"
                                   "\n"
                                   "[test]\n"
                                   "; scaled       = new description (default)\n"
                                   "; scaled_black = another\n"
                                   "look = scaled\n";
        cs::SetTemplate(tmpl);
        const std::wstring bakPath = std::wstring(g_path) + L".bak";
        const std::string old = "; Header of an older version.\r\n"
                                "[test]\r\n"
                                "; framed = old description\r\n"
                                "look = black\r\n"
                                "; a removed setting\r\n"
                                "gone = 1\r\n"
                                "mistyped = 9\r\n";
        WriteAll(old);
        cs::Load(g_path);
        std::string bak;
        if (FILE *f = _wfopen(bakPath.c_str(), L"rb")) {
            char buf[256];
            size_t got;
            while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) bak.append(buf, got);
            std::fclose(f);
        }
        const std::string want = "; Header of this version.\r\n"
                                 "\r\n"
                                 "[test]\r\n"
                                 "; scaled       = new description (default)\r\n"
                                 "; scaled_black = another\r\n"
                                 "look = scaled_black\r\n"
                                 "\r\n"
                                 "; Not described above (kept as written):\r\n"
                                 "mistyped = 9\r\n";
        Check(ReadAll() == want && bak == old && Value("test", "look") == "scaled_black",
              "template: older file rewritten, values kept, old file in .bak");
        DeleteFileW(bakPath.c_str());
        cs::Load(g_path);
        Check(ReadAll() == want && GetFileAttributesW(bakPath.c_str()) == INVALID_FILE_ATTRIBUTES,
              "template: nothing to do the next time");
        WriteAll(old);
        SetFileAttributesW(g_path, FILE_ATTRIBUTE_READONLY);
        cs::Load(g_path);
        Check(ReadAll() == old && Value("test", "look") == "scaled_black",
              "template: read-only file left as it is, values still read");
        SetFileAttributesW(g_path, FILE_ATTRIBUTE_NORMAL);

        // The file deleted by mistake, its .bak still there: made again with the .bak's values; the .bak unchanged.
        DeleteFileW(g_path);
        {
            FILE *f = _wfopen(bakPath.c_str(), L"wb");
            std::fwrite(old.data(), 1, old.size(), f);
            std::fclose(f);
        }
        cs::Request(cs::Find("test", "look"), "scaled", true); // so that the value read can be seen
        cs::RunPending();
        DeleteFileW(g_path);
        cs::Load(g_path);
        std::string bak2;
        if (FILE *f = _wfopen(bakPath.c_str(), L"rb")) {
            char buf[256];
            size_t got;
            while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) bak2.append(buf, got);
            std::fclose(f);
        }
        Check(ReadAll() == want && bak2 == old && Value("test", "look") == "scaled_black",
              "template: no file, .bak there: made from the template with the .bak's values");

        // No file and no .bak: the template itself, every setting at its default.
        DeleteFileW(bakPath.c_str());
        DeleteFileW(g_path);
        cs::Load(g_path);
        Check(ReadAll() == tmpl && Value("test", "look") == "scaled" &&
                  GetFileAttributesW(bakPath.c_str()) == INVALID_FILE_ATTRIBUTES,
              "template: no file, no .bak: the template, at the defaults");
        if (ReadAll() != tmpl) std::printf("  file:\n%s\n", ReadAll().c_str());
        DeleteFileW(g_path);
        cs::SetTemplate(nullptr);
    }

    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
