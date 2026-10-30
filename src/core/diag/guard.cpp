#include "core/diag/guard.h"

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace core::diag {

namespace {

constexpr long kMaxHardware = 40, kMaxCpp = 20; // logged per run
std::atomic<long> g_loggedHardware{0}, g_loggedCpp{0};
HMODULE g_self = nullptr;
constexpr unsigned kWhereSize = 1024; // module+offset, and the function and source line from the symbol table

// One Guarded call in progress on a thread (they may nest).
struct GuardFrame {
    void *jump[5];        // __builtin_setjmp buffer
    void *seh;            // the SEH chain head when Guarded was entered
    GuardFrame *prev;
    Feature *feature;
    Restore *undo;        // put back before the feature is switched off
    DWORD code;           // the exception that abandoned the call
    void *address;
};
thread_local GuardFrame *t_guard = nullptr;

bool Hardware(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return true;
    default:
        return false;
    }
}

constexpr DWORD kCppException = 0xE06D7363; // MSVC (the game's VC6 runtime) throw

bool Readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        return false;
    }
    return static_cast<const BYTE *>(p) + n <= static_cast<const BYTE *>(mbi.BaseAddress) + mbi.RegionSize;
}

bool InExecutableModule(DWORD a)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void *>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        mbi.Type != MEM_IMAGE) {
        return false;
    }
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Symbol table of our module (<module>.syms, made by tools/build/make_syms.py at build time): for an address in our
// code, the function (the nearest symbol at or before it) and, in our own sources, the file and line. Read at
// start-up; read-only afterwards.
struct SymRow {
    DWORD rva;
    int name, file, line; // file -1: no source line (a function start, or code that is not ours)
};
struct Symbols {
    const char **files, **names;
    SymRow *rows;
    int fileCount, nameCount, rowCount;
    DWORD textBegin, textEnd; // RVAs of the module's .text
};
std::atomic<const Symbols *> g_syms{nullptr};

const IMAGE_SECTION_HEADER *Section(HMODULE m, const char *name)
{
    auto *base = reinterpret_cast<const BYTE *>(m);
    auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(m)->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (std::strncmp(reinterpret_cast<const char *>(s->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0) {
            return s;
        }
    }
    return nullptr;
}

// The module's .buildid section in hex, as make_syms.py writes it ("" when there is none). Bytes 24-27, the debug
// directory's PointerToRawData (a file offset that llvm-strip changes), are written as 0 on both sides: the table is
// made from ddraw.dbg.dll, before ddraw.dll is stripped (found in O1's second test, 2026-10-28).
void BuildId(HMODULE m, char *out, size_t size)
{
    out[0] = 0;
    const IMAGE_SECTION_HEADER *s = Section(m, ".buildid");
    if (!s) {
        return;
    }
    DWORD n = s->Misc.VirtualSize < s->SizeOfRawData ? s->Misc.VirtualSize : s->SizeOfRawData;
    auto *p = reinterpret_cast<const BYTE *>(m) + s->VirtualAddress;
    static const char kHex[] = "0123456789abcdef";
    size_t o = 0;
    for (DWORD i = 0; i < n && o + 2 < size; ++i) {
        const BYTE v = i >= 24 && i < 28 ? 0 : p[i];
        out[o++] = kHex[v >> 4];
        out[o++] = kHex[v & 15];
    }
    out[o] = 0;
}

// A number (base 10 with an optional '-', or base 16) followed by a space or the end of the line.
bool Number(char *&p, int base, long long &v)
{
    bool neg = base == 10 && *p == '-';
    p += neg;
    long long r = 0;
    int digits = 0;
    for (;; ++p, ++digits) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : base == 16 && *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
        if (d < 0 || r > 0xffffffffLL) {
            break;
        }
        r = r * base + d;
    }
    if (!digits || (*p != ' ' && *p != 0)) {
        return false;
    }
    p += *p == ' ';
    v = neg ? -r : r;
    return true;
}

