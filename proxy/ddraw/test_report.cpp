// Offline test of core/diag/report (not the game; R2b.1): the frame check counts game frames that hand two or more
// frames to the output (with how they were handed over in the log) and only those, not presents outside a game frame,
// and nested begin/end count once; torn output frames are counted and logged, the log lines stop after 10 of each
// kind while the counts go on; the window events of interest are logged and the others are not; report numbers go
// up and the game thread takes each request once.
#include "core/diag/report.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-78s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

int LogCount(const char *text)
{
    FILE *f = std::fopen("test_report.log", "rb");
    if (!f) {
        return 0;
    }
    static char buf[1 << 20];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    int count = 0;
    for (const char *p = buf; (p = std::strstr(p, text)) != nullptr; p += std::strlen(text)) {
        ++count;
    }
    return count;
}

void Frame(int presents, const char *how = "BltFast")
{
    core::diag::GameFrameBegin();
    for (int i = 0; i < presents; ++i) {
        core::diag::NotePresent(i == 0 && presents > 1 ? "Blt(colorfill)" : how);
    }
    core::diag::GameFrameEnd();
}

} // namespace

int main()
{
    DeleteFileA("test_report.log");
    core_log_open(GetModuleHandleW(nullptr), "test_report.log");
    long multi = -1, torn = -1;
    core::diag::TakeIntegrity(&multi, &torn);
    Check(multi == 0 && torn == 0, "nothing counted at the start");

    // Frame check: one present per game frame is normal.
    for (int i = 0; i < 50; ++i) {
        Frame(1);
    }
    Frame(0);
    core::diag::NotePresent("Unlock"); // outside any game frame (e.g. a loading screen)
    core::diag::NotePresent("Unlock");
    core::diag::TakeIntegrity(&multi, &torn);
    Check(multi == 0 && torn == 0, "one present per game frame, or presents outside a frame: nothing counted");

    Frame(2);
    core::diag::TakeIntegrity(&multi, &torn);
    Check(multi == 1, "two presents in one game frame: counted once");
    Check(LogCount("frame check: 2 frames handed to the output in one game frame (Blt(colorfill), BltFast)") == 1,
          "logged with how they were handed over");
    core::diag::GameFrameBegin(); // nested begin/end (e.g. a draw-only frame inside the pass): one game frame
    core::diag::NotePresent("BltFast");
    core::diag::GameFrameBegin();
    core::diag::NotePresent("BltFast");
    core::diag::GameFrameEnd();
    core::diag::GameFrameEnd();
    core::diag::GameFrameEnd(); // one end too many: ignored
    core::diag::TakeIntegrity(&multi, &torn);
    Check(multi == 1, "nested begin/end: one game frame with 2 presents");
    for (int i = 0; i < 20; ++i) {
        Frame(3);
    }
    core::diag::TakeIntegrity(&multi, &torn);
    Check(multi == 20, "20 more such frames counted");
    Check(LogCount("frame check: ") == 10 && LogCount("further ones: counted in the rate line") == 1,
          "the log stops after 10 lines (the last one says so)");

    // Torn output frames.
    core::diag::NoteTornFrame(4, 6, 1920, 1080);
    core::diag::NoteTornFrame(5, 6, 1920, 1080);
    core::diag::TakeIntegrity(&multi, &torn);
    Check(torn == 2 && multi == 0, "torn output frames counted (and the multi count was reset)");
    Check(LogCount("the output drew a copy that changed while it was drawn (1920x1080, sequence 4 -> 6)") == 1 &&
              LogCount("sequence 5 -> 6, it was being written when taken") == 1,
          "torn frames logged, the odd sequence noted");
    for (int i = 0; i < 30; ++i) {
        core::diag::NoteTornFrame(8, 10, 800, 600);
    }
    core::diag::TakeIntegrity(&multi, &torn);
    Check(torn == 30 && LogCount("the output drew a copy") == 10, "torn: counted on, at most 10 log lines");

    // Window events.
    core::diag::LogWindowEvent(WM_ACTIVATEAPP, 0, 0);
    core::diag::LogWindowEvent(WM_ACTIVATEAPP, 1, 0);
    core::diag::LogWindowEvent(WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 1), 0);
    core::diag::LogWindowEvent(WM_KILLFOCUS, 0, 0);
    core::diag::LogWindowEvent(WM_SETFOCUS, 0, 0);
    core::diag::LogWindowEvent(WM_SIZE, SIZE_MINIMIZED, 0);
    core::diag::LogWindowEvent(WM_SIZE, SIZE_RESTORED, MAKELPARAM(2560, 1440));
    core::diag::LogWindowEvent(WM_DISPLAYCHANGE, 32, MAKELPARAM(2560, 1440));
    core::diag::LogWindowEvent(WM_MOUSEMOVE, 0, 0);
    core::diag::LogWindowEvent(WM_SIZE, 4 /* SIZE_MAXHIDE */, 0);
    Check(LogCount("window: WM_ACTIVATEAPP inactive (switched away)") == 1 &&
              LogCount("window: WM_ACTIVATEAPP active (switched to the game)") == 1,
          "alt-tab away and back logged");
    Check(LogCount("window: WM_ACTIVATE inactive, minimised") == 1 && LogCount("window: WM_KILLFOCUS") == 1 &&
              LogCount("window: WM_SETFOCUS") == 1,
          "activation and focus logged");
    Check(LogCount("window: WM_SIZE minimised") == 1 && LogCount("window: WM_SIZE restored 2560x1440") == 1 &&
              LogCount("window: WM_DISPLAYCHANGE 2560x1440, 32 bits") == 1,
          "minimise / restore and display changes logged");
    Check(LogCount("window: ") == 8, "other messages are not logged");

    // Reports.
    Check(core::diag::TakeReportRequest() == 0, "no report request at first");
    int a = core::diag::BeginReport("report key");
    int b = core::diag::BeginReport("report key");
    Check(a == 1 && b == 2 && LogCount("==== REPORT #2 (report key) ====") == 1, "reports numbered 1, 2, logged");
    Check(core::diag::TakeReportRequest() == 2 && core::diag::TakeReportRequest() == 0,
          "the game thread takes the latest request once");

    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
