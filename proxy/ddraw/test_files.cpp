// Offline test of core/platform/files (docs/testing/v0_2_0_files.md): where our files go.
//   - the decision with fake probes: game folder writable, cannot tell, Documents, Documents not available or not
//     writable (Controlled folder access) -> LocalAppData, nothing works -> game folder;
//   - the newest settings file among the candidates;
//   - the real probes: this exe's folder is writable by a standard user, C:\Program Files (x86) is not (whether or not
//     this test runs elevated), the known folders exist, a new folder below this exe's is usable (made and removed);
//   - switching file virtualization off and on again for this process (informational when it is not on).
// Writes only below this exe's folder (test_files_tmp\, removed at the end).
#include "core/platform/files.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace files = core::platform::files;

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-72s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

// The fake world.
struct Fake {
    int canWrite = 1;            // 1 yes, 0 no, -1 cannot tell
    bool docs = true, local = true;
    bool docsUsable = true, localUsable = true;
} g_fake;

bool FakeCanWrite(const std::wstring &, std::string *why)
{
    if (g_fake.canWrite < 0) {
        *why = "GetFileSecurity error 5";
    }
    return g_fake.canWrite == 1;
}

bool FakeKnown(int which, std::wstring *path, std::string *why)
{
    if (which == 0 ? !g_fake.docs : !g_fake.local) {
        *why = "SHGetKnownFolderPath 0x80070002";
        return false;
    }
    *path = which == 0 ? L"D:\\Docs" : L"E:\\Profile\\Local\\"; // the second with a slash: kept single
    return true;
}

bool FakeUsable(const std::wstring &folder, std::string *why)
{
    const bool docs = folder.find(L"D:\\Docs") == 0;
    if (docs ? g_fake.docsUsable : g_fake.localUsable) {
        return true;
    }
    *why = "cannot write in ... (error 5, e.g. Controlled folder access)";
    return false;
}

files::Decision Run(Fake f)
{
    g_fake = f;
    return files::Decide(L"C:\\Program Files (x86)\\Game\\", L"Yongle", files::Probe{&FakeCanWrite, &FakeKnown,
                                                                                     &FakeUsable});
}

bool Is(const files::Decision &d, const wchar_t *folder, core_files_where where)
{
    return d.folder == folder && d.where == where;
}

FILETIME Time(DWORD low)
{
    FILETIME t{};
    t.dwLowDateTime = low;
    t.dwHighDateTime = 1;
    return t;
}

} // namespace

int main()
{
    const wchar_t *game = L"C:\\Program Files (x86)\\Game\\";
    const wchar_t *docs = L"D:\\Docs\\My Games\\Yongle\\";
    const wchar_t *local = L"E:\\Profile\\Local\\Yongle\\";

    // --- the decision
    Check(Is(Run(Fake{}), game, CORE_FILES_GAME_FOLDER), "writable game folder: the game folder");
    Fake f;
    f.canWrite = -1;
    Check(Is(Run(f), game, CORE_FILES_GAME_FOLDER), "cannot tell: the game folder (as before)");
    f = Fake{};
    f.canWrite = 0;
    Check(Is(Run(f), docs, CORE_FILES_DOCUMENTS), "protected game folder: Documents\\My Games\\Yongle");
    f.docs = false;
    Check(Is(Run(f), local, CORE_FILES_LOCAL_APPDATA), "Documents not available: LocalAppData\\Yongle");
    f.docs = true;
    f.docsUsable = false;
    {
        files::Decision d = Run(f);
        Check(Is(d, local, CORE_FILES_LOCAL_APPDATA), "Documents not writable: LocalAppData\\Yongle");
        bool noted = false;
        for (const std::string &n : d.notes) {
            noted = noted || n.find("Controlled folder access") != std::string::npos;
        }
        Check(noted, "  the reason is in the notes");
    }
    f.localUsable = false;
    Check(Is(Run(f), game, CORE_FILES_GAME_FOLDER), "neither works: the game folder");
    f.local = false;
    Check(Is(Run(f), game, CORE_FILES_GAME_FOLDER), "neither available: the game folder");

    // --- the newest settings file
    std::vector<files::Candidate> c(3);
    Check(files::Newest(c) == -1, "no candidate exists: none");
    c[0].exists = true;
    c[0].written = Time(100);
    Check(files::Newest(c) == 0, "one exists: that one");
    c[1].exists = true;
    c[1].written = Time(200);
    c[2].written = Time(900); // newer but missing
    Check(files::Newest(c) == 1, "two exist: the newer one (a missing one does not count)");

    // --- the real probes
    files::Probe p = files::SystemProbe();
    std::string why;
    const std::wstring here = core_files_module_folder();
    Check(p.userCanWrite(here, &why) && why.empty(), "this exe's folder: a standard user can write");
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    HANDLE token = nullptr;
    DWORD size = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size);
    }
    why.clear();
    const bool pf = p.userCanWrite(L"C:\\Program Files (x86)\\", &why);
    std::printf("  (this test runs %s)\n", type == TokenElevationTypeFull ? "elevated" : "not elevated");
    Check(!pf && why.empty(), "C:\\Program Files (x86): a standard user cannot write");
    std::wstring known;
    why.clear();
    Check(p.knownFolder(0, &known, &why) && !known.empty() && known.back() != L'\\', "Documents is known");
    std::printf("  Documents: %ls\n", known.c_str());
    why.clear();
    Check(p.knownFolder(1, &known, &why) && !known.empty(), "LocalAppData is known");
    const std::wstring tmp = here + L"test_files_tmp\\";
    const std::wstring sub = tmp + L"My Games\\Yongle\\";
    why.clear();
    Check(p.usable(sub, &why), "a new folder below this exe's (two levels): usable");
    Check(GetFileAttributesW((sub + L"yongle_write_test.tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
          "  the test file is gone");
    why.clear();
    Check(!p.usable(L"Q:\\no such drive\\Yongle\\", &why) && !why.empty(), "a folder that cannot be made: not usable");
    RemoveDirectoryW(sub.c_str());
    RemoveDirectoryW((tmp + L"My Games").c_str());
    RemoveDirectoryW(tmp.c_str());

    // --- file virtualization for this process
    DWORD allowed = 0, on = 0;
    HANDLE adjust = nullptr;
    GetTokenInformation(token, TokenVirtualizationAllowed, &allowed, sizeof(allowed), &size);
    GetTokenInformation(token, TokenVirtualizationEnabled, &on, sizeof(on), &size);
    std::printf("  file virtualization: allowed %lu, on %lu\n", allowed, on);
    if (on && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &adjust)) {
        DWORD off = 0, now = 1;
        const bool switched = SetTokenInformation(adjust, TokenVirtualizationEnabled, &off, sizeof(off)) != FALSE;
        GetTokenInformation(adjust, TokenVirtualizationEnabled, &now, sizeof(now), &size);
        Check(switched && now == 0, "file virtualization can be switched off by the process itself");
        DWORD back = 1;
        SetTokenInformation(adjust, TokenVirtualizationEnabled, &back, sizeof(back));
        GetTokenInformation(adjust, TokenVirtualizationEnabled, &now, sizeof(now), &size);
        Check(now == 1, "  and on again");
        CloseHandle(adjust);
    }
    else {
        std::printf("%-72s %s\n", "file virtualization off and on again", "SKIPPED (not on for this process)");
    }
    if (token) {
        CloseHandle(token);
    }

    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAILED" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
