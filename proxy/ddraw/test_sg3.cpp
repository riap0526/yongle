// Offline test of core/assets/sg3 (not the game) on the player's own image pack: decodes images first..first+count-1
// and prints their sizes and an FNV-1a hash of all their RGB565 pixels (transparent = 0xF81F), to compare with
// tools/verify/sg3_hash.py (the Python reader the C++ one was ported from). Nothing is written.
// Usage: test_sg3.exe <pack.sg3> <first> <count> [expected hash]
#include "core/assets/sg3.h"
#include "core/log.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

int main(int argc, char **argv)
{
    if (argc < 4) {
        std::printf("usage: test_sg3 <pack.sg3> <first> <count> [expected hash]\n");
        return 2;
    }
    core_log_open(GetModuleHandleW(nullptr), "test_sg3.log");
    wchar_t path[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, path, MAX_PATH);
    core::assets::Sg3Pack pack;
    if (!pack.Open(path)) {
        std::printf("RESULT: FAIL (cannot open %s)\n", argv[1]);
        return 1;
    }
    const int first = std::atoi(argv[2]), count = std::atoi(argv[3]);
    unsigned hash = 2166136261u;
    int sizes[2] = {-1, -1};
    bool same = true;
    for (int i = first; i < first + count; ++i) {
        std::vector<WORD> px;
        int w = 0, h = 0;
        if (!pack.Size(i, &w, &h) || !pack.Decode565(i, 0xF81F, &px)) {
            std::printf("RESULT: FAIL (image %d)\n", i);
            return 1;
        }
        if (sizes[0] < 0) {
            sizes[0] = w;
            sizes[1] = h;
        }
        same = same && w == sizes[0] && h == sizes[1];
        for (WORD v : px) {
            hash = (hash ^ (v & 0xff)) * 16777619u;
            hash = (hash ^ (v >> 8)) * 16777619u;
        }
    }
    std::printf("%d images from %d: %s%dx%d, hash %08x\n", count, first, same ? "all " : "first ", sizes[0], sizes[1],
                hash);
    bool ok = argc < 5 || std::strtoul(argv[4], nullptr, 16) == hash;
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL (hash differs from the Python reader)");
    return ok ? 0 : 1;
}