// Parses the table in buf (size bytes and a 0 after them; split into lines in place, the names point into it).
// The arrays go into one VirtualAlloc block. Null (and why) when it does not fit this module.
const Symbols *ParseSymbols(HMODULE m, char *buf, DWORD size, const char *&why)
{
    // Lines, 0-terminated in place ("\r\n" too); counted by their first character.
    int counts[128] = {};
    bool lineStart = false; // the header line is not counted
    for (char *p = buf; p < buf + size; ++p) {
        if (*p == '\n' || *p == '\r') {
            *p = 0;
            lineStart = true;
        }
        else if (lineStart) {
            counts[*p & 127]++;
            lineStart = false;
        }
    }
    char id[160];
    BuildId(m, id, sizeof(id));
    if (std::strncmp(buf, "EMPEROR-SYMS 1 ", 15) != 0) {
        why = "not a symbol table (EMPEROR-SYMS 1)";
        return nullptr;
    }
    if (!id[0] || std::strcmp(buf + 15, id) != 0) {
        why = id[0] ? "its build id differs from the loaded module (a table from another build)"
                    : "the module has no .buildid section";
        return nullptr;
    }
    const IMAGE_SECTION_HEADER *text = Section(m, ".text");
    if (!text || !counts['L']) {
        why = text ? "no rows" : "the module has no .text section";
        return nullptr;
    }
    size_t bytes = sizeof(Symbols) + (counts['F'] + counts['N']) * sizeof(char *) + counts['L'] * sizeof(SymRow);
    auto *block = static_cast<BYTE *>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!block) {
        why = "out of memory";
        return nullptr;
    }
    auto *t = reinterpret_cast<Symbols *>(block);
    t->files = reinterpret_cast<const char **>(block + sizeof(Symbols));
    t->names = t->files + counts['F'];
    t->rows = reinterpret_cast<SymRow *>(t->names + counts['N']);
    why = nullptr;
    for (char *p = buf + std::strlen(buf) + 1; p < buf + size && !why; p += std::strlen(p) + 1) {
        char kind = *p; // 0: the second half of a CR LF, or an empty line
        char *q = p + (kind && p[1] == ' ' ? 2 : 1);
        long long a, b, c, d;
        if (kind == 'F' || kind == 'N') {
            int &n = kind == 'F' ? t->fileCount : t->nameCount;
            if (!Number(q, 10, a) || a != n) {
                why = "a file or name out of order";
            }
            else {
                (kind == 'F' ? t->files : t->names)[n++] = q;
            }
        }
        else if (kind == 'L') {
            if (!Number(q, 16, a) || !Number(q, 10, b) || !Number(q, 10, c) || !Number(q, 10, d) || *q ||
                (t->rowCount && static_cast<DWORD>(a) < t->rows[t->rowCount - 1].rva)) {
                why = "a malformed or unsorted row";
            }
            else {
                t->rows[t->rowCount++] = {static_cast<DWORD>(a), static_cast<int>(b), static_cast<int>(c),
                                          static_cast<int>(d)};
            }
        }
        else if (kind) {
            why = "an unknown line";
        }
    }
    for (int i = 0; !why && i < t->rowCount; ++i) {
        const SymRow &r = t->rows[i];
        if (r.name < 0 || r.name >= t->nameCount || r.file < -1 || r.file >= t->fileCount) {
            why = "a row with an index out of range";
        }
    }
    if (why) {
        VirtualFree(block, 0, MEM_RELEASE);
        return nullptr;
    }
    t->textBegin = text->VirtualAddress;
    t->textEnd = text->VirtualAddress + text->Misc.VirtualSize;
    return t;
}

