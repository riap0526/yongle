// Exception log and feature guards (CLAUDE.md: a failing feature of ours is switched off and logged; the game runs on).
//
// Exception log: a vectored exception handler that only records. For access violations and the other hardware
// exceptions, and for C++ exceptions (the game's own throw), it logs the code, the faulting address as module+offset
// (marked when it is in our DLL), the access (read/write and address), the registers, the frame-pointer chain and
// the code addresses found on the top of the stack. Then it lets the exception go on as before (the game's own
// handlers still run), unless the exception happened inside a Guarded call (below). First-chance: some of these may be
// handled by the game itself; the log says so when it is not ours to recover. At most 40 hardware and 20 C++
// exceptions are logged.
//
// Feature guards: Guarded(feature, fn, arg) runs fn on this thread; a hardware exception inside it (in our code or in
// a game function it called) abandons the call -- the stack and the SEH chain are put back to where Guarded was
// entered -- and the feature is switched off for the rest of the run (logged once). Destructors of the abandoned
// frames do not run (a little memory may leak). Callers check Enabled(feature) and fall back to the game's own
// behaviour. Nothing here knows a game.
//
// Game state: what the abandoned call changed in the game's memory is not put back by the stack alone. A guarded
// call records each change in a Restore list (the old value is kept before it writes); when the call is abandoned,
// Guarded undoes the list (newest first) before the feature is switched off, so the fallback starts from the
// game's own values. A call that completes normally undoes the list itself where the change was only for the
// call's duration (e.g. a moved origin), or keeps it (Forget) where the change is the call's result.
//
// The handler is a vectored one, so it acts before the game's own SEH handlers: a hardware exception inside a
// guarded call is taken by the guard even if a game function further down would have caught it (VC6 catch(...)
// also catches hardware exceptions). Inside our guarded calls that is the intent: the fault comes from the state we
// set up, and the game's handler would carry on with that state.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace core::diag {

struct Feature {
    const char *name;
    std::atomic<bool> off{false};
};

// From start-up (DllMain): kernel32 only. Also loads the symbol table next to our module (LoadSymbols with
// <module>.syms: ddraw.dll -> ddraw.syms).
void InstallExceptionLog();

// Symbol table (tools/build/make_syms.py): from then on, addresses in our module are logged with the function and,
// in our own sources, the file and line ("... [OUR DLL] = Function+0x12 (src\games\emperor\x.cpp:123)"). The
// table's build id must equal the loaded module's .buildid section, so a table from another build is not used.
// False (logged) when the file is missing or does not fit; the log then has module+offset only, as before.
// Start-up only (InstallExceptionLog calls it; the offline test calls it again with other files).
bool LoadSymbols(const wchar_t *path);

inline bool Enabled(const Feature &f)
{
    return !f.off.load();
}

// Switches the feature off (logged once with the reason).
void Disable(Feature &f, const char *why);

// Logs a stack copied from another thread (core/diag/stall: suspended, copied, resumed): eip, the frame-pointer chain
// and the code addresses on its top that follow a call, as module+offset with our symbols. copy holds size bytes read
// from esp up. Any thread but the one the copy is of.
void LogCapturedStack(const char *label, unsigned long eip, unsigned long esp, unsigned long ebp,
                      const unsigned char *copy, unsigned long size);

// Values in the game's memory (or ours) that a guarded call changes: 1, 2 or 4 bytes each, at most kMax.
class Restore {
public:
    static constexpr int kMax = 16;

    // Remembers *p, then writes v. False (nothing written, logged) when the list is full.
    template <typename T> bool Set(volatile T *p, std::type_identity_t<T> v)
    {
        if (!Save(p)) {
            return false;
        }
        *p = v;
        return true;
    }
    // Only remembers *p (something else -- the game, or our code later -- will change it). False when full.
    template <typename T> bool Save(volatile T *p)
    {
        static_assert(sizeof(T) <= 4 && std::is_trivially_copyable_v<T>, "1, 2 or 4 byte values");
        if (n_ >= kMax) {
            Full();
            return false;
        }
        T old = *p;
        Entry &e = e_[n_++];
        e.p = p;
        e.put = &Put<T>;
        e.old = 0;
        std::memcpy(&e.old, &old, sizeof(T));
        return true;
    }
    // Writes every remembered value back, newest first, and empties the list. Returns how many.
    int Undo();
    // Empties the list without writing anything back (the changes are kept).
    void Forget()
    {
        n_ = 0;
    }
    int Count() const
    {
        return n_;
    }

private:
    struct Entry {
        volatile void *p;
        void (*put)(volatile void *, std::uint32_t);
        std::uint32_t old;
    };
    template <typename T> static void Put(volatile void *p, std::uint32_t v)
    {
        T t;
        std::memcpy(&t, &v, sizeof(T));
        *static_cast<volatile T *>(p) = t;
    }
    static void Full();

    Entry e_[kMax];
    int n_ = 0;
};

// Runs fn(arg) under the guard; false when the feature is off or fn was abandoned after an exception. When it was
// abandoned and undo is given, undo->Undo() runs first (the game state goes back to what it was), then the feature
// is switched off.
bool Guarded(Feature &f, void (*fn)(void *), void *arg, Restore *undo = nullptr);

// The features switched off so far in this run, e.g. "smooth scrolling (games/emperor/scroll), ..." ("" when none).
// Any thread.
void SwitchedOff(char *out, size_t size);

// Convenience for a lambda without captures or with a context pointer: Guarded(f, [](void *p) { ... }, &ctx).

} // namespace core::diag
