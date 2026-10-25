// Offline test of core/diag/guard (not the game): the exception log and the feature guards. A guarded call that
// faults (null read, write to a read-only page, division by zero, a fault several frames down) is abandoned, its
// feature switched off and logged with module+offset, registers and stack; the thread goes on normally (another
// feature still runs, the stack is intact); a call that does not fault returns true; a switched-off feature does not
// run again. Restore lists: values of 1, 2 and 4 bytes (pointers too) are put back newest first; a full list
// refuses the change; Guarded puts the list back before the feature is switched off (logged with the count);
// nested guards each put back only their own list; Forget keeps the changes. Symbol table (test_guard.syms, made at
// build time): addresses in this exe are logged with the function and the source line (a return address with the
// line of the call); a table from another build or a missing one is not used (logged); CR LF line breaks load.
#include "core/diag/guard.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-66s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

bool LogContains(const char *text)
{
    FILE *f = std::fopen("test_guard.log", "rb");
    if (!f) {
        return false;
    }
    static char buf[1 << 20];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return std::strstr(buf, text) != nullptr;
}

volatile int *volatile g_null = nullptr;
volatile int g_zero = 0;
int g_ran = 0;
core::diag::Feature g_innerFeature{"inner"};

__attribute__((noinline)) void Deep(int n)
{
    volatile char pad[256];
    pad[0] = static_cast<char>(n);
    if (n == 0) {
        *g_null = pad[0];
    }
    else {
        Deep(n - 1);
    }
}

// Symbol table: the faulting line, and the line of the call a return address belongs to.
volatile int g_faultLine = 0, g_callLine = 0;

__attribute__((noinline)) void Fault()
{
    g_faultLine = __LINE__ + 1;
    *g_null = 1;
}

__attribute__((noinline)) void Caller(void *)
{
    g_callLine = __LINE__ + 1;
    Fault();
    ++g_ran; // not a tail call: the return address into Caller is on the stack
}

// A log line that contains both texts.
bool LogLine(const char *a, const char *b)
{
    FILE *f = std::fopen("test_guard.log", "rb");
    if (!f) {
        return false;
    }
    char line[4096];
    bool found = false;
    while (!found && std::fgets(line, sizeof(line), f)) {
        found = std::strstr(line, a) && std::strstr(line, b);
    }
    std::fclose(f);
    return found;
}

// test_guard.syms rewritten: the build id changed, or every line break as "\r\n".
bool WriteSymsVariant(const char *path, bool otherId)
{
    FILE *in = std::fopen("test_guard.syms", "rb");
    FILE *out = in ? std::fopen(path, "wb") : nullptr;
    if (!out) {
        if (in) {
            std::fclose(in);
        }
        return false;
    }
    int c, at = 0;
    while ((c = std::fgetc(in)) != EOF) {
        if (otherId && at == 15) {
            c = c == '0' ? '1' : '0'; // first digit of the build id
        }
        if (!otherId && c == '\n') {
            std::fputc('\r', out);
        }
        std::fputc(c, out);
        ++at;
    }
    std::fclose(in);
    std::fclose(out);
    return true;
}

} // namespace

