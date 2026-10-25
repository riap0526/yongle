#include "core/ddraw/surface_memory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

namespace core::ddraw {

namespace {

std::atomic<int> g_allowRows{0};

std::size_t PageSize()
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}

// The pattern: a different 32-bit value at every offset, so that no run of pixels (or zeros) matches it by chance.
inline std::uint32_t Canary(std::size_t offset)
{
    return 0x5ac3f00du ^ static_cast<std::uint32_t>(offset * 0x9e3779b1u);
}

void Fill(const SurfaceMemory &m)
{
    auto *p = reinterpret_cast<std::uint32_t *>(m.Spare());
    for (std::size_t i = 0; i < m.spareBytes / 4; ++i) {
        p[i] = Canary(i);
    }
}

} // namespace

bool AllocSurfaceMemory(SurfaceMemory &m, int pitch, int rows, int spareRows)
{
    m = SurfaceMemory{};
    if (pitch <= 0 || pitch % 4 != 0 || rows <= 0 || spareRows < 0) {
        return false;
    }
    const std::size_t page = PageSize();
    const std::size_t used = static_cast<std::size_t>(pitch) * (static_cast<std::size_t>(rows) + spareRows);
    const std::size_t body = (used + page - 1) / page * page;
    const std::size_t size = page + body + page;
    auto *base = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
    if (!base) {
        return false;
    }
    if (!VirtualAlloc(base + page, body, MEM_COMMIT, PAGE_READWRITE)) { // the guard pages stay reserved only
        VirtualFree(base, 0, MEM_RELEASE);
        return false;
    }
    m.base = base;
    m.size = size;
    m.bits = base + page;
    m.pitch = pitch;
    m.rows = rows;
    m.spareBytes = body - static_cast<std::size_t>(pitch) * rows;
    Fill(m);
    return true;
}

void FreeSurfaceMemory(SurfaceMemory &m)
{
    if (m.base) {
        VirtualFree(m.base, 0, MEM_RELEASE);
    }
    m = SurfaceMemory{};
}

bool CheckCanary(const SurfaceMemory &m, CanaryDamage *damage)
{
    if (!m.bits) {
        return true;
    }
    const auto *p = reinterpret_cast<const std::uint32_t *>(m.Spare());
    const std::size_t words = m.spareBytes / 4;
    const int allow = g_allowRows.load();
    std::size_t start = allow > 0 ? static_cast<std::size_t>(allow) * m.pitch / 4 : 0;
    if (start > words) {
        start = words;
    }
    std::size_t first = words, last = 0, bytes = 0;
    for (std::size_t i = start; i < words; ++i) {
        std::uint32_t diff = p[i] ^ Canary(i);
        if (diff) {
            if (first == words) {
                first = i;
            }
            last = i;
            for (; diff; diff >>= 8) {
                bytes += (diff & 0xff) != 0;
            }
        }
    }
    if (first == words) {
        return true;
    }
    if (damage) {
        damage->firstRow = m.rows + static_cast<int>(first * 4 / m.pitch);
        damage->lastRow = m.rows + static_cast<int>(last * 4 / m.pitch);
        damage->bytes = bytes;
    }
    Fill(m);
    return false;
}

namespace {
StateDescriber g_describe = nullptr;
}

void SetCanaryAllowance(int rows)
{
    g_allowRows.store(rows > 0 ? rows : 0);
}

int CanaryAllowance()
{
    return g_allowRows.load();
}

void SetStateDescriber(StateDescriber describe)
{
    g_describe = describe;
}

void DescribeState(char *out, std::size_t size)
{
    if (!size) {
        return;
    }
    out[0] = 0;
    if (g_describe) {
        g_describe(out, size);
    }
}

} // namespace core::ddraw
