#include "games/emperor/notice.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/output/sdl_output.h"
#include "games/emperor/options_text.h"
#include "games/emperor/site.h"

#include <atomic>
#include <cstdio>
#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/archive/game_notices.md section 2 (Ghidra, capstone,
// 2026-11-08); calling conventions: docs/symbols.md.
namespace emperor::notice {
namespace {

// city_warnings_draw 0x547a20 (__cdecl, no arguments): every game frame (game_frame 0x536e55) draws the boxes and
// clears those shown more than 15000 ms ago.
const Site kDraw{"city_warnings_draw", 0x547a20, {0x83, 0xec, 0x18, 0xe8, 0xa8, 0xd4, 0xf2, 0xff}};
// city_warning_show_text(text, flag) 0x547d30 (__cdecl; ret): a box with this text (99 characters at most); flag 0:
// a plain notice (id -1, the game's notice font); 1 would also send it as a multiplayer chat line.
const Site kShowText{"city_warning_show_text", 0x547d30, {0x53, 0x8a, 0x5c, 0x24, 0x0c, 0x55, 0x8b, 0x6c}};
// What city_warning_show_text does with a slot, for rewriting the zoom box in place (the same calls, __cdecl):
// text_get_width(text, font) 0x527a80, text_lines(text) 0x527b70, text_copy_lines(text, to, lines) 0x5279d0.
const Site kTextWidth{"text_get_width", 0x527a80, {0x51, 0x53, 0x55, 0x56, 0x57, 0x68, 0x1c, 0x1e}};
const Site kTextLines{"text_lines", 0x527b70, {0x56, 0x8b, 0x74, 0x24, 0x08, 0xba, 0x10, 0x27}};
const Site kTextCopy{"text_copy_lines", 0x5279d0, {0x56, 0x8b, 0x74, 0x24, 0x10, 0x85, 0xf6, 0x7e}};

using Draw_fn = void(__cdecl *)();
using ShowText_fn = void(__cdecl *)(const char *text, char flag);
using TextWidth_fn = int(__cdecl *)(const char *text, int font);
using TextLines_fn = short(__cdecl *)(const char *text);
using TextCopy_fn = void(__cdecl *)(const char *text, char *to, int lines);
using ImageGetId_fn = int(__thiscall *)(void *, int);
Draw_fn o_Draw = nullptr;

const DWORD kImageGetId = 0x408170, kImages = 0x1c42130; // image_get_id (__thiscall on the image table)
constexpr int kNoticeFont = 0x1e14;                        // what city_warning_show_text measures with

const Var<BYTE> kWarningsOn{0x10de131}; // the Help menu's Warnings ON / OFF (0x449520)
const Var<int> kFrameTime{0x1156dec};   // frame_time (ms): when a box was shown
constexpr DWORD kSlots = 0x1192778;     // 5 boxes of 0x78 bytes
constexpr int kSlotCount = 5, kSlotSize = 0x78, kTextSize = 100;

// One box (the game's layout, city_warning_show 0x547e60).
#pragma pack(push, 1)
struct Slot {
    short id;    // +0: the text's number; -1 for a text given (ours), -2 a chat line
    short x, y;  // +2, +4
    short inUse; // +6
    short lines; // +8
    short width; // +0xA: the box's width (200, 300, 400 or 460)
    int shown;   // +0xC: frame_time when shown
    short pad;   // +0x10
    char text[kTextSize]; // +0x12
};
#pragma pack(pop)
static_assert(sizeof(Slot) == 0x76 && sizeof(Slot) <= kSlotSize, "the game's notice slot");

Slot *SlotAt(int k)
{
    return reinterpret_cast<Slot *>(kSlots + g_imageShift + k * kSlotSize);
}

core::diag::Feature g_feature{"notices in the game's boxes (games/emperor/notice)"};
bool g_attached = false;

// Posted texts (any thread), shown at the next frame.
struct Posted {
    char text[kTextSize];
    bool red;
};
SRWLOCK g_lock = SRWLOCK_INIT;
Posted g_queue[4];
int g_queued = 0;

std::atomic<int> g_zoom{-1}; // the zoom to show (-1: nothing new)
int g_zoomSlot = -1;          // the box we made for the zoom (game thread)
char g_zoomText[kTextSize];   // what that box holds (to tell it is still ours)

// The game draws a box in its red warning font when the box's id is a notice number whose byte in 0x858048 is 0
// (e.g. 132 "Opinion of you has plummeted.", 185 "Your city needs more workers."); 1 gives font 0x1e16, 2 and a text
// given (id -1, ours) 0x1e17 (e.g. 7 "Game saved"). The id is read only there (city_warnings_draw 0x547c63; the
// other reference is city_warning_show_text writing it), so a red box of ours gets the first such number.
constexpr short kRedId = 5;

void ShowText(const char *text)
{
    const BYTE on = *kWarningsOn;
    *kWarningsOn = 1; // ours answer the player: shown also with the game's warnings off
    Function<ShowText_fn>(kShowText)(text, 0);
    *kWarningsOn = on;
}

int FirstFree()
{
    for (int k = 0; k < kSlotCount; ++k) {
        if (SlotAt(k)->inUse == 0) {
            return k;
        }
    }
    return -1;
}

int TextWidth(const char *text)
{
    const int font = reinterpret_cast<ImageGetId_fn>(kImageGetId + g_imageShift)(
        reinterpret_cast<void *>(kImages + g_imageShift), kNoticeFont);
    return Function<TextWidth_fn>(kTextWidth)(text, font);
}

// Our box's width for a text this many pixels wide. The game's own rule (city_warning_show_text) gives 460 above
// 300 pixels, without the side ornaments; between the 400-pixel box's ornaments there is room for about 330
// (2026-11-08: a 309-pixel text looked as if it would fit), so ours keep the 400-pixel box up to 320.
short BoxWidth(int px)
{
    return px <= 100 ? 200 : px <= 200 ? 300 : px <= 320 ? 400 : 460;
}

// A new box with text; its slot index, or -1 when there was no room (five boxes shown, as for the game's own).
int NewBox(const char *text, bool red)
{
    const int k = FirstFree();
    if (k < 0) {
        return -1;
    }
    ShowText(text);
    Slot *s = SlotAt(k);
    if (s->inUse == 0 || s->id != -1) {
        return -1; // not shown (e.g. not on a game screen)
    }
    s->width = BoxWidth(TextWidth(text));
    if (red) {
        s->id = kRedId;
    }
    return k;
}

void ShowZoom(int percent)
{
    char text[32];
    std::snprintf(text, sizeof(text), emperor::options_text::kNoticeZoom, percent);
    if (g_zoomSlot >= 0) {
        Slot *s = SlotAt(g_zoomSlot);
        if (s->inUse != 0 && s->id == -1 && std::memcmp(s->text, g_zoomText, kTextSize) == 0) {
            // Rewritten in place: the box keeps its place among the others and does not open again.
            s->width = BoxWidth(TextWidth(text));
            s->lines = Function<TextLines_fn>(kTextLines)(text);
            std::memset(s->text, 0, kTextSize);
            Function<TextCopy_fn>(kTextCopy)(text, s->text, s->lines);
            s->shown = *kFrameTime;
            std::memcpy(g_zoomText, s->text, kTextSize);
            return;
        }
        g_zoomSlot = -1; // gone (its time was up) or taken by another notice
    }
    g_zoomSlot = NewBox(text, false);
    if (g_zoomSlot >= 0) {
        std::memcpy(g_zoomText, SlotAt(g_zoomSlot)->text, kTextSize);
    }
}

void Flush(void *)
{
    Posted posted[4];
    AcquireSRWLockExclusive(&g_lock);
    const int n = g_queued;
    std::memcpy(posted, g_queue, sizeof(posted));
    g_queued = 0;
    ReleaseSRWLockExclusive(&g_lock);
    for (int k = 0; k < n; ++k) {
        NewBox(posted[k].text, posted[k].red);
    }
    const int zoom = g_zoom.exchange(-1);
    if (zoom >= 0) {
        ShowZoom(zoom);
    }
}

void __cdecl Draw_detour()
{
    if (core::diag::Enabled(g_feature)) {
        core::diag::Guarded(g_feature, &Flush, nullptr);
    }
    Original(o_Draw, kDraw)();
}

} // namespace

bool Attach()
{
    const Site *sites[] = {&kDraw, &kShowText, &kTextWidth, &kTextLines, &kTextCopy};
    for (const Site *s : sites) {
        if (!Matches(*s)) {
            core_log("notice: %s at 0x%lx does not match; our messages stay on our own caption line", s->name,
                     s->address);
            return false;
        }
    }
    if (!Install(kDraw, &Draw_detour, o_Draw)) {
        core_log("notice: city_warnings_draw could not be hooked; our messages stay on our own caption line");
        return false;
    }
    g_attached = true;
    core_log("notice: our messages are shown in the game's notice boxes (also with the game's warnings off)");
    return true;
}

void Post(const char *text, bool red)
{
    core_log("notice%s: %s", red ? " (red)" : "", text);
    if (!g_attached || !core::diag::Enabled(g_feature)) {
        core::output::SetNotice(text, 5000);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_queued < 4) {
        std::snprintf(g_queue[g_queued].text, kTextSize, "%s", text);
        g_queue[g_queued++].red = red;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void Zoom(int percent)
{
    if (g_attached) {
        g_zoom.store(percent);
    }
}

} // namespace emperor::notice