// Appends " = Function+0x12 (src\file.cpp:123)" for an address in our code (nothing without a table). A return
// address is looked up one byte back: the line of the call, not of the code after it.
void AppendSymbol(const void *a, bool returnAddress, char *buf, unsigned size)
{
    const Symbols *t = g_syms.load();
    if (!t) {
        return;
    }
    DWORD at = reinterpret_cast<DWORD>(a) - reinterpret_cast<DWORD>(g_self);
    DWORD rva = at - (returnAddress ? 1 : 0);
    if (rva < t->textBegin || rva >= t->textEnd || rva < t->rows[0].rva) {
        return;
    }
    int lo = 0, hi = t->rowCount - 1; // the last row at or before rva
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (t->rows[mid].rva <= rva) {
            lo = mid;
        }
        else {
            hi = mid - 1;
        }
    }
    const SymRow &r = t->rows[lo];
    int start = lo; // the function's first row (make_syms writes one with file -1 where each function starts)
    while (start > 0 && t->rows[start].file >= 0 && t->rows[start - 1].name == r.name) {
        --start;
    }
    size_t n = std::strlen(buf);
    unsigned long off = at - t->rows[start].rva;
    if (r.file >= 0) {
        std::snprintf(buf + n, size - n, " = %s+0x%lx (%s:%d)", t->names[r.name], off, t->files[r.file], r.line);
    }
    else {
        std::snprintf(buf + n, size - n, " = %s+0x%lx", t->names[r.name], off);
    }
}

const char *Where(const void *a, char *buf, unsigned size, bool returnAddress = false)
{
    core_describe_address(a, buf, size);
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(a), &m) &&
        m == g_self) {
        std::strncat(buf, " [OUR DLL]", size - std::strlen(buf) - 1);
        AppendSymbol(a, returnAddress, buf, size);
    }
    return buf;
}

void LogException(EXCEPTION_POINTERS *ep, GuardFrame *guard)
{
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    const CONTEXT *c = ep->ContextRecord;
    char where[kWhereSize];
    const char *kind = r->ExceptionCode == kCppException ? "C++ exception (throw)" : "exception";
    core_log("EXCEPTION 0x%08lx (%s) at %s, thread %lu%s", r->ExceptionCode, kind,
             Where(r->ExceptionAddress, where, sizeof(where)), GetCurrentThreadId(),
             guard ? "; inside a guarded feature of ours, which is abandoned" : "; first chance, passed on");
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        static const char *const kAccess[] = {"read", "write", "?", "?", "?", "?", "?", "?", "execute"};
        ULONG_PTR op = r->ExceptionInformation[0];
        core_log("  access: %s of 0x%08lx", op < 9 ? kAccess[op] : "?",
                 static_cast<unsigned long>(r->ExceptionInformation[1]));
    }
    core_log("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx eip=%08lx", c->Eax,
             c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip);
    // Frame-pointer chain (the game and most of our code keep frame pointers).
    NT_TIB *tib = reinterpret_cast<NT_TIB *>(NtCurrentTeb());
    DWORD ebp = c->Ebp;
    for (int i = 0; i < 10; ++i) {
        if (ebp < reinterpret_cast<DWORD>(tib->StackLimit) || ebp + 8 > reinterpret_cast<DWORD>(tib->StackBase) ||
            !Readable(reinterpret_cast<void *>(ebp), 8)) {
            break;
        }
        DWORD ret = reinterpret_cast<DWORD *>(ebp)[1];
        core_log("  frame %d: return to %s", i, Where(reinterpret_cast<void *>(ret), where, sizeof(where), true));
        DWORD next = reinterpret_cast<DWORD *>(ebp)[0];
        if (next <= ebp) {
            break;
        }
        ebp = next;
    }
    // Code addresses on the top of the stack (also when frame pointers are missing).
    DWORD esp = c->Esp;
    int shown = 0;
    for (int i = 0; i < 96 && shown < 12; ++i) {
        DWORD *slot = reinterpret_cast<DWORD *>(esp) + i;
        if (reinterpret_cast<DWORD>(slot) + 4 > reinterpret_cast<DWORD>(tib->StackBase) || !Readable(slot, 4)) {
            break;
        }
        if (InExecutableModule(*slot)) {
            core_log("  stack [esp+0x%03x]: %s", i * 4,
                     Where(reinterpret_cast<void *>(*slot), where, sizeof(where), true));
            ++shown;
        }
    }
}