int main()
{
    DeleteFileA("test_guard.log");
    core_log_open(GetModuleHandleW(nullptr), "test_guard.log");
    core::diag::InstallExceptionLog();

    core::diag::Feature ok{"ok"}, nullRead{"null read"}, roWrite{"read-only write"}, divZero{"divide"},
        deep{"deep"};
    Check(core::diag::Guarded(ok, [](void *) { ++g_ran; }, nullptr) && g_ran == 1, "a call without faults runs");
    Check(!core::diag::Guarded(nullRead, [](void *) { g_ran += *g_null; }, nullptr), "null read: abandoned");
    Check(!core::diag::Enabled(nullRead), "null read: feature switched off");
    Check(!core::diag::Guarded(roWrite,
                               [](void *) {
                                   auto *p = reinterpret_cast<volatile char *>(&Check);
                                   *p = 0; // code page: read-only
                               },
                               nullptr),
          "write to a read-only page: abandoned");
    Check(!core::diag::Guarded(divZero, [](void *) { g_ran += 10 / g_zero; }, nullptr), "division by zero: abandoned");
    Check(!core::diag::Guarded(deep, [](void *) { Deep(20); }, nullptr), "fault 21 frames down: abandoned");
    int before = g_ran;
    Check(core::diag::Guarded(ok, [](void *) { ++g_ran; }, nullptr) && g_ran == before + 1,
          "afterwards another feature still runs");
    Check(!core::diag::Guarded(nullRead, [](void *) { ++g_ran; }, nullptr) && g_ran == before + 1,
          "a switched-off feature does not run again");
    // Restore lists.
    {
        volatile int a = 1;
        volatile short b = 2;
        volatile char c = 3;
        char *volatile p = nullptr;
        core::diag::Restore r;
        r.Set(&a, 10);
        r.Set(&b, 20);
        r.Set(&c, 30);
        r.Set(&p, reinterpret_cast<char *>(&g_ran));
        r.Set(&a, 11); // changed twice: newest first gives back 1
        Check(a == 11 && b == 20 && c == 30 && p != nullptr && r.Count() == 5, "restore list: values changed");
        Check(r.Undo() == 5 && a == 1 && b == 2 && c == 3 && p == nullptr && r.Count() == 0,
              "restore list: 4, 2, 1 byte and pointer values back, newest first");
        for (int i = 0; i < core::diag::Restore::kMax; ++i) {
            r.Save(&a);
        }
        Check(!r.Set(&b, 99) && b == 2, "restore list: full -> the change is not made");
        r.Undo();
        r.Set(&a, 5);
        r.Forget();
        Check(a == 5 && r.Count() == 0 && r.Undo() == 0, "restore list: Forget keeps the change");
        a = 1;
    }
    {
        struct Ctx {
            volatile int x = 1, y = 2;
            core::diag::Restore r;
        } ctx;
        core::diag::Feature undoFeature{"undo"};
        Check(!core::diag::Guarded(
                  undoFeature,
                  [](void *p) {
                      auto &c = *static_cast<Ctx *>(p);
                      c.r.Set(&c.x, 100);
                      c.r.Set(&c.y, 200);
                      g_ran += *g_null;
                  },
                  &ctx, &ctx.r),
              "guarded call with a restore list: abandoned");
        Check(ctx.x == 1 && ctx.y == 2 && ctx.r.Count() == 0, "its changes put back");
        Check(LogContains("feature \"undo\" switched off: exception 0xc0000005") &&
                  LogContains("2 changed value(s) put back"),
              "log: switched off, with the number of values put back");
    }
    {
        struct Ctx {
            volatile int x = 1, y = 2;
            core::diag::Restore outer, inner;
            bool innerOk = true;
        } ctx;
        core::diag::Feature outerFeature{"outer"};
        bool ok = core::diag::Guarded(
            outerFeature,
            [](void *p) {
                auto &c = *static_cast<Ctx *>(p);
                c.outer.Set(&c.x, 10);
                c.innerOk = core::diag::Guarded(
                    g_innerFeature,
                    [](void *q) {
                        auto &d = *static_cast<Ctx *>(q);
                        d.inner.Set(&d.y, 20);
                        g_ran += *g_null;
                    },
                    &c, &c.inner);
                c.outer.Undo();
            },
            &ctx, &ctx.outer);
        Check(ok && !ctx.innerOk && ctx.x == 1 && ctx.y == 2 && core::diag::Enabled(outerFeature) &&
                  !core::diag::Enabled(g_innerFeature),
              "nested: the inner call put back its own value, the outer one completed");
    }

    volatile char big[4096];
    big[4095] = 7;
    Check(big[4095] == 7, "the stack is usable after the recoveries");

    // Symbol table (test_guard.syms next to the exe, loaded by InstallExceptionLog).
    {
        char text[160];
        core::diag::Feature symFeature{"symbols"};
        Check(LogContains("diag: symbol table test_guard.syms: "), "symbols: test_guard.syms loaded at install");
        Check(!core::diag::Guarded(symFeature, &Caller, nullptr), "symbols: a fault two calls down: abandoned");
        std::snprintf(text, sizeof(text), "(proxy\\ddraw\\test_guard.cpp:%d)", g_faultLine);
        Check(LogLine("EXCEPTION 0xc0000005", text) &&
                  LogLine("EXCEPTION 0xc0000005", "= (anonymous namespace)::Fault()+0x"),
              "symbols: faulting address -> Fault(), its source line");
        std::snprintf(text, sizeof(text), "(proxy\\ddraw\\test_guard.cpp:%d)", g_callLine);
        Check(LogLine("(anonymous namespace)::Caller(void*)+0x", text),
              "symbols: return address -> Caller(), the line of the call");
        Check(LogLine("feature \"symbols\" switched off", "= (anonymous namespace)::Fault()+0x"),
              "symbols: also in the switched-off line");
        Check(LogLine("Guarded(", "(src\\core\\diag\\guard.cpp:"), "symbols: frames in guard.cpp with their lines");

        Check(WriteSymsVariant("test_guard_otherid.syms", true) &&
                  !core::diag::LoadSymbols(L"test_guard_otherid.syms") &&
                  LogContains("test_guard_otherid.syms not used: its build id differs"),
              "symbols: a table from another build is not used");
        Check(!core::diag::LoadSymbols(L"test_guard_missing.syms") &&
                  LogContains("test_guard_missing.syms not used: not found"),
              "symbols: a missing table is logged");
        Check(WriteSymsVariant("test_guard_crlf.syms", false) && core::diag::LoadSymbols(L"test_guard_crlf.syms"),
              "symbols: the same table with CR LF line breaks loads");
        DeleteFileA("test_guard_otherid.syms");
        DeleteFileA("test_guard_crlf.syms");
        core::diag::Feature symFeature2{"symbols again"};
        core::diag::Guarded(symFeature2, &Caller, nullptr);
        std::snprintf(text, sizeof(text), "(proxy\\ddraw\\test_guard.cpp:%d)", g_faultLine);
        Check(LogLine("feature \"symbols again\" switched off", text),
              "symbols: the CR LF table in use (the rejected ones changed nothing)");
    }
    Check(LogContains("EXCEPTION 0xc0000005") && LogContains("access: read of 0x00000000") &&
              LogContains("access: write of"),
          "log: access violations with read/write and address");
    Check(LogContains("EXCEPTION 0xc0000094"), "log: division by zero");
    Check(LogContains("[OUR DLL]") || LogContains("test_guard.exe+0x"), "log: faulting address as module+offset");
    Check(LogContains("eax=") && LogContains("frame 0: return to"), "log: registers and frame chain");
    Check(LogContains("feature \"null read\" switched off") && LogContains("feature \"deep\" switched off"),
          "log: each feature switched off once, with the reason");
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
