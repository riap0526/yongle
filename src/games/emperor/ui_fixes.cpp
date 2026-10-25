#include "games/emperor/ui_fixes.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "games/emperor/site.h"

#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/archive/resolution_ui_r2b1.md, R2b.1-7.
namespace emperor::ui_fixes {
namespace {

// messages_text_buttons 0x857ed8: 3 entries of 0x1c bytes, each starting with its rectangle (x1, y1, x2, y2 as
// shorts) relative to the base point that messages_draw 0x549720 and messages_input 0x549ad0 both compute (window
// x + width / 2 - 0x8c). Entry 1 is "Click to change how messages are received": (-100, 32)-(200, 62); entry 0,
// "Delete opened messages", is (40, 0)-(260, 30), so the same width centred under it is (0, 32)-(300, 62).
constexpr DWORD kChangeButton = 0x857ed8 + 0x1c;
constexpr short kOriginalX1 = -100, kOriginalX2 = 200, kCentredX1 = 0, kCentredX2 = 300;
const BYTE kChangeButtonBytes[8] = {0x9c, 0xff, 0x20, 0x00, 0xc8, 0x00, 0x3e, 0x00};

// The button's text is not drawn from the table: messages_draw_background calls lang_text_draw_centered 0x528890
// (__cdecl; group, id, x, y, width, font, 0; ret) at 0x5496ba with x = window x + 0x40 and width 0xf0. The English
// text is 285 px wide, wider than 0xf0, and then the game draws it from x without centring (eighth R2b.1 test: it
// ran past the frame's right edge). We call it instead with the frame itself as the area: x = the frame's left,
// base + 0 = window x + width * 8 - 0x8c, i.e. x + width * 8 - 204 (width in 16-pixel units at 0x1192a58), and
// width 300, so the game centres the text in the frame whatever its length.
constexpr DWORD kChangeTextCall = 0x5496ba;
const BYTE kChangeTextBytes[8] = {0x6a, 0x07, 0x6a, 0x3f, 0xe8, 0xd1, 0xf1, 0xfd}; // push 7; push 0x3f; call ...
constexpr DWORD kTextCentred = 0x528890;
const Var<int> kDialogWidth{0x1192a58};

// messages_third_button 0x4fae50 is [this + 0x10] != 0 with this = 0x10bc348 (the constant 0x408ac0 returns).
const Var<int> kThirdButton{0x10bc348 + 0x10};

// A one-byte displacement in the game's code that moves something (e.g. lea ebx, [ecx+0x73]).
struct Displacement {
    const char *name;
    DWORD address;  // of the instruction
    BYTE expect[8]; // its bytes and what follows
    int offset;     // of the displacement byte within them
};

// The Mill's list rows: y = layout top + 0x73, in mill_window_foreground 0x556a80 (drawing) and mill_window_input
// 0x556940 (clicks). Both move, so the clicks stay on the rows.
const Displacement kMillRows[] = {
    {"mill_window_foreground list top", 0x556b0b, {0x8d, 0x59, 0x73, 0x89, 0x5c, 0x24, 0x28, 0x0f}, 2},
    {"mill_window_input list top", 0x556983, {0x8d, 0x6a, 0x73, 0x8b, 0x54, 0x24, 0x20, 0xc1}, 2},
};

// build_submenu_draw 0x5b7030: each item's frame at y (0x6e + scroll + 0x18 per item), its name at y + 3 (0x71, four
// paths) and its cost at y + 4 (0x72, two paths). Only drawing; clicks go by the frames, which stay.
const Displacement kSubmenuText[] = {
    {"build_submenu_draw name (0x11)", 0x5b7136, {0x8d, 0x4c, 0x06, 0x71, 0x8b, 0x44, 0x24, 0x4c}, 3},
    {"build_submenu_draw name (0x29)", 0x5b71cf, {0x8d, 0x44, 0x16, 0x71, 0x68, 0xb0, 0x00, 0x00}, 3},
    {"build_submenu_draw name", 0x5b7242, {0x8d, 0x54, 0x0e, 0x71, 0x68, 0xb0, 0x00, 0x00}, 3},
    {"build_submenu_draw name (7, 0x13)", 0x5b7375, {0x8d, 0x4c, 0x06, 0x71, 0x68, 0x00, 0x01, 0x00}, 3},
    {"build_submenu_draw cost", 0x5b72e2, {0x8d, 0x54, 0x0e, 0x72, 0x53, 0x8d, 0x47, 0xbe}, 3},
    {"build_submenu_draw cost (7, 0x13)", 0x5b73fb, {0x8d, 0x4c, 0x06, 0x72, 0x53, 0x8d, 0x57, 0xdc}, 3},
};

core::diag::Feature g_messages{"messages screen buttons (games/emperor/ui_fixes)"};
bool g_messagesOn = false;

BYTE *At(DWORD address)
{
    return reinterpret_cast<BYTE *>(address + g_imageShift);
}

volatile short *ChangeButton()
{
    return reinterpret_cast<volatile short *>(At(kChangeButton));
}

void SetChangeButton(bool centred)
{
    volatile short *r = ChangeButton();
    r[0] = centred ? kCentredX1 : kOriginalX1;
    r[2] = centred ? kCentredX2 : kOriginalX2;
}

void UpdateMessages(void *)
{
    bool centred = *kThirdButton == 0;
    if ((ChangeButton()[0] == kCentredX1) != centred) {
        SetChangeButton(centred);
    }
}

struct TextArea {
    int x, width;
};

void CentreChangeText(void *p)
{
    if (ChangeButton()[0] == kCentredX1) {
        TextArea &a = *static_cast<TextArea *>(p);
        a.x += *kDialogWidth * 8 - 0x8c - 0x40 + kCentredX1;
        a.width = kCentredX2 - kCentredX1;
    }
}

using TextCentred_fn = int(__cdecl *)(int, int, int, int, int, int, int);

int __cdecl ChangeText(int group, int id, int x, int y, int width, int font, int z)
{
    TextArea a{x, width};
    if (g_messagesOn && core::diag::Enabled(g_messages) && !core::diag::Guarded(g_messages, &CentreChangeText, &a)) {
        a = TextArea{x, width};
    }
    return reinterpret_cast<TextCentred_fn>(kTextCentred + g_imageShift)(group, id, a.x, y, a.width, font, z);
}

// All of a group or none: checks every site, then adds delta to each displacement.
bool Shift(const Displacement *list, size_t n, int delta, const char *what)
{
    for (size_t i = 0; i < n; ++i) {
        if (std::memcmp(At(list[i].address), list[i].expect, sizeof(list[i].expect)) != 0) {
            core_log("ui_fixes: %s at 0x%lx does not match; %s left as it is", list[i].name, list[i].address, what);
            return false;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const BYTE b = static_cast<BYTE>(list[i].expect[list[i].offset] + delta);
        if (!PatchCode(list[i].address + g_imageShift + list[i].offset, &b, 1)) {
            core_log("ui_fixes: cannot write %s at 0x%lx", list[i].name, list[i].address);
            return false;
        }
    }
    return true;
}

bool CentreMessages()
{
    if (std::memcmp(At(kChangeButton), kChangeButtonBytes, sizeof(kChangeButtonBytes)) != 0 ||
        std::memcmp(At(kChangeTextCall - 4), kChangeTextBytes, sizeof(kChangeTextBytes)) != 0 ||
        *reinterpret_cast<DWORD *>(At(kChangeTextCall + 1)) != kTextCentred - (kChangeTextCall + 5)) {
        core_log("ui_fixes: messages screen code or table does not match; messages screen left as it is");
        return false;
    }
    BYTE call[5] = {0xe8};
    const DWORD rel = reinterpret_cast<DWORD>(&ChangeText) - reinterpret_cast<DWORD>(At(kChangeTextCall + 5));
    std::memcpy(call + 1, &rel, 4);
    if (!PatchCode(kChangeTextCall + g_imageShift, call, sizeof(call))) {
        core_log("ui_fixes: cannot write the call at 0x%lx", kChangeTextCall);
        return false;
    }
    return true;
}

} // namespace

bool Attach()
{
    g_messagesOn = CentreMessages();
    bool mill = Shift(kMillRows, sizeof(kMillRows) / sizeof(kMillRows[0]), kMillListShift, "Mill window");
    bool submenu = Shift(kSubmenuText, sizeof(kSubmenuText) / sizeof(kSubmenuText[0]), kSubmenuTextShift,
                         "build submenu");
    core_log("ui_fixes: messages screen buttons %s; Mill list %s; build submenu text %s",
             g_messagesOn ? "centred when 2" : "unchanged", mill ? "moved down" : "unchanged",
             submenu ? "moved down" : "unchanged");
    return g_messagesOn || mill || submenu;
}

void OnLoop()
{
    if (!g_messagesOn || !core::diag::Enabled(g_messages)) {
        return;
    }
    if (!core::diag::Guarded(g_messages, &UpdateMessages, nullptr)) {
        SetChangeButton(false); // the game's own table again; ChangeText then passes x through
    }
}

} // namespace emperor::ui_fixes
