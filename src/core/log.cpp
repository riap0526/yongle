#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>

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
    // FILE_SHARE_WRITE: the editor (EmperorEdit.exe) starts while the game that launched it still has the log open;
    // both append whole lines (every line has its thread id; the first one of a process has its pid).
    g_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
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
