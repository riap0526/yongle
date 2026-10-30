#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

} // namespace

extern "C" void core_log_open(void *module, const char *file_name)
{
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(static_cast<HMODULE>(module), path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    char *slash = nullptr;
    for (char *p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            slash = p;
        }
    }
    if (!slash || static_cast<size_t>(slash - path) + 1 + strlen(file_name) + 1 > MAX_PATH) {
        return;
    }
    strcpy(slash + 1, file_name);
    // O5a (docs/optimization.md, TODO 10): a log over kRotateBytes at start-up becomes <name>.old.log (replacing the
    // previous one) and a new one starts; never during a run. When the file is open elsewhere (the game that started
    // the editor), the rename fails and this run appends as before.
    constexpr ULONGLONG kRotateBytes = 4ull * 1024 * 1024;
    char old[MAX_PATH] = "";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &a) &&
        ((static_cast<ULONGLONG>(a.nFileSizeHigh) << 32) | a.nFileSizeLow) > kRotateBytes) {
        const char *dot = strrchr(slash + 1, '.');
        const size_t stem = dot ? static_cast<size_t>(dot - path) : strlen(path);
        if (stem + strlen(".old") + strlen(dot ? dot : "") + 1 <= MAX_PATH) {
            memcpy(old, path, stem);
            old[stem] = '\0';
            strcat(old, ".old");
            strcat(old, dot ? dot : "");
            if (!MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING)) {
                old[0] = '\0';
            }
        }
    }
    // FILE_SHARE_WRITE: the editor (EmperorEdit.exe) starts while the game that launched it still has the log open;
    // both append whole lines (every line has its thread id; the first one of a process has its pid).
    g_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (old[0]) {
        core_log("log: the previous log was over %llu MB and is now %s", kRotateBytes / (1024 * 1024),
                 strrchr(old, '\\') ? strrchr(old, '\\') + 1 : old);
    }
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
    if (g_file == INVALID_HANDLE_VALUE || !g_lockReady) {
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
    WriteFile(g_file, buf, static_cast<DWORD>(n), &written, nullptr);
    LeaveCriticalSection(&g_lock);
}
