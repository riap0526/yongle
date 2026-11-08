#include "core/ui/hotkey.h"

#include <cstdio>
#include <cstring>

namespace core::ui {

namespace {

struct Name {
    const char *name;
    unsigned vk;
};

// Canonical names first (FormatHotkey uses the first entry of a code), aliases after.
const Name kNames[] = {
    {"Insert", 0x2D},         {"Delete", 0x2E},         {"Home", 0x24},           {"End", 0x23},
    {"PageUp", 0x21},         {"PageDown", 0x22},       {"Pause", 0x13},          {"ScrollLock", 0x91},
    {"Tab", 0x09},            {"Space", 0x20},          {"Backquote", 0xC0},      {"NumpadAdd", 0x6B},
    {"NumpadSubtract", 0x6D}, {"NumpadMultiply", 0x6A}, {"NumpadDivide", 0x6F},   {"NumpadDecimal", 0x6E},
    {"Up", 0x26},             {"Down", 0x28},           {"Left", 0x25},           {"Right", 0x27},
    {"LeftBracket", 0xDB},    {"RightBracket", 0xDD},   {"Semicolon", 0xBA},      {"Minus", 0xBD},
    {"Equals", 0xBB},         {"Comma", 0xBC},          {"Period", 0xBE},         {"Slash", 0xBF},
    {"Backslash", 0xDC},      {"Quote", 0xDE},           {"Mouse4", 0x05},          {"Mouse5", 0x06},
    {"Ins", 0x2D},            {"Del", 0x2E},            {"PgUp", 0x21},           {"PgDn", 0x22},
    {"`", 0xC0},
};

bool Equals(const char *b, const char *e, const char *word)
{
    size_t n = std::strlen(word);
    return static_cast<size_t>(e - b) == n && _strnicmp(b, word, n) == 0;
}

void Trim(const char *&b, const char *&e)
{
    while (b < e && (*b == ' ' || *b == '\t')) ++b;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t')) --e;
}

// Number in [b, e) within [lo, hi], or -1.
int Number(const char *b, const char *e, int lo, int hi)
{
    if (b == e || e - b > 2) return -1;
    int v = 0;
    for (const char *c = b; c < e; ++c) {
        if (*c < '0' || *c > '9') return -1;
        v = v * 10 + (*c - '0');
    }
    return v >= lo && v <= hi ? v : -1;
}

unsigned KeyCode(const char *b, const char *e)
{
    size_t n = static_cast<size_t>(e - b);
    if (n == 1) {
        char c = *b;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<unsigned char>(c);
    }
    if (n >= 2 && (*b == 'F' || *b == 'f')) {
        int f = Number(b + 1, e, 1, 24);
        if (f > 0) return 0x70 + f - 1;
    }
    if (n == 7 && _strnicmp(b, "Numpad", 6) == 0) {
        int d = Number(b + 6, e, 0, 9);
        if (d >= 0) return 0x60 + d;
    }
    for (const Name &k : kNames) {
        if (Equals(b, e, k.name)) return k.vk;
    }
    return 0;
}

} // namespace

bool ParseHotkey(const char *b, const char *e, Hotkey *out)
{
    Hotkey key{0, 0};
    for (;;) {
        const char *plus = b;
        while (plus < e && *plus != '+') ++plus;
        const char *pb = b, *pe = plus;
        Trim(pb, pe);
        if (plus == e) { // the last part: the key itself
            key.vk = KeyCode(pb, pe);
            break;
        }
        unsigned mod = Equals(pb, pe, "Ctrl") || Equals(pb, pe, "Control") ? kModCtrl
                       : Equals(pb, pe, "Shift")                           ? kModShift
                       : Equals(pb, pe, "Alt")                             ? kModAlt
                                                                           : 0;
        if (!mod || (key.mods & mod)) return false;
        key.mods |= mod;
        b = plus + 1;
    }
    if (!key.vk) return false;
    *out = key;
    return true;
}

void FormatHotkey(const Hotkey &key, char *out, size_t size)
{
    char name[16] = "?";
    if ((key.vk >= 'A' && key.vk <= 'Z') || (key.vk >= '0' && key.vk <= '9')) {
        std::snprintf(name, sizeof(name), "%c", static_cast<char>(key.vk));
    }
    else if (key.vk >= 0x70 && key.vk <= 0x87) {
        std::snprintf(name, sizeof(name), "F%u", key.vk - 0x70 + 1);
    }
    else if (key.vk >= 0x60 && key.vk <= 0x69) {
        std::snprintf(name, sizeof(name), "Numpad%u", key.vk - 0x60);
    }
    else {
        for (const Name &k : kNames) {
            if (k.vk == key.vk) {
                std::snprintf(name, sizeof(name), "%s", k.name);
                break;
            }
        }
    }
    std::snprintf(out, size, "%s%s%s%s", key.mods & kModCtrl ? "Ctrl+" : "", key.mods & kModShift ? "Shift+" : "",
                  key.mods & kModAlt ? "Alt+" : "", name);
}

int ParseHotkeyList(const char *b, const char *e, Hotkey out[kMaxKeys])
{
    Hotkey keys[kMaxKeys] = {};
    int n = 0;
    const char *p = b;
    while (true) {
        const char *comma = p;
        while (comma < e && *comma != ',') {
            ++comma;
        }
        const char *s = p, *t = comma;
        while (s < t && (*s == ' ' || *s == '\t')) ++s;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t')) --t;
        if (s == t || n == kMaxKeys || !ParseHotkey(s, t, &keys[n])) {
            return 0; // an empty part, too many, or not a hotkey
        }
        for (int k = 0; k < n; ++k) {
            if (keys[k].vk == keys[n].vk && keys[k].mods == keys[n].mods) {
                return 0; // the same key twice
            }
        }
        ++n;
        if (comma == e) {
            break;
        }
        p = comma + 1;
    }
    for (int k = 0; k < n; ++k) {
        out[k] = keys[k];
    }
    return n;
}

void FormatHotkeyList(const Hotkey *keys, int count, char *out, size_t size)
{
    size_t used = 0;
    if (size > 0) {
        out[0] = '\0';
    }
    for (int k = 0; k < count; ++k) {
        char one[32];
        FormatHotkey(keys[k], one, sizeof(one));
        const int w = std::snprintf(out + used, size > used ? size - used : 0, "%s%s", k > 0 ? ", " : "", one);
        if (w < 0 || used + static_cast<size_t>(w) >= size) {
            break;
        }
        used += static_cast<size_t>(w);
    }
}

void FormatHotkeyShown(const Hotkey &key, char *out, size_t size)
{
    // The punctuation keys as their characters on a US keyboard (the settings file spells them as words: ';' starts
    // a comment there and ',' separates two keys); [ ] and ` as words (the game's fonts have no such characters).
    static const struct {
        unsigned vk;
        const char *shown;
    } kShown[] = {{0xDB, "Left bracket"}, {0xDD, "Right bracket"}, {0xBA, ";"}, {0xBD, "-"}, {0xBB, "="},
                  {0xBC, ","}, {0xBE, "."}, {0xBF, "/"}, {0xDC, "\\"}, {0xDE, "'"}, {0xC0, "Backquote"},
                  {0x05, "Mouse 4"}, {0x06, "Mouse 5"}};
    for (const auto &k : kShown) {
        if (k.vk == key.vk) {
            std::snprintf(out, size, "%s%s%s%s", key.mods & kModCtrl ? "Ctrl+" : "", key.mods & kModShift ? "Shift+" : "",
                          key.mods & kModAlt ? "Alt+" : "", k.shown);
            return;
        }
    }
    FormatHotkey(key, out, size);
}

bool IsHotkeyKey(unsigned vk)
{
    char text[32];
    FormatHotkey(Hotkey{vk, 0}, text, sizeof(text));
    Hotkey back{0, 0};
    return text[0] != '?' && ParseHotkey(text, text + std::strlen(text), &back) && back.vk == vk;
}

} // namespace core::ui
