// Framed full-screen pictures, Emperor's part. The framing itself (the panel tiles, the background, the city
// background, the settings but menu_screens's item) is the core's (core/display/framed_screens, shared with Zeus's
// layer); this layer gives it the pack, the tiles' entries, its variables and the front-end screens without
// the city (fullscreen.h), and keeps what only Emperor has: the loading screens' status line and its game font, and the
// loading pictures' black bands. The addresses are in the Site and Var declarations below.
//
//   fullscreen_art_load 0x526040 (hooked in resolution.cpp, which calls OnArtLoaded): loads a picture into slot
//      0x1a01 and draws it at (x, y). Indices 0xb..0xf are the loading pictures (800x600 at ((w-800)/2, (h-600)/2),
//      shown in five stages by 0x5b7490 from the start-up loader 0x475b60); their real area is found here, once per
//      picture, on the back buffer right after it was drawn (rows and columns at the edges with fewer than 2% of
//      pixels brighter than 24 are bands; a rule measured on the game's pictures).
//   screen_text_gdi 0x52b460 (cdecl, one string): the loader's status line, drawn by the game with GDI straight on the
//      screen at the centre + 250 px. Replaced: the line goes to the core, which draws it in the status block under
//      the framed picture in the game's font (below), or has the output draw it; it is cleared by the next picture
//      that is not a loading one.
//   panel_draw_tiles 0x5bed30: the dialog panel of group 0x20b, entry 348 of China_Interface.sg3 on; the inset panel
//      (inset_panel_draw_tiles 0x5bf080) group 0x20c, entry 492 on. Read by the core from the player's own
//      DATA\China_Interface.sg3/.555, in memory only, independent of the game's loading order and drawing state (the
//      game's own image_draw faulted during the start-up loader in testing).
//   Every entry point runs under the core's guard: on an exception the framing is switched off, the game's own
//      behaviour comes back (scaled frame with black bars, GDI status line) and the game goes on.
#include "games/emperor/fullscreen.h"

#include "core/assets/sg3.h"
#include "core/config.h"
#include "core/display/framed_screens.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "games/emperor/scroll.h"
#include "games/emperor/site.h"
#include "games/emperor/text.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace emperor::fullscreen {

