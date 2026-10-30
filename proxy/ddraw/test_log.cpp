// Offline test of core/log's size limit (O5a, 2026-10-30): a log over 4 MB at start-up becomes <name>.old.log and a new
// one starts with a line saying so; a smaller one is appended to. Files next to the exe, deleted after.
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

long long Size(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) {
        return -1;
    }
    return (static_cast<long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

void Fill(const char *path, long long bytes)
{
    FILE *f = std::fopen(path, "wb");
    static char line[1024];
    std::memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\n';
    for (long long n = 0; n < bytes; n += sizeof(line)) {
        std::fwrite(line, 1, sizeof(line), f);
    }
    std::fclose(f);
}

bool Contains(const char *path, const char *what)
{
    static char text[1 << 16];
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    text[std::fread(text, 1, sizeof(text) - 1, f)] = 0;
    std::fclose(f);
    return std::strstr(text, what) != nullptr;
}

} // namespace

int main()
{
    DeleteFileA("test_log.log");
    DeleteFileA("test_log.old.log");

    Fill("test_log.log", 1024 * 1024); // 1 MB: kept and appended to
    core_log_open(nullptr, "test_log.log");
    core_log("small");
    core_log_close();
    Check(Size("test_log.old.log") < 0 && Size("test_log.log") > 1024 * 1024, "a log under 4 MB is appended to");

    Fill("test_log.log", 5 * 1024 * 1024); // 5 MB: renamed
    core_log_open(nullptr, "test_log.log");
    core_log("after");
    core_log_close();
    Check(Size("test_log.old.log") >= 5 * 1024 * 1024, "a log over 4 MB becomes test_log.old.log");
    Check(Size("test_log.log") < 4096 && Contains("test_log.log", "the previous log was over 4 MB and is now "
                                                                   "test_log.old.log") &&
              Contains("test_log.log", "after"),
          "the new log starts with a line saying so");

    Fill("test_log.log", 5 * 1024 * 1024); // again: replaces the previous old one
    core_log_open(nullptr, "test_log.log");
    core_log_close();
    Check(Size("test_log.old.log") >= 5 * 1024 * 1024 && Size("test_log.log") < 4096, "the old one is replaced");

    DeleteFileA("test_log.log");
    DeleteFileA("test_log.old.log");
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
