#include "core/platform/files.h"

#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include <objbase.h>

namespace core::platform::files {
namespace {

std::string Format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::string Utf8(const std::wstring &w)
{
    char buf[MAX_PATH * 6];
    if (!WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof(buf), nullptr, nullptr)) {
        return "?";
    }
    return buf;
}

std::wstring WithSlash(std::wstring s)
{
    if (!s.empty() && s.back() != L'\\' && s.back() != L'/') {
        s += L'\\';
    }
    return s;
}

std::wstring WithoutSlash(std::wstring s)
{
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) {
        s.pop_back();
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// The real probes

bool SysUserCanWrite(const std::wstring &folder, std::string *why)
{
    HANDLE process = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process)) {
        *why = Format("OpenProcessToken error %lu", GetLastError());
        return false;
    }
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD size = 0;
    GetTokenInformation(process, TokenElevationType, &type, sizeof(type), &size);
    HANDLE check = nullptr;
    if (type == TokenElevationTypeFull) {
        // Elevated: ask for the same user without elevation (an identification token, enough for AccessCheck).
        TOKEN_LINKED_TOKEN linked{};
        if (GetTokenInformation(process, TokenLinkedToken, &linked, sizeof(linked), &size)) {
            check = linked.LinkedToken;
        }
        else {
            *why = Format("TokenLinkedToken error %lu", GetLastError());
        }
    }
    else if (!DuplicateToken(process, SecurityIdentification, &check)) {
        *why = Format("DuplicateToken error %lu", GetLastError());
        check = nullptr;
    }
    CloseHandle(process);
    if (!check) {
        return false;
    }
    const std::wstring path = WithoutSlash(folder);
    const SECURITY_INFORMATION info =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION;
    DWORD need = 0;
    GetFileSecurityW(path.c_str(), info, nullptr, 0, &need);
    std::vector<BYTE> sd(need ? need : 1);
    bool result = false;
    if (!need || !GetFileSecurityW(path.c_str(), info, sd.data(), need, &need)) {
        *why = Format("GetFileSecurity error %lu", GetLastError());
    }
    else {
        GENERIC_MAPPING map{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        DWORD want = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
        MapGenericMask(&want, &map);
        PRIVILEGE_SET privileges{};
        DWORD privilegesSize = sizeof(privileges);
        DWORD granted = 0;
        BOOL status = FALSE;
        if (!AccessCheck(sd.data(), check, want, &map, &privileges, &privilegesSize, &granted, &status)) {
            *why = Format("AccessCheck error %lu", GetLastError());
        }
        else {
            result = status != FALSE;
        }
    }
    CloseHandle(check);
    return result;
}

// shell32 and ole32 are loaded here, not imported: nothing of them under the loader lock.
bool SysKnownFolder(int which, std::wstring *out, std::string *why)
{
    static const GUID kDocuments = {0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}};
    static const GUID kLocalAppData = {0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
    using Get = HRESULT(WINAPI *)(const GUID &, DWORD, HANDLE, wchar_t **);
    using Free = void(WINAPI *)(void *);
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    auto get = shell ? reinterpret_cast<Get>(reinterpret_cast<void *>(GetProcAddress(shell, "SHGetKnownFolderPath")))
                     : nullptr;
    auto free = ole ? reinterpret_cast<Free>(reinterpret_cast<void *>(GetProcAddress(ole, "CoTaskMemFree"))) : nullptr;
    bool ok = false;
    if (!get || !free) {
        *why = "SHGetKnownFolderPath not available";
    }
    else {
        wchar_t *path = nullptr;
        const HRESULT hr = get(which == 0 ? kDocuments : kLocalAppData, 0, nullptr, &path);
        if (SUCCEEDED(hr) && path && path[0]) {
            *out = WithoutSlash(path);
            ok = true;
        }
        else {
            *why = Format("SHGetKnownFolderPath(%s) 0x%08lx", which == 0 ? "Documents" : "LocalAppData",
                          static_cast<unsigned long>(hr));
        }
        if (path) {
            free(path);
        }
    }
    if (!ok && which == 1) { // the environment as the last resort for %LOCALAPPDATA%
        wchar_t env[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            *out = WithoutSlash(env);
            ok = true;
        }
    }
    // shell32 and ole32 stay loaded (the game's own DLLs load them anyway); nothing is freed that may be in use.
    return ok;
}

bool SysUsable(const std::wstring &folder, std::string *why)
{
    // Every missing level (e.g. My Games, then Yongle); only the last error counts.
    const std::wstring path = WithoutSlash(folder);
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') {
            const std::wstring part = path.substr(0, i);
            if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS &&
                i == path.size()) {
                *why = Format("cannot create %s (error %lu)", Utf8(part).c_str(), GetLastError());
                return false;
            }
        }
    }
    const std::wstring test = WithSlash(folder) + L"yongle_write_test.tmp";
    HANDLE f = CreateFileW(test.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *why = Format("cannot write in %s (error %lu%s)", Utf8(folder).c_str(), GetLastError(),
                      GetLastError() == ERROR_ACCESS_DENIED ? ", e.g. Controlled folder access" : "");
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(f, "y", 1, &written, nullptr) && written == 1;
    const DWORD error = GetLastError();
    CloseHandle(f);
    if (!ok) {
        *why = Format("cannot write in %s (error %lu)", Utf8(folder).c_str(), error);
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------
// State

wchar_t g_folder[MAX_PATH * 2] = L"";
char g_folderUtf8[MAX_PATH * 6] = "";
wchar_t g_module[MAX_PATH * 2] = L"";
std::wstring g_app;
core_files_where g_where = CORE_FILES_GAME_FOLDER;
bool g_decided = false;
std::vector<std::string> g_notes;

void Set(const std::wstring &folder)
{
    std::swprintf(g_folder, sizeof(g_folder) / sizeof(g_folder[0]), L"%ls", folder.c_str());
    if (!WideCharToMultiByte(CP_UTF8, 0, g_folder, -1, g_folderUtf8, sizeof(g_folderUtf8), nullptr, nullptr)) {
        g_folderUtf8[0] = '\0';
    }
}

const wchar_t *ModuleFolder()
{
    if (!g_module[0]) {
        HMODULE self = nullptr;
        wchar_t path[MAX_PATH];
        DWORD len = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&ModuleFolder), &self)) {
            len = GetModuleFileNameW(self, path, MAX_PATH);
        }
        if (len == 0 || len >= MAX_PATH) {
            return g_module; // empty: relative to the current folder, as a last resort
        }
        while (len > 0 && path[len - 1] != L'\\' && path[len - 1] != L'/') {
            --len;
        }
        path[len] = L'\0';
        std::swprintf(g_module, sizeof(g_module) / sizeof(g_module[0]), L"%ls", path);
    }
    return g_module;
}

