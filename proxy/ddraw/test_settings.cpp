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
                                 "[display]\r\n"
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

    cs::Request(cs::Find("display", "scale"), "133", true); // not a step of 5
    cs::Request(cs::Find("display", "present"), "fast", true);
    cs::RunPending();
    Check(Value("display", "scale") == "125" && Value("display", "present") == "display" && ReadAll() == want,
          "invalid values dropped, file unchanged");

    cs::Request(cs::Find("display", "scale"), "150", true);
    cs::RunPending();
    want.replace(want.find("scale = 125   ;"), 15, "scale = 150   ;");
    Check(ReadAll() == want && cfg->scale_percent == 150, "inline comment after a changed value kept");

    int panelScale = cs::Find("ui", "panel_scale");
    cs::Request(panelScale, "120", true);
    cs::RunPending();
    want += "\r\n[ui]\r\npanel_scale = 120\r\n";
    Check(ReadAll() == want && cfg->panel_scale_percent == 120, "key of a missing section: section added at the end");

    int backend = cs::Find("display", "backend");
    cs::Request(backend, "system", true);
    cs::RunPending();
    Check(cs::RestartPending(backend) && cfg->backend == CORE_BACKEND_EMULATED && Value("display", "backend") == "system",
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
        int res = cs::Find("display", "resolution");
        cs::Request(res, "1920x1080", false);
        cs::Request(cs::Find("display", "present"), "cap60", true);
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
        cs::Request(cs::Find("display", "present"), "display", true);
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

    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
