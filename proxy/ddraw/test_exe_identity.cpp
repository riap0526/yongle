// Offline test of core/platform/exe_identity (no game, no DLL): SHA-256 on the standard test vectors and on a file,
// and Check() with the test's own exe as "the game": the tested exe gives no notice; another SHA-256 gives the
// caveat in the log and the notice from the first frame.
#include "core/game.h"
#include "core/log.h"
#include "core/platform/exe_identity.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace core::output { // what the check shows (stub)
std::string g_notice;
unsigned g_noticeMs = 0;
void SetNoticeFromFirstFrame(const char *utf8, unsigned ms)
{
    g_notice = utf8;
    g_noticeMs = ms;
}
} // namespace core::output

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("  %-70s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

bool LogContains(const char *text)
{
    FILE *f = std::fopen("test_exe_identity.log", "rb");
    if (!f) {
        return false;
    }
    static char buf[1 << 16];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return std::strstr(buf, text) != nullptr;
}

bool Vector(const char *text, std::size_t repeat, const char *want)
{
    std::string s;
    for (std::size_t i = 0; i < repeat; ++i) {
        s += text;
    }
    char hex[65];
    core::exe::Sha256(s.data(), s.size(), hex);
    return std::strcmp(hex, want) == 0;
}

core_game_info g_info = {"test", "Tester", "dev", L"Tester", L"test.ini", "test.ini", 1024, 768,
                         nullptr, nullptr, nullptr, nullptr, "English test.exe 1.0"};

} // namespace

int main()
{
    DeleteFileA("test_exe_identity.log");
    core_log_open(GetModuleHandleW(nullptr), "test_exe_identity.log");
    // FIPS 180-4 / NIST examples, and the lengths around one block's padding.
    Check(Vector("", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256 of nothing");
    Check(Vector("abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of abc");
    Check(Vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
          "SHA-256 of the 448-bit message");
    Check(Vector("a", 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
          "SHA-256 of a million a's");
    Check(Vector("a", 64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"), "SHA-256 of 64 a's");

    // A file, read in pieces (larger than the 64 KB buffer).
    const wchar_t *file = L"test_exe_identity.bin";
    std::string data(200000, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(i * 7 + 3);
    }
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD put = 0;
    WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr);
    CloseHandle(h);
    char a[65], b[65];
    std::uint64_t size = 0;
    core::exe::Sha256(data.data(), data.size(), a);
    Check(core::exe::Sha256File(file, b, &size) && std::strcmp(a, b) == 0 && size == data.size(),
          "a 200,000-byte file: the same SHA-256 as in memory, its size");
    DeleteFileW(file);
    Check(!core::exe::Sha256File(L"no_such_file.bin", b, &size), "a file that does not exist: false");

    // Check() with this test as the game.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    char mine[65];
    core::exe::Sha256File(self, mine, &size);
    g_info.exe_sha256 = mine;
    core_game_set(&g_info);
    Check(core::exe::Check() && core::exe::Tested() && core::output::g_notice.empty(),
          "the tested exe: true, no notice");
    Check(LogContains("SHA-256 ") && LogContains(mine) && LogContains("this is the English test.exe 1.0"),
          "log: its size, time stamp and SHA-256, and that it is the tested one");
    g_info.exe_sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    Check(!core::exe::Check() && !core::exe::Tested(), "another SHA-256: false");
    Check(core::output::g_notice == "Untested test_exe_identity.exe: Tester was made for the English test.exe 1.0. "
                                    "With a patched exe, some features may not work." &&
              core::output::g_noticeMs == 20000,
          "the notice, 20 s from the first frame");
    Check(LogContains("not the English test.exe 1.0 that Tester is made and tested with") &&
              LogContains("widescreen, resolution or animation fixes"),
          "log: not the tested exe, with the caveat");
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