namespace {

namespace framed = core::display::framed;
using framed::Canvas;

const Site kTextGdi{"screen_text_gdi", 0x52b460, {0x83, 0xec, 0x10, 0x53, 0x8b, 0x5c, 0x24, 0x18}};

BYTE *volatile *const kDrawBuffer = reinterpret_cast<BYTE *volatile *>(0x1156df8); // draw_buffer
volatile int *const kScreenPitch = reinterpret_cast<volatile int *>(0x10de000);   // bytes per row
volatile int *const kScreenWidth = reinterpret_cast<volatile int *>(0x10de01c);
volatile int *const kScreenHeight = reinterpret_cast<volatile int *>(0x10de018);
const Var<int> kScreenMode{0x10de0d8};
const Var<unsigned char> kGameScreen{0x10de045};

constexpr wchar_t kInterfacePack[] = L"DATA\\China_Interface.sg3"; // next to Emperor.exe
constexpr int kPanelFirst = 348; // group 0x20b in that pack
constexpr int kInsetFirst = 492; // group 0x20c: the inset panel (inset_panel_draw_tiles 0x5bf080)
constexpr wchar_t kFontPack[] = L"DATA\\China_Fonts.sg3";
constexpr int kFontFirst = 3283; // group 0x1e15 of China_Fonts: the font inside the message box's inset panel
const BYTE *const kCharMap = reinterpret_cast<const BYTE *>(0x8549ac); // font_char_map: byte -> glyph (0: none)
// The loading screen's block below the picture (mockup v5): parchment, the inset panel with the status line, parchment.
constexpr int kGapAbove = 8, kInsetHeight = 40, kGapBelow = 8;
constexpr int kSpaceWidth = 6, kLineHeight = 11; // text_get_width 0x527a80, text_draw 0x528430 (this font)
constexpr int kLoadFirst = 0xb, kLoadLast = 0xf; // China_Load1..5
constexpr unsigned char kScreenVideo = 0x4a;

using TextGdi_fn = void(__cdecl *)(const char *);
TextGdi_fn o_TextGdi;

bool g_installed = false;

// ---------------------------------------------------------------------------------------------
// The game's own bitmap font (China_Fonts, group 0x1e15), decoded from the player's file per glyph, in memory only.
// Chinese text comes later through the text layer (emperor::text) and a font atlas of our own.

struct Glyph {
    int w = 0, h = 0;
    std::vector<WORD> px;
};
core::assets::Sg3Pack g_fonts;
int g_fontsState = 0; // 0: not opened, 1: open, -1: failed
Glyph g_glyphs[256];  // by glyph number (font_char_map), decoded on first use
bool g_glyphTried[256];

const Glyph *GlyphFor(BYTE c)
{
    const int g = kCharMap[c];
    if (g == 0) {
        return nullptr;
    }
    if (g_fontsState == 0) {
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        while (len > 0 && path[len - 1] != L'\\') {
            --len;
        }
        path[len] = L'\0';
        std::wcscat(path, kFontPack);
        g_fontsState = g_fonts.Open(path) && g_fonts.Count() > kFontFirst + 255 ? 1 : -1;
        if (g_fontsState < 0) {
            core_log("fullscreen: no game font (%ls): the status line is drawn by the output instead", path);
        }
    }
    if (g_fontsState < 0) {
        return nullptr;
    }
    if (!g_glyphTried[g]) {
        g_glyphTried[g] = true;
        Glyph &gl = g_glyphs[g];
        if (!g_fonts.Size(kFontFirst + g - 1, &gl.w, &gl.h) || gl.w <= 0 || gl.h <= 0 || gl.w > 64 || gl.h > 64 ||
            !g_fonts.Decode565(kFontFirst + g - 1, framed::kTransparent, &gl.px)) {
            gl = Glyph{};
        }
    }
    return g_glyphs[g].w > 0 ? &g_glyphs[g] : nullptr;
}

// Draws the game's string (Windows-1252 bytes) centred in w x h at (x0, y0), as text_draw does it: a space advances
// 6 px, a letter its width; glyphs hang from a common top, characters from 0x80 that are taller than the line are
// raised by the difference (text_draw_glyph 0x5287c0). False when the font is not available.
bool DrawGameText(Canvas &im, const char *text, int x0, int y0, int w, int h)
{
    int width = 0, height = 0;
    for (const BYTE *c = reinterpret_cast<const BYTE *>(text); *c; ++c) {
        if (*c == ' ') {
            width += kSpaceWidth;
        }
        else if (const Glyph *g = GlyphFor(*c)) {
            width += g->w;
            height = g->h > height ? g->h : height;
        }
        else if (g_fontsState < 0) {
            return false;
        }
    }
    int x = x0 + (w - width) / 2;
    const int top = y0 + (h - height) / 2;
    for (const BYTE *c = reinterpret_cast<const BYTE *>(text); *c; ++c) {
        if (*c == ' ') {
            x += kSpaceWidth;
        }
        else if (const Glyph *g = GlyphFor(*c)) {
            const int raise = *c >= 0x80 && g->h > kLineHeight ? g->h - kLineHeight : 0;
            im.Paste(g->px.data(), g->w, g->h, x, top - raise, false);
            x += g->w;
        }
    }
    return true;
}

// Where the game's GDI status line went (the frame's centre + 250 px): the output draws it there when it is not in
// the status block.
int CaptionY()
{
    return (*kScreenHeight - 16) / 2 + 250;
}

bool CityAroundScreen(unsigned char screen, bool playerPicture)
{
    return CityAround(static_cast<char>(screen), playerPicture);
}

const framed::FramedGame kGame("fullscreen", kInterfacePack, kPanelFirst, kInsetFirst, kScreenMode, kGameScreen,
                               kScreenVideo, &CityAroundScreen,
                               framed::StatusBlock{kGapAbove, kInsetHeight, kGapBelow, &DrawGameText, &CaptionY});

// ---------------------------------------------------------------------------------------------
// The loading picture's real area

DWORD g_stageStart = 0;     // debug.loading_stage_ms: when the current loading stage's picture was loaded ...
int g_stageLines = 0;       // ... and how many status lines it has drawn (the loader draws one before and one after
                            // presenting the stage)
bool g_pending = false;     // a loading picture whose real area is still to be found ...
int g_pendingX = 0, g_pendingY = 0, g_pendingIndex = 0; // ... drawn at (x, y)

int Luminance(WORD p)
{
    int r = (p >> 11) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
    return (r * 299 + g * 587 + b * 114) / 1000;
}

// The real area of an 800x600 loading picture drawn at (x, y) on the back buffer; false (whole area) while it does
// not look drawn.
bool FindRealArea(int x, int y, int w, int h, RECT *out)
{
    const BYTE *back = *kDrawBuffer;
    const int pitch = *kScreenPitch, sw = *kScreenWidth, sh = *kScreenHeight;
    *out = RECT{x, y, x + w, y + h};
    if (!back || x < 0 || y < 0 || x + w > sw || y + h > sh || pitch < sw * 2) {
        return false;
    }
    auto lum = [&](int px, int py) {
        return Luminance(reinterpret_cast<const WORD *>(back + static_cast<size_t>(py) * pitch)[px]);
    };
    auto darkRow = [&](int py) {
        int n = 0;
        for (int px = x; px < x + w; px += 2) n += lum(px, py) > 24;
        return n < 0.02 * w / 2;
    };
    auto darkCol = [&](int px) {
        int n = 0;
        for (int py = y; py < y + h; py += 2) n += lum(px, py) > 24;
        return n < 0.02 * h / 2;
    };
    int top = y, bottom = y + h - 1, left = x, right = x + w - 1;
    while (top < bottom && darkRow(top)) ++top;
    while (bottom > top && darkRow(bottom)) --bottom;
    while (left < right && darkCol(left)) ++left;
    while (right > left && darkCol(right)) --right;
    RECT r{left, top, right + 1, bottom + 1};
    if (r.right - r.left < w / 2 || r.bottom - r.top < h / 4) {
        return false; // not drawn yet: the whole area meanwhile
    }
    *out = r;
    return true;
}

// Finds the pending loading picture's real area (logged once); the area meanwhile.
RECT DetectPending(const char *when)
{
    RECT r{};
    bool found = FindRealArea(g_pendingX, g_pendingY, 800, 600, &r);
    if (found) {
        g_pending = false;
    }
    core_log("fullscreen: loading picture %d at (%d,%d), %s: real area [%ld,%ld,%ld,%ld] (%ldx%ld)%s", g_pendingIndex,
             g_pendingX, g_pendingY, when, r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top,
             found ? "" : " -- not drawn yet, the whole 800x600 meanwhile");
    return r;
}

// The loader's status line (runs under the core's guard; see TextGdi_detour).
void StatusLine(void *p)
{
    const char *text = static_cast<const char *>(p);
    char game[256], utf8[512];
    std::snprintf(game, sizeof(game), "%s", text ? text : "");
    emperor::text::GameToUtf8(game, utf8, sizeof(utf8));
    if (g_pending) { // the loader draws the line right after the picture, before presenting
        framed::SetLoadingArea(DetectPending("at its status line"));
    }
    framed::SetStatus(game, utf8); // framed: composes the status block with the line in the game's font
    static long logs = 0;
    if (++logs <= 12) {
        core_log("fullscreen: status line \"%s\" (%s)", utf8,
                 framed::StatusInBlock() ? "in the status block, game font" : "drawn by the output instead of GDI");
    }
}

void __cdecl TextGdi_detour(const char *text)
{
    if (!framed::Guarded(&StatusLine, const_cast<char *>(text))) {
        o_TextGdi(text); // the game's own GDI line
    }
    // The second line of a stage comes after the stage was presented: keep it on screen (the output thread keeps
    // showing it) until the stage has lasted debug.loading_stage_ms.
    const int hold = core_config_get()->loading_stage_ms;
    if (hold > 0 && g_stageStart && ++g_stageLines == 2) {
        DWORD elapsed = GetTickCount() - g_stageStart;
        if (elapsed < static_cast<DWORD>(hold)) {
            Sleep(hold - elapsed);
        }
        core_log("fullscreen: loading stage held %d ms (debug.loading_stage_ms)", hold);
    }
}

struct ArtLoaded {
    int index, x, y;
};

void ArtLoadedBody(void *p)
{
    const ArtLoaded &a = *static_cast<ArtLoaded *>(p);
    if (a.index >= kLoadFirst && a.index <= kLoadLast) {
        g_pending = true;
        g_pendingX = a.x;
        g_pendingY = a.y;
        g_pendingIndex = a.index;
        g_stageStart = GetTickCount();
        g_stageLines = 0;
        const RECT r = DetectPending("after loading");
        framed::PictureShown(&r);
    }
    else {
        g_pending = false;
        g_stageStart = 0;
        framed::PictureShown(nullptr);
    }
}

// [display] fullscreen_pictures (framed | black) became loading_screen and menu_screens. Only black, a
// choice the player made, is carried over; framed was the default of the time, so the new defaults apply.
int MigrateFullscreenPictures(const char *old, core::settings::Migration *out, int max)
{
    if (_stricmp(old, "black") != 0 || max < 2) {
        return 0;
    }
    out[0] = core::settings::Migration{"video", "loading_screen", "original"};
    out[1] = core::settings::Migration{"video", "menu_screens", "original"};
    return 2;
}

void RegisterMenuScreens()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    const int i = core::settings::Register(Item{
        "video", "menu_screens", "Menu screens",
        "Main menu, emperor and campaign choice, briefings, high scores, victory and defeat, at the display scale. "
        "city: in a fret border, your city blurred around (menu_background.png or .bmp in Yongle's folder or the game "
        "folder if you put one there, otherwise the city you saw last); panel: in a fret border on the game's own "
        "panel pattern; framed: in a fret border, black around; original: the game's own picture, black "
        "around.",
        Type::Choice, "city", Effect::Live, true, true, 0, 0, 0, nullptr, framed::kMenuChoices, framed::kMenuLabels,
        4});
    core::settings::OnChange(i, &framed::MenuScreensChanged);
}

} // namespace

