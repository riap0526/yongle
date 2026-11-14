// Which exe the game is (exe_identity.h).
#include "core/platform/exe_identity.h"

#include "core/game.h"
#include "core/log.h"
#include "core/output/sdl_output.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

// SHA-256 (FIPS 180-4).
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

struct Sha {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block[64];
    std::size_t used = 0;
    std::uint64_t bytes = 0;
};

inline std::uint32_t Rot(std::uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

void Compress(Sha &s, const unsigned char *p)
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t{p[4 * i]} << 24) | (std::uint32_t{p[4 * i + 1]} << 16) |
               (std::uint32_t{p[4 * i + 2]} << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = Rot(w[i - 15], 7) ^ Rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = Rot(w[i - 2], 17) ^ Rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t t1 = h + (Rot(e, 6) ^ Rot(e, 11) ^ Rot(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        const std::uint32_t t2 = (Rot(a, 2) ^ Rot(a, 13) ^ Rot(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s.h[0] += a;
    s.h[1] += b;
    s.h[2] += c;
    s.h[3] += d;
    s.h[4] += e;
    s.h[5] += f;
    s.h[6] += g;
    s.h[7] += h;
}

void Update(Sha &s, const unsigned char *p, std::size_t n)
{
    s.bytes += n;
    while (n > 0) {
        const std::size_t take = n < 64 - s.used ? n : 64 - s.used;
        std::memcpy(s.block + s.used, p, take);
        s.used += take;
        p += take;
        n -= take;
        if (s.used == 64) {
            Compress(s, s.block);
            s.used = 0;
        }
    }
}

void Finish(Sha &s, char hex[65])
{
    const std::uint64_t bits = s.bytes * 8;
    const unsigned char one = 0x80, zero = 0;
    Update(s, &one, 1);
    while (s.used != 56) {
        Update(s, &zero, 1);
    }
    unsigned char len[8];
    for (int i = 0; i < 8; ++i) {
        len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    }
    Update(s, len, 8);
    for (int i = 0; i < 8; ++i) {
        std::snprintf(hex + 8 * i, 9, "%08lx", static_cast<unsigned long>(s.h[i]));
    }
    hex[64] = 0;
}

bool g_tested = false;

// The exe's PE time stamp, from the loaded image (the exe is never relocated).
DWORD TimeStamp()
{
    const BYTE *base = reinterpret_cast<const BYTE *>(GetModuleHandleW(nullptr));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->FileHeader.TimeDateStamp : 0;
}

} // namespace

namespace core::exe {

void Sha256(const void *data, std::size_t size, char hex[65])
{
    Sha s;
    Update(s, static_cast<const unsigned char *>(data), size);
    Finish(s, hex);
}

bool Sha256File(const wchar_t *path, char hex[65], std::uint64_t *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    Sha s;
    static unsigned char buf[64 * 1024]; // start-up only, one thread
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(f, buf, sizeof(buf), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        Update(s, buf, got);
    }
    CloseHandle(f);
    if (!ok) {
        return false;
    }
    if (size) {
        *size = s.bytes;
    }
    Finish(s, hex);
    return true;
}

bool Check()
{
    const core_game_info *game = core_game();
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        core_log("exe: the exe's path is not known; its SHA-256 is not checked");
        return g_tested = false;
    }
    const wchar_t *name = path;
    for (const wchar_t *p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            name = p + 1;
        }
    }
    char nameUtf8[MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr)) {
        std::snprintf(nameUtf8, sizeof(nameUtf8), "the exe");
    }
    char hex[65];
    std::uint64_t size = 0;
    const DWORD t0 = GetTickCount();
    if (!Sha256File(path, hex, &size)) {
        core_log("exe: %s cannot be read (error %lu); its SHA-256 is not checked", nameUtf8, GetLastError());
        return g_tested = false;
    }
    core_log("exe: %s, %llu bytes, time stamp 0x%08lx, SHA-256 %s (%lu ms)", nameUtf8,
             static_cast<unsigned long long>(size), static_cast<unsigned long>(TimeStamp()), hex,
             static_cast<unsigned long>(GetTickCount() - t0));
    if (!game->exe_sha256 || !game->exe_sha256[0]) {
        return g_tested = true; // the game layer names no tested exe (offline tests)
    }
    g_tested = std::strcmp(hex, game->exe_sha256) == 0;
    if (g_tested) {
        core_log("exe: this is the %s (what %s is made and tested with)", game->exe_label, game->product);
        return true;
    }
    core_log("exe: not the %s that %s is made and tested with (its SHA-256 is %s). An exe changed by another tool (widescreen, "
             "resolution or animation fixes, other mods) or another release may keep some of %s's features from "
             "working, or make them work differently; the game's own checks below decide what is installed.",
             game->exe_label, game->product, game->exe_sha256, game->product);
    char notice[256];
    std::snprintf(notice, sizeof(notice), "Untested %s: %s was made for the %s. With a patched exe, some features may "
                  "not work.", nameUtf8, game->product, game->exe_label);
    core::output::SetNoticeFromFirstFrame(notice, 20000);
    return false;
}

bool Tested()
{
    return g_tested;
}

} // namespace core::exe

extern "C" int core_exe_check(void)
{
    return core::exe::Check() ? 1 : 0;
}