// Windows' file virtualization off for this process while it lives, then back as it was. Only in the start-up work,
// before the game's own code runs: the game's saves rely on it.
class VirtualizationOff {
public:
    VirtualizationOff()
    {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &token_)) {
            token_ = nullptr;
            return;
        }
        DWORD on = 0, size = 0;
        if (GetTokenInformation(token_, TokenVirtualizationEnabled, &on, sizeof(on), &size) && on) {
            DWORD off = 0;
            switched_ = SetTokenInformation(token_, TokenVirtualizationEnabled, &off, sizeof(off)) != FALSE;
            error_ = switched_ ? 0 : GetLastError();
            failed_ = !switched_;
        }
    }
    ~VirtualizationOff()
    {
        if (switched_) {
            DWORD on = 1;
            SetTokenInformation(token_, TokenVirtualizationEnabled, &on, sizeof(on));
        }
        if (token_) {
            CloseHandle(token_);
        }
    }
    bool Failed() const { return failed_; } // virtualization was on and could not be switched off
    DWORD Error() const { return error_; }

private:
    HANDLE token_ = nullptr;
    bool switched_ = false;
    bool failed_ = false;
    DWORD error_ = 0;
};

bool Attributes(const std::wstring &path, FILETIME *written)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }
    *written = a.ftLastWriteTime;
    return true;
}

bool ReadAll(const std::wstring &path, std::string *out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < 16 * 1024 * 1024;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out->empty() || (ReadFile(f, &(*out)[0], static_cast<DWORD>(out->size()), &got, nullptr) &&
                              got == out->size());
    }
    CloseHandle(f);
    return ok;
}

std::string Time(const FILETIME &t)
{
    FILETIME local;
    SYSTEMTIME s;
    if (!FileTimeToLocalFileTime(&t, &local) || !FileTimeToSystemTime(&local, &s)) {
        return "?";
    }
    return Format("%04u-%02u-%02u %02u:%02u:%02u", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond);
}

// The VirtualStore copy of the game folder: %LOCALAPPDATA%\VirtualStore\<the path without its drive>.
std::wstring VirtualStoreOf(const std::wstring &folder)
{
    std::wstring local;
    std::string why;
    if (folder.size() < 3 || folder[1] != L':' || !SysKnownFolder(1, &local, &why)) {
        return L"";
    }
    return WithSlash(local) + L"VirtualStore\\" + folder.substr(3);
}

} // namespace

