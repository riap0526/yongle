#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
// Lines logged before the log's folder is known (core_log_begin_buffer), written out when it is opened.
bool g_buffering = false;
char g_buffer[64 * 1024];
size_t g_buffered = 0;
bool g_bufferFull = false;

void InitLock()
{
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
}

// path: the full path of the log file.
void OpenPath(const wchar_t *path)
{
    // O5a (TODO 10): a log over kRotateBytes at start-up becomes <name>.old.log (replacing the previous one) and a new
    // one starts; never during a run. When the file is open elsewhere (the game that started the editor), the rename
    // fails and this run appends as before.
    constexpr ULONGLONG kRotateBytes = 4ull * 1024 * 1024;
    wchar_t old[MAX_PATH * 2] = L"";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a) &&
        ((static_cast<ULONGLONG>(a.nFileSizeHigh) << 32) | a.nFileSizeLow) > kRotateBytes) {
        const wchar_t *slash = wcsrchr(path, L'\\');
        const wchar_t *dot = wcsrchr(slash ? slash : path, L'.');
        const size_t stem = dot ? static_cast<size_t>(dot - path) : wcslen(path);
        if (stem + 4 + (dot ? wcslen(dot) : 0) + 1 <= sizeof(old) / sizeof(old[0])) {
            wmemcpy(old, path, stem);
            old[stem] = L'\0';
            wcscat(old, L".old");
            wcscat(old, dot ? dot : L"");
            if (!MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING)) {
                old[0] = L'\0';
            }
        }
    }
    // FILE_SHARE_WRITE: the editor (EmperorEdit.exe) starts while the game that launched it still has the log open;
    // both append whole lines (every line has its thread id; the first one of a process has its pid).
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    EnterCriticalSection(&g_lock);
    g_file = f;
    if (f != INVALID_HANDLE_VALUE && g_buffered) {
        DWORD written;
        WriteFile(f, g_buffer, static_cast<DWORD>(g_buffered), &written, nullptr);
    }
    const bool lost = g_bufferFull;
    g_buffering = false;
    g_buffered = 0;
    g_bufferFull = false;
    LeaveCriticalSection(&g_lock);
    if (lost) {
        core_log("log: some lines from before this point were lost (the start-up buffer was full)");
    }
    if (old[0]) {
        const wchar_t *name = wcsrchr(old, L'\\');
        core_log("log: the previous log was over %llu MB and is now %ls", kRotateBytes / (1024 * 1024),
                 name ? name + 1 : old);
    }
}

} // namespace

extern "C" void core_log_begin_buffer(void)
{
    InitLock();
    g_buffering = true;
}

extern "C" void core_log_open_folder(const wchar_t *folder, const char *file_name)
{
    InitLock();
    wchar_t path[MAX_PATH * 2];
    wchar_t name[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, file_name, -1, name, MAX_PATH) ||
        std::swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls%ls", folder, name) < 0) {
        return;
    }
    OpenPath(path);
}

extern "C" void core_log_open(void *module, const char *file_name)
{
    InitLock();
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(static_cast<HMODULE>(module), path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    while (len > 0 && path[len - 1] != L'\\' && path[len - 1] != L'/') {
        --len;
    }
    path[len] = L'\0';
    core_log_open_folder(path, file_name);
}

extern "C" void core_log_close(void)
{
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    if (g_lockReady) {
        DeleteCriticalSection(&g_lock);
        g_lockReady = false;
    }
}

extern "C" const char *core_describe_address(const void *addr, char *buf, unsigned len)
{
    HMODULE mod = nullptr;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod) &&
        GetModuleFileNameA(mod, path, MAX_PATH)) {
        const char *name = path;
        for (const char *p = path; *p; ++p) {
            if (*p == '\\' || *p == '/') {
                name = p + 1;
            }
        }
        std::snprintf(buf, len, "%s+0x%lx (%p)", name,
                      static_cast<unsigned long>(reinterpret_cast<const char *>(addr) -
                                                 reinterpret_cast<const char *>(mod)),
                      addr);
    }
    else {
        std::snprintf(buf, len, "%p", addr);
    }
    return buf;
}

extern "C" void core_log(const char *fmt, ...)
{
    if (!g_lockReady || (g_file == INVALID_HANDLE_VALUE && !g_buffering)) {
        return;
    }
    char buf[2048];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u [tid %lu] ", t.wHour, t.wMinute, t.wSecond,
                          t.wMilliseconds, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) {
        m = 0;
    }
    n += (m < static_cast<int>(sizeof(buf)) - n - 2) ? m : static_cast<int>(sizeof(buf)) - n - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD written;
    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        WriteFile(g_file, buf, static_cast<DWORD>(n), &written, nullptr);
    }
    else if (g_buffering) {
        if (g_buffered + n <= sizeof(g_buffer)) {
            std::memcpy(g_buffer + g_buffered, buf, n);
            g_buffered += n;
        }
        else {
            g_bufferFull = true;
        }
    }
    LeaveCriticalSection(&g_lock);
}
