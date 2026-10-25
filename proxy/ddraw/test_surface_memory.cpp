// Offline test of core/ddraw/surface_memory (not the game; R2b.1): the memory of an emulated surface has spare rows
// with a canary below the last row and guard pages before and after. For the game's sizes (and odd widths): the rows
// the game sees are all writable and leave the canary alone; a write into the spare rows is reported with the right
// rows and byte count, and the pattern is written again; the report sees pixels of any value (zeros, the pattern's
// neighbours); a write past the spare rows or before row 0 is an access violation (inside a guard, so the test goes
// on) and does not reach other memory; the memory is released.
#include "core/ddraw/surface_memory.h"
#include "core/diag/guard.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0, g_checks = 0;

void Check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) {
        std::printf("%-90s FAILED\n", what);
        ++g_failures;
    }
}

bool Committed(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE);
}

volatile std::uint8_t *g_target = nullptr;

// True when writing one byte at p faults (abandoned by the guard).
bool WriteFaults(std::uint8_t *p)
{
    core::diag::Feature probe{"probe"};
    g_target = p;
    return !core::diag::Guarded(probe, [](void *) { *g_target = 0x55; }, nullptr);
}

void Size(int w, int h)
{
    char what[160];
    const int pitch = (w * 2 + 3) & ~3; // as the emulated surface
    core::ddraw::SurfaceMemory m;
    std::snprintf(what, sizeof(what), "%dx%d: allocated", w, h);
    Check(core::ddraw::AllocSurfaceMemory(m, pitch, h) && m.bits && m.pitch == pitch && m.rows == h, what);
    if (!m.bits) {
        return;
    }
    const std::size_t frame = static_cast<std::size_t>(pitch) * h;
    std::snprintf(what, sizeof(what), "%dx%d: at least %d spare rows, all committed", w, h, core::ddraw::kSpareRows);
    Check(m.spareBytes >= static_cast<std::size_t>(pitch) * core::ddraw::kSpareRows && Committed(m.bits) &&
              Committed(m.bits + frame + m.spareBytes - 1),
          what);
    core::ddraw::CanaryDamage d;
    std::snprintf(what, sizeof(what), "%dx%d: canary intact after allocation", w, h);
    Check(core::ddraw::CheckCanary(m, &d), what);

    // Every row the game sees, every value: the canary does not notice.
    for (int y = 0; y < h; ++y) {
        std::memset(m.bits + static_cast<std::size_t>(y) * pitch, y & 1 ? 0x00 : 0xff, pitch);
    }
    std::snprintf(what, sizeof(what), "%dx%d: writing all %d rows (0x00 / 0xff) leaves the canary intact", w, h, h);
    Check(core::ddraw::CheckCanary(m, &d), what);

    // Ten rows past the end (a city clip 10 rows below the screen, as 1090 > 1080 in small-tile mode).
    std::memset(m.Spare(), 0, static_cast<std::size_t>(pitch) * 10);
    std::snprintf(what, sizeof(what), "%dx%d: 10 rows past the end reported as rows %d..%d", w, h, h, h + 9);
    Check(!core::ddraw::CheckCanary(m, &d) && d.firstRow == h && d.lastRow == h + 9 &&
              d.bytes >= static_cast<std::size_t>(pitch) * 10 * 9 / 10,
          what);
    std::snprintf(what, sizeof(what), "%dx%d: pattern written again after the report", w, h);
    Check(core::ddraw::CheckCanary(m, &d), what);

    // One byte in the last spare row, and one pixel in the middle.
    m.bits[frame + static_cast<std::size_t>(pitch) * (core::ddraw::kSpareRows - 1) + pitch / 2] ^= 0x01;
    std::snprintf(what, sizeof(what), "%dx%d: one bit in spare row %d reported (1 byte)", w, h,
                  core::ddraw::kSpareRows - 1);
    Check(!core::ddraw::CheckCanary(m, &d) && d.firstRow == h + core::ddraw::kSpareRows - 1 &&
              d.lastRow == d.firstRow && d.bytes == 1,
          what);
    // A pixel with the value the pattern has right next to it: still a change.
    auto *spare = reinterpret_cast<std::uint32_t *>(m.Spare());
    spare[20 * pitch / 4] = spare[20 * pitch / 4 - 1]; // the first word of spare row 20
    std::snprintf(what, sizeof(what), "%dx%d: a copy of the neighbouring pattern word is reported", w, h);
    Check(!core::ddraw::CheckCanary(m, &d) && d.firstRow == h + 20, what);

    // Past the spare area and before row 0: guard pages.
    std::snprintf(what, sizeof(what), "%dx%d: the byte after the spare area faults (guard page)", w, h);
    Check(WriteFaults(m.bits + frame + m.spareBytes), what);
    std::snprintf(what, sizeof(what), "%dx%d: the byte before row 0 faults (guard page)", w, h);
    Check(WriteFaults(m.bits - 1), what);
    std::snprintf(what, sizeof(what), "%dx%d: a fault in a guard page changed nothing in the canary", w, h);
    Check(core::ddraw::CheckCanary(m, &d), what);

    std::uint8_t *base = m.base;
    core::ddraw::FreeSurfaceMemory(m);
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(base, &mbi, sizeof(mbi));
    std::snprintf(what, sizeof(what), "%dx%d: released", w, h);
    Check(mbi.State == MEM_FREE && !m.bits, what);
}

} // namespace