// A code address that a call returns to: the bytes before it are a call (E8 rel32, or FF /2 with a ModRM of 1-6
// bytes). Fewer false addresses than "in an executable module" alone when the whole top of a stack is scanned.
bool FollowsCall(DWORD a)
{
    const BYTE *p = reinterpret_cast<const BYTE *>(a);
    if (!Readable(p - 7, 7)) {
        return false;
    }
    if (p[-5] == 0xE8) {
        return true;
    }
    static const int kBack[] = {2, 3, 4, 6, 7};
    for (int k : kBack) {
        if (p[-k] == 0xFF && ((p[-k + 1] >> 3) & 7) == 2) {
            return true;
        }
    }
    return false;
}

} // namespace

void LogCapturedStack(const char *label, unsigned long eip, unsigned long esp, unsigned long ebp,
                      const unsigned char *copy, unsigned long size)
{
    char where[kWhereSize];
    core_log("  %s: eip %s; esp %08lx ebp %08lx", label, Where(reinterpret_cast<void *>(eip), where, sizeof(where)),
             esp, ebp);
    auto at = [&](unsigned long addr) { return *reinterpret_cast<const DWORD *>(copy + (addr - esp)); };
    // 1. The frame-pointer chain, inside the copy: exact where every function on the way keeps ebp as its frame
    // pointer (the game's VC6 code does); it goes wrong or stops at the first one that does not (system DLLs,
    // drivers and overlays are often built without frame pointers).
    for (int i = 0; i < 12; ++i) {
        if (ebp < esp || ebp + 8 > esp + size) {
            break;
        }
        DWORD ret = at(ebp + 4);
        core_log("  %s frame %d (ebp chain): return to %s", label, i,
                 Where(reinterpret_cast<void *>(ret), where, sizeof(where), true));
        DWORD next = at(ebp);
        if (next <= ebp) {
            break;
        }
        ebp = next;
    }
    // 2. A scan of the copy from the top: every value that is a code address in a loaded module and follows a call
    // instruction. Works without frame pointers, but is a guess: a value can be left over from an earlier call, or be
    // data that happens to look like one; the real callers are among them, in order, nearest first.
    int shown = 0;
    for (unsigned long o = 0; o + 4 <= size && shown < 16; o += 4) {
        DWORD v = *reinterpret_cast<const DWORD *>(copy + o);
        if (InExecutableModule(v) && FollowsCall(v)) {
            core_log("  %s stack scan [esp+0x%04lx] (a guess): %s", label, o,
                     Where(reinterpret_cast<void *>(v), where, sizeof(where), true));
            ++shown;
        }
    }
}

namespace {

// Runs on the faulting thread after the handler moved it here: puts the SEH chain back and jumps to Guarded.
[[noreturn]] void __cdecl Recover()
{
    GuardFrame *g = t_guard;
    reinterpret_cast<NT_TIB *>(NtCurrentTeb())->ExceptionList = static_cast<EXCEPTION_REGISTRATION_RECORD *>(g->seh);
    __builtin_longjmp(g->jump, 1);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS *ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool hardware = Hardware(code);
    if (!hardware && code != kCppException) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    GuardFrame *g = hardware ? t_guard : nullptr;
    std::atomic<long> &count = hardware ? g_loggedHardware : g_loggedCpp;
    if (g || count.fetch_add(1) < (hardware ? kMaxHardware : kMaxCpp)) {
        LogException(ep, g);
    }
    if (g && code != EXCEPTION_STACK_OVERFLOW) {
        g->code = code;
        g->address = ep->ExceptionRecord->ExceptionAddress;
        ep->ContextRecord->Eip = reinterpret_cast<DWORD>(&Recover);
        ep->ContextRecord->Esp &= ~static_cast<DWORD>(15); // a sane alignment for Recover's own frame
        ep->ContextRecord->Esp -= 64;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool LoadSymbols(const wchar_t *path)
{
    if (!g_self) {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&LoadSymbols), &g_self);
    }
    const wchar_t *name = path;
    for (const wchar_t *p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            name = p + 1;
        }
    }
    const char *why = nullptr;
    const Symbols *t = nullptr;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        why = "not found";
    }
    else {
        DWORD size = GetFileSize(f, nullptr), got = 0;
        char *buf = size != INVALID_FILE_SIZE && size < (64u << 20)
                        ? static_cast<char *>(VirtualAlloc(nullptr, size + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
                        : nullptr;
        if (!buf) {
            why = "too large, or out of memory";
        }
        else if (!ReadFile(f, buf, size, &got, nullptr) || got != size) {
            why = "read failed";
        }
        else {
            buf[size] = 0;
            t = ParseSymbols(g_self, buf, size, why); // the names point into buf, which stays
        }
        if (!t && buf) {
            VirtualFree(buf, 0, MEM_RELEASE);
        }
        CloseHandle(f);
    }
    if (!t) {
        core_log("diag: symbol table %ls not used: %s; addresses in our DLL are logged as module+offset only", name,
                 why);
        return false;
    }
    g_syms.store(t); // a table loaded before stays allocated: nothing is freed that a log line may be reading
    core_log("diag: symbol table %ls: %d functions, %d source files, %d rows", name, t->nameCount, t->fileCount,
             t->rowCount);
    return true;
}

void InstallExceptionLog()
{
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallExceptionLog), &g_self);
    // <module>.syms next to the module: ddraw.dll -> ddraw.syms.
    wchar_t path[MAX_PATH + 8];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t *dot = nullptr;
        for (wchar_t *p = path; *p; ++p) {
            dot = *p == L'.' ? p : *p == L'\\' ? nullptr : dot;
        }
        std::wcscpy(dot ? dot : path + n, L".syms");
        LoadSymbols(path);
    }
    if (AddVectoredExceptionHandler(1, &Handler)) {
        core_log("diag: exception log installed (at most %ld hardware and %ld C++ exceptions are logged; guarded "
                 "features of ours are switched off on an exception)",
                 kMaxHardware, kMaxCpp);
    }
    else {
        core_log("diag: AddVectoredExceptionHandler failed (%lu)", GetLastError());
    }
}

