#include "core/settings/settings.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/settings/ini_text.h"
#include "core/ui/hotkey.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace core::settings {

namespace {

constexpr int kMaxItems = 48;
constexpr DWORD kMaxFileSize = 64 * 1024;

struct Slot {
    Item item;
    char value[kValueSize];   // in effect now
    char saved[kValueSize];   // confirmed: what the file gets (an unconfirmed re-layout value is not saved)
    char startup[kValueSize];
    Apply apply[4];
    int applyCount;
    bool notApplied; // the callback could not apply the current value: waits for a restart
    core::diag::Feature guard; // applying this item's changes (switched off when a callback faults)
    char guardName[80];
};

Slot g_items[kMaxItems];
int g_count = 0;
SRWLOCK g_lock = SRWLOCK_INIT; // values and the request queue
wchar_t g_path[MAX_PATH] = L"";

struct Pending {
    int index; // -1: save only
    char value[kValueSize];
    bool save;
};
std::vector<Pending> *g_queue = nullptr;
void (*g_wake)() = nullptr;
char g_saveResult[160] = "";

void Copy(char *out, size_t size, const char *s)
{
    if (size) {
        std::snprintf(out, size, "%s", s);
    }
}

bool Digits(const char *s, int *v)
{
    if (!*s) return false;
    int n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > 100000) return false;
        n = n * 10 + (*s - '0');
    }
    *v = n;
    return true;
}

// "WxH" with both sides in [320, 16384].
bool ParseSize(const char *s, int *w, int *h)
{
    const char *x = std::strpbrk(s, "xX");
    if (!x || x == s) return false;
    char a[16], b[16];
    size_t na = static_cast<size_t>(x - s), nb = std::strlen(x + 1);
    if (na >= sizeof(a) || nb >= sizeof(b)) return false;
    std::memcpy(a, s, na);
    a[na] = 0;
    std::memcpy(b, x + 1, nb + 1);
    return Digits(a, w) && Digits(b, h) && *w >= 320 && *h >= 320 && *w <= 16384 && *h <= 16384;
}

