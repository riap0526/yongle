// Offline test of core/capture/screenshot's writer (O1, 2026-10-30; not the game): eight 2560x1440 pictures asked
// for at once, of which 6 are queued and 2 are not saved (logged, before the frame is copied); each saved picture's
// line has the processor time; Flush waits for all. The pictures go to shots\ next to the exe and are deleted after.
#include "core/capture/screenshot.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core::output {
void RequestScreenCapture(const char *, const char *) {} // not used by SaveOne
} // namespace core::output

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

int Count(const char *text, const char *what)
{
    int n = 0;
    for (const char *p = std::strstr(text, what); p; p = std::strstr(p + 1, what)) {
        ++n;
    }
    return n;
}

} // namespace

int main()
{
    DeleteFileA("test_screenshot.log"); // the log appends
    core_log_open(nullptr, "test_screenshot.log");
    const int w = 2560, h = 1440;
    auto *pixels = static_cast<unsigned short *>(std::malloc(sizeof(unsigned short) * w * h));
    unsigned x = 12345;
    for (int i = 0; i < w * h; ++i) { // noise: as slow to compress as a city
        x = x * 1103515245u + 12345u;
        pixels[i] = static_cast<unsigned short>(x >> 16);
    }
    int asked = 0;
    for (int i = 0; i < 8; ++i) {
        asked += core::capture::SaveOne(pixels, w * 2, w, h, "t8", "game") ? 1 : 0;
    }
    const int left = core::capture::Flush(120000);
    std::free(pixels);
    static char log[1 << 16];
    log[0] = 0;
    if (FILE *f = std::fopen("test_screenshot.log", "rb")) {
        log[std::fread(log, 1, sizeof(log) - 1, f)] = 0;
        std::fclose(f);
    }
    Check(asked == 6, "6 of the 8 pictures are queued");
    Check(Count(log, "not saved: 6 pictures are still being written") == 2, "the other 2 are logged as not saved");
    Check(Count(log, "screenshot: saved shots\\t8_") == 6, "6 pictures are saved");
    Check(Count(log, "ms of it on a processor)") == 6, "each saved line has the processor time");
    Check(left == 0, "Flush waited for all of them");
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA("shots/t8_*.png", &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            char path[MAX_PATH];
            std::snprintf(path, sizeof(path), "shots/%s", fd.cFileName);
            DeleteFileA(path);
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