void OnArtLoaded(int index, int x, int y)
{
    if (!g_installed) {
        return;
    }
    ArtLoaded a{index, x, y};
    framed::Guarded(&ArtLoadedBody, &a);
}

void OnGameEnd()
{
    framed::OnGameEnd();
}

void OnPresent()
{
    framed::OnPresent(); // the menus have their own loop, without game_loop_step: compose here
}

void OnLoop()
{
    framed::OnLoop();
}

void RegisterSettings()
{
    core::settings::Retire("display", "fullscreen_pictures", &MigrateFullscreenPictures);
    // 0.2.0 item 8: the looks are designs only (the size follows the display scale). Older spellings, also of before
    // 0.1.3 (framed, black), go straight to the new ones (renames do not chain): the scaled looks to the design they
    // had around them. Later, parchment became panel (the same word in Zeus, whose panels are stone); framed is a value
    // again (a fret border, black around), so the spelling of before 0.1.3 is no longer renamed (it now gives that
    // look instead of parchment).
    core::settings::RenameValue("video", "menu_screens", "native_city", "city");
    core::settings::RenameValue("video", "menu_screens", "native", "panel");
    core::settings::RenameValue("video", "menu_screens", "scaled", "panel");
    core::settings::RenameValue("video", "menu_screens", "parchment", "panel");
    core::settings::RenameValue("video", "menu_screens", "scaled_black", "original");
    core::settings::RenameValue("video", "menu_screens", "black", "original");
    core::settings::RenameValue("video", "loading_screen", "native", "framed");
    core::settings::RenameValue("video", "loading_screen", "scaled", "framed");
    core::settings::RenameValue("video", "loading_screen", "scaled_black", "original");
    core::settings::RenameValue("video", "loading_screen", "black", "original");
    framed::RegisterSettings(&RegisterMenuScreens);
}

bool Attach()
{
    framed::Configure(kGame); // the China map's border reads the tiles also without framing
    if (!Matches(kTextGdi)) {
        core_log("fullscreen: %s at 0x%lx does not match; framed pictures not installed", kTextGdi.name,
                 kTextGdi.address);
        return false;
    }
    Install(kTextGdi, &TextGdi_detour, o_TextGdi);
    g_installed = true;
    framed::Install();
    emperor::scroll::SetCityDrawn(&framed::KeepCity);
    core_log("fullscreen: the loading screens' status line is drawn in the status block or by the output");
    return true;
}

} // namespace emperor::fullscreen