int main()
{
    DeleteFileA("test_surface_memory.log");
    core_log_open(GetModuleHandleW(nullptr), "test_surface_memory.log");
    core::diag::InstallExceptionLog();

    const int sizes[][2] = {{640, 480},   {800, 600},   {1024, 768},  {1280, 1024}, {1920, 1080},
                            {2560, 1440}, {2560, 1080}, {1896, 1066}, {1550, 872},  {1382, 778},
                            {1366, 768},  {1, 1},       {3, 7},       {3840, 2160}};
    for (const auto &s : sizes) {
        Size(s[0], s[1]);
    }
    core::ddraw::SurfaceMemory bad;
    Check(!core::ddraw::AllocSurfaceMemory(bad, 6, 10) && !core::ddraw::AllocSurfaceMemory(bad, 0, 10) &&
              !core::ddraw::AllocSurfaceMemory(bad, 8, 0),
          "a pitch that is not a multiple of 4, or an empty size, is refused");
    { // Allowance (R2b.1-5): rows the game draws into on purpose are not reported; the rows below them are.
        core::ddraw::SurfaceMemory m;
        const int pitch = 1920 * 2, h = 1066;
        core::ddraw::CanaryDamage d;
        Check(core::ddraw::AllocSurfaceMemory(m, pitch, h), "allowance: allocated 1920x1066");
        core::ddraw::SetCanaryAllowance(14);
        std::memset(m.Spare(), 0x5a, static_cast<std::size_t>(pitch) * 14);
        Check(core::ddraw::CheckCanary(m, &d), "allowance 14: writes into the first 14 spare rows are not reported");
        std::memset(m.Spare() + static_cast<std::size_t>(pitch) * 14, 0x5a, 2);
        Check(!core::ddraw::CheckCanary(m, &d) && d.firstRow == h + 14 && d.lastRow == h + 14,
              "allowance 14: a write into spare row 14 is reported (row 1080)");
        core::ddraw::SetCanaryAllowance(1000);
        Check(core::ddraw::CanaryAllowance() == 1000 && core::ddraw::CheckCanary(m, &d),
              "an allowance larger than the spare area: nothing checked, nothing out of range");
        core::ddraw::SetCanaryAllowance(-3);
        Check(core::ddraw::CanaryAllowance() == 0, "a negative allowance counts as 0");
        core::ddraw::FreeSurfaceMemory(m);
    }
    char state[64];
    core::ddraw::DescribeState(state, sizeof(state));
    Check(state[0] == 0, "no describer: an empty state");
    core::ddraw::SetStateDescriber([](char *out, std::size_t size) { std::snprintf(out, size, "screen 0x01"); });
    core::ddraw::DescribeState(state, sizeof(state));
    Check(std::strcmp(state, "screen 0x01") == 0, "the game layer's describer is used");
    std::printf("%d checks\n", g_checks);
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