Decision Decide(const std::wstring &gameFolder, const wchar_t *app, const Probe &probe)
{
    Decision d;
    std::string why;
    if (probe.userCanWrite(gameFolder, &why)) {
        d.folder = gameFolder;
        d.notes.push_back("a standard user can write to the game folder: our files stay there");
        return d;
    }
    if (!why.empty()) { // could not tell: as before
        d.folder = gameFolder;
        d.notes.push_back("could not check the game folder's permissions (" + why + "): our files stay there");
        return d;
    }
    d.notes.push_back("a standard user cannot write to the game folder (e.g. under Program Files)");
    std::wstring known;
    why.clear();
    if (probe.knownFolder(0, &known, &why)) {
        const std::wstring folder = WithSlash(known) + L"My Games\\" + app + L"\\";
        why.clear();
        if (probe.usable(folder, &why)) {
            d.folder = folder;
            d.where = CORE_FILES_DOCUMENTS;
            return d;
        }
        d.notes.push_back("Documents: " + why);
    }
    else {
        d.notes.push_back("Documents not available: " + why);
    }
    why.clear();
    if (probe.knownFolder(1, &known, &why)) {
        const std::wstring folder = WithSlash(known) + app + L"\\";
        why.clear();
        if (probe.usable(folder, &why)) {
            d.folder = folder;
            d.where = CORE_FILES_LOCAL_APPDATA;
            return d;
        }
        d.notes.push_back("LocalAppData: " + why);
    }
    else {
        d.notes.push_back("LocalAppData not available: " + why);
    }
    d.folder = gameFolder;
    d.notes.push_back("no other folder works: our files stay in the game folder");
    return d;
}

Probe SystemProbe()
{
    return Probe{&SysUserCanWrite, &SysKnownFolder, &SysUsable};
}

int Newest(const std::vector<Candidate> &candidates)
{
    int best = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (candidates[i].exists &&
            (best < 0 || CompareFileTime(&candidates[i].written, &candidates[best].written) > 0)) {
            best = i;
        }
    }
    return best;
}

} // namespace core::platform::files

namespace files = core::platform::files;

extern "C" void core_files_decide(const wchar_t *app, int safe_only)
{
    if (files::g_decided) {
        return;
    }
    files::g_decided = true;
    files::g_app = app ? app : L"Yongle";
    const std::wstring game = files::ModuleFolder();
    if (safe_only) {
        files::Set(game);
        files::g_notes.push_back("decided under the loader lock: our files stay in the game folder");
        return;
    }
    files::Decision d = files::Decide(game, files::g_app.c_str(), files::SystemProbe());
    files::Set(d.folder);
    files::g_where = d.where;
    files::g_notes = std::move(d.notes);
}

extern "C" const wchar_t *core_files_folder(void)
{
    if (!files::g_folder[0]) {
        return files::ModuleFolder();
    }
    return files::g_folder;
}

extern "C" const char *core_files_folder_utf8(void)
{
    if (!files::g_folder[0]) {
        files::Set(files::ModuleFolder());
    }
    return files::g_folderUtf8;
}

extern "C" core_files_where core_files_where_now(void)
{
    return files::g_where;
}

extern "C" const wchar_t *core_files_module_folder(void)
{
    return files::ModuleFolder();
}

extern "C" void core_files_log(void)
{
    static const char *const kWhere[] = {"the game folder", "Documents", "LocalAppData"};
    for (const std::string &n : files::g_notes) {
        core_log("files: %s", n.c_str());
    }
    core_log("files: our files (log, settings, screenshots, menu backdrop) are in %s (%s)", core_files_folder_utf8(),
             kWhere[files::g_where]);
    if (files::g_where != CORE_FILES_GAME_FOLDER) {
        const std::wstring game = files::ModuleFolder();
        const std::wstring store = files::VirtualStoreOf(game);
        const DWORD a = store.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(store.c_str());
        core_log("files: older files of ours (log, screenshots) may still be in %s%s%s", files::Utf8(game).c_str(),
                 a != INVALID_FILE_ATTRIBUTES ? " and in " : "",
                 a != INVALID_FILE_ATTRIBUTES ? files::Utf8(store).c_str() : "");
    }
}

