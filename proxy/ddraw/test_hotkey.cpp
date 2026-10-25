// Offline test of core/ui/hotkey (no DLL): the panel hotkey syntax of emperor_engine.ini.
#include "core/ui/hotkey.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Expect(const char *text, bool valid, unsigned vk = 0, unsigned mods = 0, const char *canonical = nullptr)
{
    core::ui::Hotkey key{0xFFFF, 0xFFFF};
    bool ok = core::ui::ParseHotkey(text, text + std::strlen(text), &key);
    char name[40] = "";
    if (ok) {
        core::ui::FormatHotkey(key, name, sizeof(name));
    }
    bool pass = ok == valid && (!valid || (key.vk == vk && key.mods == mods && std::strcmp(name, canonical) == 0));
    std::printf("%-24s -> %-8s vk 0x%02X mods %u %-20s %s\n", text, ok ? "valid" : "invalid", ok ? key.vk : 0,
                ok ? key.mods : 0, name, pass ? "ok" : "FAILED");
    if (!pass) {
        ++g_failures;
    }
}

} // namespace

int main()
{
    using namespace core::ui;
    Expect("Insert", true, 0x2D, 0, "Insert");
    Expect("insert", true, 0x2D, 0, "Insert");
    Expect("Ins", true, 0x2D, 0, "Insert");
    Expect("Ctrl+F12", true, 0x7B, kModCtrl, "Ctrl+F12");
    Expect("control + f12", true, 0x7B, kModCtrl, "Ctrl+F12");
    Expect("Alt+Shift+P", true, 'P', kModShift | kModAlt, "Shift+Alt+P");
    Expect("F1", true, 0x70, 0, "F1");
    Expect("F24", true, 0x87, 0, "F24");
    Expect("Numpad0", true, 0x60, 0, "Numpad0");
    Expect("NumpadAdd", true, 0x6B, 0, "NumpadAdd");
    Expect("`", true, 0xC0, 0, "Backquote");
    Expect("PgDn", true, 0x22, 0, "PageDown");
    Expect("7", true, '7', 0, "7");
    Expect("ScrollLock", true, 0x91, 0, "ScrollLock");
    Expect("F25", false);
    Expect("F0", false);
    Expect("Escape", false);
    Expect("Esc", false);
    Expect("Ctrl+", false);
    Expect("Ctrl", false);
    Expect("Ctrl+Ctrl+A", false);
    Expect("Win+A", false);
    Expect("", false);
    Expect("AB", false);
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