namespace {
constexpr long kMaxOff = 32;
const char *g_off[kMaxOff];
std::atomic<long> g_offCount{0};
} // namespace

void Disable(Feature &f, const char *why)
{
    if (!f.off.exchange(true)) {
        core_log("diag: feature \"%s\" switched off: %s; the game goes on without it", f.name, why);
        long i = g_offCount.load();
        if (i < kMaxOff) {
            g_off[i] = f.name;
            g_offCount.store(i + 1); // published after the name is in place
        }
    }
}

void SwitchedOff(char *out, size_t size)
{
    if (!size) {
        return;
    }
    out[0] = 0;
    size_t n = 0;
    long count = g_offCount.load();
    for (long i = 0; i < count && n + 1 < size; ++i) {
        int w = std::snprintf(out + n, size - n, "%s%s", i ? ", " : "", g_off[i]);
        if (w < 0) {
            break;
        }
        n += static_cast<size_t>(w);
    }
}

int Restore::Undo()
{
    int undone = n_;
    while (n_ > 0) {
        Entry &e = e_[--n_];
        e.put(e.p, e.old);
    }
    return undone;
}

void Restore::Full()
{
    static std::atomic<long> logged{0};
    if (logged.fetch_add(1) < 4) {
        core_log("diag: a restore list is full (%d values); the change was not made", kMax);
    }
}

bool Guarded(Feature &f, void (*fn)(void *), void *arg, Restore *undo)
{
    if (f.off.load()) {
        return false;
    }
    GuardFrame g{};
    g.prev = t_guard;
    g.feature = &f;
    g.undo = undo;
    g.seh = reinterpret_cast<NT_TIB *>(NtCurrentTeb())->ExceptionList;
    if (__builtin_setjmp(g.jump)) {
        GuardFrame *self = t_guard; // &g, as it was when the exception happened
        t_guard = self->prev;
        int restored = self->undo ? self->undo->Undo() : 0;
        char why[kWhereSize + 160], where[kWhereSize];
        std::snprintf(why, sizeof(why), "exception 0x%08lx at %s; %d changed value(s) put back", self->code,
                      Where(self->address, where, sizeof(where)), restored);
        Disable(*self->feature, why);
        return false;
    }
    t_guard = &g;
    fn(arg);
    t_guard = g.prev;
    return true;
}

} // namespace core::diag