std::string Trimmed(const char *text)
{
    std::string s = text;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

std::string ReadFileText(const wchar_t *path, bool *exists)
{
    *exists = false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    static char buf[kMaxFileSize];
    DWORD size = 0;
    bool ok = ReadFile(f, buf, sizeof(buf), &size, nullptr) != 0;
    CloseHandle(f);
    *exists = ok;
    return ok ? std::string(buf, size) : std::string();
}

// Writes text next to the file, flushes it and swaps it in, so an interrupted write never leaves half a file.
bool WriteFileAtomic(const wchar_t *path, const std::string &text, DWORD *error)
{
    wchar_t tmp[MAX_PATH + 8];
    std::swprintf(tmp, sizeof(tmp) / sizeof(tmp[0]), L"%ls.tmp", path);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
              written == text.size() && FlushFileBuffers(f);
    *error = ok ? 0 : GetLastError();
    CloseHandle(f);
    if (ok && !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *error = GetLastError();
        ok = false;
    }
    if (!ok) {
        DeleteFileW(tmp);
    }
    return ok;
}

void Save()
{
    if (!g_path[0]) {
        Copy(g_saveResult, sizeof(g_saveResult), "not saved: the settings file has no path");
        return;
    }
    bool exists = false;
    std::string text = ReadFileText(g_path, &exists);
    // What the file says now (the user may have edited it since start-up): the last valid line of each key.
    std::vector<IniLine> lines;
    ReadIni(text, &lines, nullptr);
    std::vector<IniChange> changes;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_count; ++i) {
        const Slot &s = g_items[i];
        const char *inFile = nullptr;
        char norm[kValueSize];
        for (const IniLine &l : lines) {
            if (_stricmp(l.section.c_str(), s.item.section) == 0 && _stricmp(l.key.c_str(), s.item.key) == 0 &&
                Normalize(i, l.value.c_str(), norm, sizeof(norm))) {
                inFile = norm; // (norm is overwritten by later lines: the last valid one wins, as when loading)
            }
        }
        bool same = inFile ? std::strcmp(inFile, s.saved) == 0 : std::strcmp(s.saved, s.item.defaultValue) == 0;
        if (!same) {
            changes.push_back(IniChange{s.item.section, s.item.key, s.saved});
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (changes.empty()) {
        Copy(g_saveResult, sizeof(g_saveResult), "saved (the file already had these values)");
        return;
    }
    std::string out = UpdateIni(text, changes);
    DWORD error = 0;
    if (WriteFileAtomic(g_path, out, &error)) {
        std::snprintf(g_saveResult, sizeof(g_saveResult), "saved %d change(s) to emperor_engine.ini",
                      static_cast<int>(changes.size()));
        for (const IniChange &c : changes) {
            core_log("settings: saved [%s] %s = %s", c.section.c_str(), c.key.c_str(), c.value.c_str());
        }
    }
    else {
        std::snprintf(g_saveResult, sizeof(g_saveResult), "could not save emperor_engine.ini (error %lu)", error);
        core_log("settings: %s", g_saveResult);
    }
}

} // namespace

struct Retired {
    const char *section;
    const char *key;
    Migrate migrate;
};
Retired g_retired[16];
int g_retiredCount = 0;

void Retire(const char *section, const char *key, Migrate migrate)
{
    if (g_retiredCount < static_cast<int>(sizeof(g_retired) / sizeof(g_retired[0]))) {
        g_retired[g_retiredCount++] = Retired{section, key, migrate};
    }
}

int Register(const Item &item)
{
    if (g_count >= kMaxItems) {
        return -1;
    }
    Slot &s = g_items[g_count];
    s.item = item;
    Copy(s.value, sizeof(s.value), item.defaultValue);
    Copy(s.saved, sizeof(s.saved), item.defaultValue);
    Copy(s.startup, sizeof(s.startup), item.defaultValue);
    s.applyCount = 0;
    s.notApplied = false;
    std::snprintf(s.guardName, sizeof(s.guardName), "applying [%s] %s (core/settings)", item.section, item.key);
    s.guard.name = s.guardName;
    return g_count++;
}

int Find(const char *section, const char *key)
{
    for (int i = 0; i < g_count; ++i) {
        if (_stricmp(g_items[i].item.section, section) == 0 && _stricmp(g_items[i].item.key, key) == 0) {
            return i;
        }
    }
    return -1;
}

int Count()
{
    return g_count;
}

const Item &Get(int index)
{
    return g_items[index].item;
}

void OnChange(int index, Apply apply)
{
    if (index >= 0 && index < g_count && g_items[index].applyCount < 4) {
        g_items[index].apply[g_items[index].applyCount++] = apply;
    }
}

bool Normalize(int index, const char *text, char *out, size_t size)
{
    if (index < 0 || index >= g_count) {
        return false;
    }
    const Item &it = g_items[index].item;
    std::string t = Trimmed(text);
    const char *v = t.c_str();
    switch (it.type) {
    case Type::Bool:
        if (_stricmp(v, "on") == 0) { Copy(out, size, "on"); return true; }
        if (_stricmp(v, "off") == 0) { Copy(out, size, "off"); return true; }
        return false;
    case Type::Int: {
        if (it.autoWord && _stricmp(v, it.autoWord) == 0) {
            Copy(out, size, it.autoWord);
            return true;
        }
        int n;
        if (!Digits(v, &n) || n < it.min || n > it.max || (it.step > 1 && (n - it.min) % it.step != 0)) {
            return false;
        }
        std::snprintf(out, size, "%d", n);
        return true;
    }
    case Type::Choice:
        for (int i = 0; i < it.choiceCount; ++i) {
            if (_stricmp(v, it.choices[i]) == 0) {
                Copy(out, size, it.choices[i]);
                return true;
            }
        }
        return false;
    case Type::Resolution: {
        if (_stricmp(v, "scaled") == 0) {
            Copy(out, size, "scaled");
            return true;
        }
        int w, h;
        if (!ParseSize(v, &w, &h)) return false;
        std::snprintf(out, size, "%dx%d", w, h);
        return true;
    }
    case Type::Hotkey: {
        core::ui::Hotkey key;
        if (!core::ui::ParseHotkey(v, v + t.size(), &key)) return false;
        core::ui::FormatHotkey(key, out, size);
        return true;
    }
    }
    return false;
}

void Load(const wchar_t *path)
{
    Copy(g_saveResult, sizeof(g_saveResult), "");
    std::swprintf(g_path, MAX_PATH, L"%ls", path);
    bool exists = false;
    std::string text = ReadFileText(path, &exists);
    std::vector<IniLine> lines;
    std::vector<int> invalid;
    ReadIni(text, &lines, &invalid);
    std::vector<IniKey> retired;   // lines to remove from the file
    std::vector<IniChange> carried; // their values carried over to current settings
    for (const IniLine &l : lines) {
        const Retired *r = nullptr;
        for (int k = 0; k < g_retiredCount; ++k) {
            if (_stricmp(l.section.c_str(), g_retired[k].section) == 0 && _stricmp(l.key.c_str(), g_retired[k].key) == 0) {
                r = &g_retired[k];
            }
        }
        if (!r) {
            continue;
        }
        retired.push_back(IniKey{l.section, l.key});
        Migration m[4];
        const int n = r->migrate ? r->migrate(l.value.c_str(), m, 4) : 0;
        for (int k = 0; k < n && k < 4; ++k) {
            bool inFile = false;
            for (const IniLine &o : lines) {
                inFile = inFile || (_stricmp(o.section.c_str(), m[k].section) == 0 && _stricmp(o.key.c_str(), m[k].key) == 0);
            }
            const int i = Find(m[k].section, m[k].key);
            char norm[kValueSize];
            if (!inFile && i >= 0 && Normalize(i, m[k].value, norm, sizeof(norm))) {
                carried.push_back(IniChange{m[k].section, m[k].key, norm});
            }
        }
        core_log("config: line %d: [%s] %s = %s is no longer a setting: removed from the file%s", l.line,
                 l.section.c_str(), l.key.c_str(), l.value.c_str(), n > 0 ? " (its value carried over)" : "");
    }
    for (const IniLine &l : lines) {
        int i = Find(l.section.c_str(), l.key.c_str());
        char norm[kValueSize];
        bool isRetired = false;
        for (const IniKey &k : retired) {
            isRetired = isRetired || (k.section == l.section && k.key == l.key);
        }
        if (isRetired) {
            continue;
        }
        if (i < 0 || !Normalize(i, l.value.c_str(), norm, sizeof(norm))) {
            core_log("config: line %d: unknown setting [%s] %s = %s (ignored)", l.line, l.section.c_str(),
                     l.key.c_str(), l.value.c_str());
            continue;
        }
        Copy(g_items[i].value, kValueSize, norm);
    }
    for (int n : invalid) {
        core_log("config: line %d: not a setting (ignored)", n);
    }
    for (const IniChange &c : carried) {
        const int i = Find(c.section.c_str(), c.key.c_str());
        Copy(g_items[i].value, kValueSize, c.value.c_str());
        core_log("config: [%s] %s = %s (carried over from a removed setting)", c.section.c_str(), c.key.c_str(),
                 c.value.c_str());
    }
    if (!retired.empty()) {
        wchar_t bak[MAX_PATH + 8];
        std::swprintf(bak, sizeof(bak) / sizeof(bak[0]), L"%ls.bak", path);
        DWORD error = 0;
        const std::string cleaned = UpdateIni(RemoveIniKeys(text, retired), carried);
        if (WriteFileAtomic(bak, text, &error) && WriteFileAtomic(path, cleaned, &error)) {
            core_log("config: removed %d line(s) of settings that no longer exist (the file as it was: "
                     "emperor_engine.ini.bak)",
                     static_cast<int>(retired.size()));
        }
        else {
            core_log("config: could not remove the old settings from the file (error %lu); they are ignored", error);
        }
    }
    for (int i = 0; i < g_count; ++i) {
        Copy(g_items[i].startup, kValueSize, g_items[i].value);
        Copy(g_items[i].saved, kValueSize, g_items[i].value);
    }
    char what[512];
    Describe(what, sizeof(what));
    char p[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, p, sizeof(p), nullptr, nullptr);
    core_log("config: %s; %s", exists ? p : "emperor_engine.ini not found, defaults", what);
    for (int i = 0; i < g_count; ++i) {
        for (int k = 0; k < g_items[i].applyCount; ++k) {
            g_items[i].apply[k](i, g_items[i].value, true);
        }
    }
}

const wchar_t *Path()
{
    return g_path;
}

void Value(int index, char *out, size_t size)
{
    AcquireSRWLockShared(&g_lock);
    Copy(out, size, index >= 0 && index < g_count ? g_items[index].value : "");
    ReleaseSRWLockShared(&g_lock);
}

void StartupValue(int index, char *out, size_t size)
{
    AcquireSRWLockShared(&g_lock);
    Copy(out, size, index >= 0 && index < g_count ? g_items[index].startup : "");
    ReleaseSRWLockShared(&g_lock);
}

bool RestartPending(int index)
{
    AcquireSRWLockShared(&g_lock);
    bool r = false;
    if (index >= 0 && index < g_count) {
        const Slot &s = g_items[index];
        r = s.notApplied || (s.item.effect == Effect::Restart && std::strcmp(s.value, s.startup) != 0);
    }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

void Request(int index, const char *value, bool save)
{
    Pending p{index, "", save};
    Copy(p.value, sizeof(p.value), value);
    AcquireSRWLockExclusive(&g_lock);
    if (!g_queue) {
        g_queue = new std::vector<Pending>();
    }
    g_queue->push_back(p);
    ReleaseSRWLockExclusive(&g_lock);
    if (g_wake) {
        g_wake();
    }
}

void RequestSave()
{
    Request(-1, "", true);
}

namespace {

struct ApplyCall {
    Apply fn;
    int index;
    const char *value;
    bool result;
};

void RunApply(void *p)
{
    ApplyCall &c = *static_cast<ApplyCall *>(p);
    c.result = c.fn(c.index, c.value, false);
}

} // namespace

void RunPending()
{
    std::vector<Pending> work;
    AcquireSRWLockExclusive(&g_lock);
    if (g_queue) {
        work.swap(*g_queue);
    }
    ReleaseSRWLockExclusive(&g_lock);
    bool save = false;
    for (const Pending &p : work) {
        save = save || p.save;
        if (p.index < 0) { // RequestSave: every current value is confirmed
            AcquireSRWLockExclusive(&g_lock);
            for (int i = 0; i < g_count; ++i) {
                Copy(g_items[i].saved, kValueSize, g_items[i].value);
            }
            ReleaseSRWLockExclusive(&g_lock);
            continue;
        }
        if (p.index >= g_count) {
            continue;
        }
        Slot &s = g_items[p.index];
        char norm[kValueSize];
        if (!Normalize(p.index, p.value, norm, sizeof(norm))) {
            core_log("settings: [%s] %s = %s is not a valid value (ignored)", s.item.section, s.item.key, p.value);
            continue;
        }
        char savedBefore[kValueSize];
        Copy(savedBefore, sizeof(savedBefore), s.saved);
        if (p.save) { // only this item's value is confirmed (others may still wait for "keep")
            AcquireSRWLockExclusive(&g_lock);
            Copy(s.saved, kValueSize, norm);
            ReleaseSRWLockExclusive(&g_lock);
        }
        if (std::strcmp(norm, s.value) == 0) {
            continue;
        }
        if (s.item.effect != Effect::Restart && !core::diag::Enabled(s.guard)) {
            core_log("settings: [%s] %s = %s not applied: applying this setting was switched off after an error",
                     s.item.section, s.item.key, norm);
            continue;
        }
        char before[kValueSize];
        Copy(before, sizeof(before), s.value);
        AcquireSRWLockExclusive(&g_lock);
        Copy(s.value, kValueSize, norm);
        ReleaseSRWLockExclusive(&g_lock);
        bool applied = true, reverted = false;
        if (s.item.effect != Effect::Restart) {
            for (int k = 0; k < s.applyCount; ++k) {
                ApplyCall call{s.apply[k], p.index, norm, false};
                if (!core::diag::Guarded(s.guard, &RunApply, &call)) {
                    // The callback faulted (logged by the guard): the value goes back to the one before, the
                    // callbacks that took the new one get the old one again, and the new one is not saved.
                    AcquireSRWLockExclusive(&g_lock);
                    Copy(s.value, kValueSize, before);
                    if (p.save) {
                        Copy(s.saved, kValueSize, savedBefore);
                    }
                    ReleaseSRWLockExclusive(&g_lock);
                    for (int j = 0; j < k; ++j) {
                        s.apply[j](p.index, before, false);
                    }
                    reverted = true;
                    break;
                }
                applied = call.result && applied;
            }
        }
        if (reverted) {
            s.notApplied = false; // the old value is in effect again
            core_log("settings: [%s] %s %s -> %s not applied after an error; %s kept", s.item.section, s.item.key,
                     before, norm, before);
            continue;
        }
        s.notApplied = !applied;
        core_log("settings: [%s] %s %s -> %s (%s)", s.item.section, s.item.key, before, norm,
                 s.item.effect == Effect::Restart ? "after a restart"
                 : applied                       ? "applied"
                                                 : "after a restart: cannot change while the game runs");
    }
    if (save) {
        Save();
    }
}

void SetWakeup(void (*wake)())
{
    g_wake = wake;
}

void LastSaveResult(char *out, size_t size)
{
    Copy(out, size, g_saveResult);
}

void Describe(char *out, size_t size)
{
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < g_count && n + 1 < size; ++i) {
        int w = std::snprintf(out + n, size - n, "%s%s.%s=%s", i ? " " : "", g_items[i].item.section,
                              g_items[i].item.key, g_items[i].value);
        if (w < 0) break;
        n += static_cast<size_t>(w);
    }
}

} // namespace core::settings