// Called by the settings (core/config) before the file is read: when our folder is not the game folder and has no
// settings file yet, the newest one found elsewhere is copied over (the others stay where they are).
extern "C" void core_files_bring_over(const wchar_t *name)
{
    if (files::g_where == CORE_FILES_GAME_FOLDER) {
        return;
    }
    const std::wstring target = std::wstring(files::g_folder) + name;
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;
    }
    const std::wstring game = files::ModuleFolder();
    std::vector<files::Candidate> c;
    {
        files::VirtualizationOff off; // the game folder's own copy, not the VirtualStore one it would show
        files::Candidate g{game + name, "game folder"};
        g.exists = files::Attributes(g.path, &g.written);
        if (off.Failed()) {
            core_log("files: could not switch file virtualization off (error %lu): the game folder's copy may be "
                     "the VirtualStore one",
                     off.Error());
        }
        c.push_back(g);
    }
    const std::wstring store = files::VirtualStoreOf(game);
    if (!store.empty()) {
        files::Candidate v{store + name, "VirtualStore"};
        v.exists = files::Attributes(v.path, &v.written);
        c.push_back(v);
    }
    std::wstring known;
    std::string why;
    if (files::g_where == CORE_FILES_DOCUMENTS && files::SysKnownFolder(1, &known, &why)) {
        files::Candidate o{files::WithSlash(known) + files::g_app + L"\\" + name, "LocalAppData"};
        o.exists = files::Attributes(o.path, &o.written);
        c.push_back(o);
    }
    else if (files::g_where == CORE_FILES_LOCAL_APPDATA && files::SysKnownFolder(0, &known, &why)) {
        files::Candidate o{files::WithSlash(known) + L"My Games\\" + files::g_app + L"\\" + name, "Documents"};
        o.exists = files::Attributes(o.path, &o.written);
        c.push_back(o);
    }
    const int best = files::Newest(c);
    for (int i = 0; i < static_cast<int>(c.size()); ++i) {
        core_log("files: settings file in the %s: %s%s", c[i].label.c_str(), files::Utf8(c[i].path).c_str(),
                 c[i].exists ? (" (written " + files::Time(c[i].written) + ")").c_str() : " (none)");
    }
    if (best < 0) {
        core_log("files: no settings file to bring over: defaults");
        return;
    }
    std::string text;
    bool read;
    {
        files::VirtualizationOff off;
        read = files::ReadAll(c[best].path, &text);
    }
    HANDLE f = read ? CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)
                    : INVALID_HANDLE_VALUE;
    DWORD written = 0;
    const bool ok = f != INVALID_HANDLE_VALUE &&
                    (text.empty() || (WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                                      written == text.size()));
    const DWORD error = GetLastError();
    if (f != INVALID_HANDLE_VALUE) {
        CloseHandle(f);
        if (!ok) {
            DeleteFileW(target.c_str());
        }
    }
    if (ok) {
        core_log("files: copied the newest settings file (the %s's) to %s; the others are left as they are",
                 c[best].label.c_str(), files::Utf8(target).c_str());
    }
    else {
        core_log("files: could not copy %s to %s (%s, error %lu): defaults", files::Utf8(c[best].path).c_str(),
                 files::Utf8(target).c_str(), read ? "write" : "read", error);
    }
}

namespace {

DWORD WINAPI OpenFolderThread(void *)
{
    using Exec = HINSTANCE(WINAPI *)(HWND, const wchar_t *, const wchar_t *, const wchar_t *, const wchar_t *, int);
    using Init = HRESULT(WINAPI *)(void *, DWORD);
    HMODULE ole = LoadLibraryW(L"ole32.dll"); // ShellExecute wants COM on its thread
    auto init = ole ? reinterpret_cast<Init>(reinterpret_cast<void *>(GetProcAddress(ole, "CoInitializeEx"))) : nullptr;
    if (init) {
        init(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    auto exec = shell ? reinterpret_cast<Exec>(reinterpret_cast<void *>(GetProcAddress(shell, "ShellExecuteW")))
                      : nullptr;
    const intptr_t r = exec ? reinterpret_cast<intptr_t>(exec(nullptr, L"open", core_files_folder(), nullptr, nullptr,
                                                               SW_SHOWNORMAL))
                            : 0;
    core_log("files: open folder %s -> %ld", core_files_folder_utf8(), static_cast<long>(r));
    return 0;
}

} // namespace

extern "C" void core_files_open_folder(void)
{
    HANDLE t = CreateThread(nullptr, 0, &OpenFolderThread, nullptr, 0, nullptr);
    if (t) {
        CloseHandle(t);
    }
    else {
        core_log("files: open folder: CreateThread error %lu", GetLastError());
    }
}
