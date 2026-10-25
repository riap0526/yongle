// R2a: framed full-screen pictures. Design: docs/resolution_ui/fullscreen_art.md §7.5; screens:
// docs/resolution_ui/r2_screens.md §4.1; addresses and evidence: docs/symbols.md.
//
//   fullscreen_art_load 0x526040 (hooked in resolution.cpp, which calls OnArtLoaded): loads a picture into slot
//      0x1a01 and draws it at (x, y). Indices 0xb..0xf are the loading pictures (800x600 at ((w-800)/2, (h-600)/2),
//      shown in five stages by 0x5b7490 from the start-up loader 0x475b60); their real area is found here, once per
//      picture, on the back buffer right after it was drawn (rows and columns at the edges with fewer than 2% of
//      pixels brighter than 24 are bands; the rule of tools/mockups/fsmock3.py).
//   screen_text_gdi 0x52b460 (cdecl, one string): the loader's status line, drawn by the game with GDI straight on the
//      screen at the centre + 250 px. Replaced: the line goes to the output (core::output::SetCaption), which draws it
//      on a dark strip over the framed picture's bottom; it is cleared by the next picture that is not a loading one.
//   panel_draw_tiles 0x5bed30: the dialog panel of group 0x20b (image_get_id(0x20b) + 0..0x8f: corners 0, 0xb,
//      0x84, 0x8f; top edge 1..10, bottom 0x85..0x8e; left 0xc + 12 r, right 0x17 + 12 r; inside 0xd + 12 r + c).
//      Group 0x20b is entry 348 of China_Interface.sg3 on. The 144 tiles are read from the player's own
//      DATA\China_Interface.sg3/.555 with core/assets/sg3, in memory only, independent of the game's loading order
//      and drawing state (the game's own image_draw faulted during the start-up loader: R2a tests 2 and 3).
//   Every entry point runs under core/diag's guard: on an exception the feature is switched off, the game's own
//      behaviour comes back (scaled frame with black bars, GDI status line) and the game goes on.
#include "games/emperor/fullscreen.h"

#include "core/assets/image_file.h"
#include "core/assets/sg3.h"
#include "core/capture/screenshot.h"
#include "core/config.h"
#include "core/diag/guard.h"
#include "core/display/framing.h"
#include "core/display/scaled_rect.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/output/sdl_output.h"
#include "core/settings/settings.h"
#include "games/emperor/resolution.h"
#include "games/emperor/scroll.h"
#include "games/emperor/site.h"
#include "games/emperor/text.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace emperor::fullscreen {

namespace {

using core::display::FrameLayout;
using core::display::kTile;

const Site kTextGdi{"screen_text_gdi", 0x52b460, {0x83, 0xec, 0x10, 0x53, 0x8b, 0x5c, 0x24, 0x18}};

BYTE *volatile *const kDrawBuffer = reinterpret_cast<BYTE *volatile *>(0x1156df8); // draw_buffer
volatile int *const kScreenPitch = reinterpret_cast<volatile int *>(0x10de000);   // bytes per row
volatile int *const kScreenWidth = reinterpret_cast<volatile int *>(0x10de01c);
volatile int *const kScreenHeight = reinterpret_cast<volatile int *>(0x10de018);
volatile int *const kScreenMode = reinterpret_cast<volatile int *>(0x10de0d8);
volatile char *const kGameScreen = reinterpret_cast<volatile char *>(0x10de045);

constexpr wchar_t kInterfacePack[] = L"DATA\\China_Interface.sg3"; // next to Emperor.exe
constexpr int kPanelFirst = 348; // group 0x20b in that pack
constexpr int kPanelTiles = 0x90;
constexpr int kInsetFirst = 492; // group 0x20c: the inset panel (inset_panel_draw_tiles 0x5bf080)
constexpr int kInsetTiles = 49;
constexpr wchar_t kFontPack[] = L"DATA\\China_Fonts.sg3";
constexpr int kFontFirst = 3283; // group 0x1e15 of China_Fonts: the font inside the message box's inset panel
const BYTE *const kCharMap = reinterpret_cast<const BYTE *>(0x8549ac); // font_char_map: byte -> glyph (0: none)
// The loading screen's block below the picture (docs/archive/resolution_ui_r2a.md §10, mockup v5): parchment, the inset panel with
// the status line, parchment.
constexpr int kGapAbove = 8, kInsetHeight = 40, kGapBelow = 8;
constexpr int kBelow = kGapAbove + kInsetHeight + kGapBelow;
constexpr int kSpaceWidth = 6, kLineHeight = 11; // text_get_width 0x527a80, text_draw 0x528430 (this font)
constexpr int kLoadFirst = 0xb, kLoadLast = 0xf; // China_Load1..5
constexpr char kScreenVideo = 0x4a;
constexpr WORD kKey = 0xF81F; // not drawn: transparent

using TextGdi_fn = void(__cdecl *)(const char *);
TextGdi_fn o_TextGdi;

bool g_installed = false;
core::diag::Feature g_feature{"framed full-screen pictures (games/emperor/fullscreen)"};

// ---------------------------------------------------------------------------------------------
// The panel tiles, read once from the player's own China_Interface pack

struct Tiles {
    bool ok = false;
    bool failed = false;
    WORD px[kPanelTiles][kTile * kTile];
    WORD inset[kInsetTiles][kTile * kTile];
};
Tiles g_tiles;

bool LoadTiles()
{
    if (g_tiles.ok || g_tiles.failed) {
        return g_tiles.ok;
    }
    g_tiles.failed = true; // tried once
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (len > 0 && path[len - 1] != L'\\') {
        --len;
    }
    path[len] = L'\0';
    if (len + sizeof(kInterfacePack) / sizeof(wchar_t) >= MAX_PATH) {
        return false;
    }
    std::wcscat(path, kInterfacePack);
    core::assets::Sg3Pack pack;
    if (!pack.Open(path) || pack.Count() < kInsetFirst + kInsetTiles) {
        core_log("fullscreen: no dialog panel tiles (%ls): framed pictures without background", path);
        return false;
    }
    long transparent = 0;
    std::vector<WORD> px;
    for (int i = 0; i < kPanelTiles; ++i) {
        int w = 0, h = 0;
        if (!pack.Size(kPanelFirst + i, &w, &h) || w != kTile || h != kTile ||
            !pack.Decode565(kPanelFirst + i, kKey, &px)) {
            core_log("fullscreen: China_Interface entry %d is not a 16x16 panel tile (%dx%d): framed pictures without "
                     "background",
                     kPanelFirst + i, w, h);
            return false;
        }
        std::memcpy(g_tiles.px[i], px.data(), sizeof(g_tiles.px[i]));
        for (WORD v : px) {
            transparent += v == kKey;
        }
    }
    for (int i = 0; i < kInsetTiles; ++i) {
        int w = 0, h = 0;
        if (!pack.Size(kInsetFirst + i, &w, &h) || w != kTile || h != kTile ||
            !pack.Decode565(kInsetFirst + i, kKey, &px)) {
            core_log("fullscreen: China_Interface entry %d is not a 16x16 inset tile (%dx%d): framed pictures without "
                     "background",
                     kInsetFirst + i, w, h);
            return false;
        }
        std::memcpy(g_tiles.inset[i], px.data(), sizeof(g_tiles.inset[i]));
    }
    g_tiles.ok = true;
    g_tiles.failed = false;
    core_log("fullscreen: %d dialog panel and %d inset panel tiles read from %ls (entries %d..%d, %d..%d; %ld "
             "transparent pixels; in memory only)",
             kPanelTiles, kInsetTiles, path, kPanelFirst, kPanelFirst + kPanelTiles - 1, kInsetFirst,
             kInsetFirst + kInsetTiles - 1, transparent);
    return true;
}

// ---------------------------------------------------------------------------------------------
// The background for a layout (tools/mockups/fsmock3.py: frame_image, parchment)

int Luminance(WORD p)
{
    int r = (p >> 11) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
    return (r * 299 + g * 587 + b * 114) / 1000;
}

constexpr WORD kLabelOut = 0x001F, kLabelDark = 0xF800, kLabelBorder = 0x07E0, kLabelPad = 0xFFE0;
constexpr int kDarkLine = 45; // the border's dark lines (luminance 26 in the tiles)

struct Image {
    int w, h;
    std::vector<WORD> px;
    Image(int w_, int h_, WORD fill) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_, fill) {}
    // Pastes sw x sh pixels (transparent: kKey) at (x, y); labels: dark-line or border label colours instead.
    void PasteImage(const WORD *src, int sw, int sh, int x, int y, bool labels)
    {
        for (int ty = 0; ty < sh; ++ty) {
            for (int tx = 0; tx < sw; ++tx) {
                WORD p = src[ty * sw + tx];
                int dx = x + tx, dy = y + ty;
                if (p == kKey || dx < 0 || dy < 0 || dx >= w || dy >= h) {
                    continue;
                }
                px[static_cast<size_t>(dy) * w + dx] = labels ? (Luminance(p) < kDarkLine ? kLabelDark : kLabelBorder) : p;
            }
        }
    }
    void Paste(int tile, int x, int y, bool labels) { PasteImage(g_tiles.px[tile], kTile, kTile, x, y, labels); }
};

void Parchment(Image &im)
{
    for (int r = 0; r * kTile < im.h; ++r) {
        for (int c = 0; c * kTile < im.w; ++c) {
            im.Paste(0xd + (r % 10) * 12 + (c % 10), c * kTile, r * kTile, false);
        }
    }
}

// Border of whole tiles, the last ones flush with the far edges, over the parchment (or label colours). fill =
// false: the border only, over what im holds; w x h (from the top left): a border of that size instead of im's.
void Frame(Image &im, bool labels, bool fill = true, int w = 0, int h = 0)
{
    w = w > 0 ? w : im.w;
    h = h > 0 ? h : im.h;
    if (!labels && fill) {
        Parchment(im);
    }
    int xs[512], ys[512];
    const int nx = core::display::FrameTilePositions(w, xs, 512), ny = core::display::FrameTilePositions(h, ys, 512);
    int midRow = 0;
    for (int r = 0; r < ny; ++r) {
        int k = 0;
        for (int c = 0; c < nx; ++c) {
            const bool lastC = c == nx - 1, lastR = r == ny - 1;
            int i = -1;
            if (r == 0) {
                i = c == 0 ? 0 : lastC ? 0xb : 1 + k;
            }
            else if (lastR) {
                i = c == 0 ? 0x84 : lastC ? 0x8f : 0x85 + k;
            }
            else if (c == 0) {
                i = 0xc + midRow;
            }
            else if (lastC) {
                i = 0x17 + midRow;
            }
            if (i >= 0) {
                im.Paste(i, xs[c], ys[r], labels);
            }
            if (c > 0 && !lastC) {
                k = (k + 1) % 10;
            }
        }
        if (r > 0 && r < ny - 1) {
            midRow = (midRow + 12) % 120;
        }
    }
}

// The inset panel of inset_panel_draw_tiles 0x5bf080 (7 tiles per row: corner, 5 cycling, corner; middle rows
// cycling over 5), whole tiles with the last row and column flush with the far edges.
void Inset(Image &im, int x0, int y0, int w, int h)
{
    int xs[512], ys[64];
    const int nx = core::display::FrameTilePositions(w, xs, 512), ny = core::display::FrameTilePositions(h, ys, 64);
    int mid = 0;
    for (int r = 0; r < ny; ++r) {
        int k = 0;
        for (int c = 0; c < nx; ++c) {
            const bool lastC = c == nx - 1, lastR = r == ny - 1;
            int i;
            if (r == 0) {
                i = c == 0 ? 0 : lastC ? 6 : 1 + k;
            }
            else if (lastR) {
                i = c == 0 ? 0x2a : lastC ? 0x30 : 0x2b + k;
            }
            else {
                i = c == 0 ? 7 + mid : lastC ? 0xd + mid : 8 + mid + k;
            }
            im.PasteImage(g_tiles.inset[i], kTile, kTile, x0 + xs[c], y0 + ys[r], false);
            if (c > 0 && !lastC) {
                k = (k + 1) % 5;
            }
        }
        if (r > 0 && r < ny - 1) {
            mid = (mid + 7) % 35;
        }
    }
}

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
            !g_fonts.Decode565(kFontFirst + g - 1, kKey, &gl.px)) {
            gl = Glyph{};
        }
    }
    return g_glyphs[g].w > 0 ? &g_glyphs[g] : nullptr;
}

// Draws the game's string (Windows-1252 bytes) centred in w x h at (x0, y0), as text_draw does it: a space advances
// 6 px, a letter its width; glyphs hang from a common top, characters from 0x80 that are taller than the line are
// raised by the difference (text_draw_glyph 0x5287c0). False when the font is not available.
bool DrawGameText(Image &im, const char *text, int x0, int y0, int w, int h)
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
            im.PasteImage(g->px.data(), g->w, g->h, x, top - raise, false);
            x += g->w;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// R2d.1 (docs/resolution_ui/r2d1.md): how the two kinds of front-end screens are shown

enum Look { kNative, kFramed, kBlack, kNativeCity };
const char *const kLoadingLooks[] = {"native", "framed", "black"};
const char *const kLoadingLabels[] = {"native: own size, black around", "framed: as large as fits",
                                      "black: whole frame scaled, black bars"};
const Look kLoadingLook[] = {kNative, kFramed, kBlack};
const char *const kMenuLooks[] = {"native_city", "native", "framed", "black"};
const char *const kMenuLabels[] = {"native, your city around", "native, parchment around", "framed: as large as fits",
                                   "black: whole frame scaled, black bars"};
const Look kMenuLook[] = {kNativeCity, kNative, kFramed, kBlack};
Look g_loadingLook = kNative;
Look g_menuLook = kNativeCity;

// The city background: a picture of the player's city, blurred and darkened (playtest 2026-10-25: sigma 2, first 75%,
// then 65% after the first test: too bright).
constexpr double kCitySigma = 2.0;
constexpr int kCityPercent = 65;
constexpr int kCityBrightness = kCityPercent * 256 / 100; // / 256
constexpr char kUserPicture[] = "menu_background";       // .png or .bmp in the game folder: the player's own choice
constexpr char kAutoPicture[] = "menu_background_auto.png"; // the last city seen, saved when it is left
constexpr DWORD kCityShotMs = 3000;                       // how often the city on screen is kept while playing

core::diag::Feature g_cityFeature{"city background of the front-end screens (games/emperor/fullscreen)"};

struct CityShot { // the latest city on screen (game thread)
    std::vector<WORD> px;
    int w = 0, h = 0;
    DWORD taken = 0;
    bool unsaved = false; // newer than the saved file
    int generation = 0;   // counts the pictures kept
};
CityShot g_shot;

struct Backdrop { // the background for one screen size, made on a worker thread
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<WORD> px; // w x h, RGB565 (under lock)
    int w = 0, h = 0;     // of px (under lock)
    int version = 0;      // counts finished backgrounds (under lock)
    volatile bool busy = false; // a worker is running (set by the game thread, cleared by the worker)
    int wantW = 0, wantH = 0, wantGeneration = -2; // what the last job was for (game thread)
};
Backdrop g_backdrop;
volatile LONG g_backdropReady = 0; // a background was finished since the last OnPresent

struct BackdropJob {
    int w, h;
    std::vector<WORD> shot; // a copy of the city on screen, or empty
    int shotW, shotH;
};

std::wstring GameFolder()
{
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (len > 0 && path[len - 1] != L'\\') {
        --len;
    }
    return std::wstring(path, len);
}

DWORD WINAPI MakeBackdrop(void *param)
{
    auto *job = static_cast<BackdropJob *>(param);
    std::vector<WORD> src;
    int sw = 0, sh = 0;
    const char *from = nullptr;
    const std::wstring folder = GameFolder();
    for (const wchar_t *ext : {L".png", L".bmp"}) { // the player's own picture first
        std::wstring path = folder + std::wstring(kUserPicture, kUserPicture + sizeof(kUserPicture) - 1) + ext;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
            core::assets::LoadPicture565(path.c_str(), job->w, job->h, &src, &sw, &sh)) {
            from = "menu_background (the player's picture)";
            break;
        }
    }
    if (!from && !job->shot.empty()) {
        src.swap(job->shot);
        sw = job->shotW;
        sh = job->shotH;
        from = "the city seen last in this run";
    }
    if (!from) {
        std::wstring path = folder + std::wstring(kAutoPicture, kAutoPicture + sizeof(kAutoPicture) - 1);
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
            core::assets::LoadPicture565(path.c_str(), job->w, job->h, &src, &sw, &sh)) {
            from = kAutoPicture;
        }
    }
    std::vector<WORD> out;
    if (from) {
        const DWORD start = GetTickCount();
        core::display::BlurredBackdrop(core::display::Pixels16{src.data(), sw, sw, sh}, job->w, job->h, kCitySigma,
                                       kCityBrightness, &out);
        core_log("fullscreen: city background %dx%d from %s (%dx%d), blur %.0f, brightness %d%%, %lu ms", job->w,
                 job->h, from, sw, sh, kCitySigma, kCityPercent, GetTickCount() - start);
    }
    else {
        core_log("fullscreen: no city picture yet (no %s.png/.bmp, no city seen, no %s): parchment around the menus",
                 kUserPicture, kAutoPicture);
    }
    AcquireSRWLockExclusive(&g_backdrop.lock);
    InterlockedExchange(&g_backdropReady, 1); // put in place at the next present (OnPresent)
    g_backdrop.px.swap(out);
    g_backdrop.w = g_backdrop.px.empty() ? 0 : job->w;
    g_backdrop.h = g_backdrop.px.empty() ? 0 : job->h;
    ++g_backdrop.version;
    ReleaseSRWLockExclusive(&g_backdrop.lock);
    delete job;
    return 0;
}

DWORD WINAPI BackdropThread(void *param)
{
    DWORD r = MakeBackdrop(param);
    g_backdrop.busy = false;
    return r;
}

// Game thread: makes sure a background for w x h from the newest source is made or being made.
void RequestBackdrop(int w, int h)
{
    if (w <= 0 || h <= 0 || !core::diag::Enabled(g_cityFeature) || g_backdrop.busy) {
        return;
    }
    if (g_backdrop.wantW == w && g_backdrop.wantH == h && g_backdrop.wantGeneration == g_shot.generation) {
        return;
    }
    auto *job = new BackdropJob{w, h, g_shot.px, g_shot.w, g_shot.h};
    g_backdrop.wantW = w;
    g_backdrop.wantH = h;
    g_backdrop.wantGeneration = g_shot.generation;
    g_backdrop.busy = true;
    HANDLE th = CreateThread(nullptr, 0, &BackdropThread, job, 0, nullptr);
    if (!th) {
        MakeBackdrop(job);
        g_backdrop.busy = false;
        return;
    }
    CloseHandle(th);
}

// From scroll's city_render hook (game thread): keeps the city on screen every few seconds.
void KeepCity(const BYTE *back, int pitch, int left, int top, int right, int bottom)
{
    const DWORD now = GetTickCount();
    if (!core::diag::Enabled(g_cityFeature) || (g_shot.taken && now - g_shot.taken < kCityShotMs)) {
        return;
    }
    struct Args {
        const BYTE *back;
        int pitch, left, top, right, bottom;
    } a{back, pitch, left, top, right, bottom};
    core::diag::Guarded(
        g_cityFeature,
        [](void *p) {
            const Args &a = *static_cast<Args *>(p);
            const int w = a.right - a.left, h = a.bottom - a.top;
            g_shot.px.resize(static_cast<size_t>(w) * h);
            for (int y = 0; y < h; ++y) {
                std::memcpy(g_shot.px.data() + static_cast<size_t>(y) * w,
                            a.back + static_cast<size_t>(a.top + y) * a.pitch + a.left * 2, static_cast<size_t>(w) * 2);
            }
            g_shot.w = w;
            g_shot.h = h;
            g_shot.unsaved = true;
        },
        &a);
    g_shot.taken = now;
}

// A front-end screen comes up (game thread): the city seen last becomes the background, and is saved for the next run.
// wait: written before returning (the game is ending: File > Exit game from the city goes straight out).
void CityLeft(bool wait = false)
{
    if (!g_shot.unsaved || g_shot.px.empty()) {
        return;
    }
    g_shot.unsaved = false;
    ++g_shot.generation;
    core::capture::SaveAs(g_shot.px.data(), g_shot.w * 2, g_shot.w, g_shot.h, kAutoPicture, wait);
}

int g_composed[8] = {0, 0, 0, 0, -1, -1, -1, -1}; // screen w, h, block w, h, labels, look, loading, background
char g_composedText[256] = "";
bool g_textDrawn = false; // the last background has the status line in it

// Background for a framed block of pw x (ph + below) units: the picture, and when `below` > 0 the loading screen's
// status panel under it with `text` (the game's bytes) in the game's font. Outside the frame (R2d.1): parchment;
// black for a loading screen at its own size; for kNativeCity the city background with the border along the screen
// edges, parchment until that background is ready.
void Compose(SIZE screen, int pw, int ph, int below, const char *text, bool labels, Look look, bool loading)
{
    const bool native = look == kNative || look == kNativeCity;
    int cityVersion = -1;
    if (look == kNativeCity && !labels && core::diag::Enabled(g_cityFeature)) {
        RequestBackdrop(screen.cx, screen.cy);
        AcquireSRWLockShared(&g_backdrop.lock);
        if (g_backdrop.w == screen.cx && g_backdrop.h == screen.cy) {
            cityVersion = g_backdrop.version;
        }
        ReleaseSRWLockShared(&g_backdrop.lock);
    }
    const int key[8] = {screen.cx, screen.cy, pw, ph + below, labels, look, loading, cityVersion};
    if ((std::memcmp(key, g_composed, sizeof(key)) == 0 && std::strcmp(text, g_composedText) == 0) || !LoadTiles()) {
        return;
    }
    FrameLayout l{};
    if (!core::display::ComputeFrameLayout(screen.cx, screen.cy, pw, ph + below, &l, native)) {
        return;
    }
    Image outer(l.outerU, l.outerV, kLabelOut), frame(l.frameU, l.frameV, kLabelPad);
    const char *around = "parchment";
    if (!labels) {
        bool city = false;
        if (l.native && cityVersion >= 0) { // units are screen pixels at scale 1
            AcquireSRWLockShared(&g_backdrop.lock);
            if (g_backdrop.w == screen.cx && g_backdrop.h == screen.cy) {
                for (int y = 0; y < screen.cy && y < outer.h; ++y) {
                    std::memcpy(outer.px.data() + static_cast<size_t>(y) * outer.w,
                                g_backdrop.px.data() + static_cast<size_t>(y) * screen.cx,
                                static_cast<size_t>(std::min<int>(screen.cx, outer.w)) * 2);
                }
                city = true;
            }
            ReleaseSRWLockShared(&g_backdrop.lock);
        }
        if (city) {
            Frame(outer, false, false, screen.cx, screen.cy); // the border along the screen edges
            around = "the city background";
        }
        else if (l.native && loading) {
            std::fill(outer.px.begin(), outer.px.end(), WORD{0});
            around = "black";
        }
        else {
            Parchment(outer);
        }
    }
    Frame(frame, labels);
    g_textDrawn = false;
    if (below > 0 && !labels) { // the block starts inside the border and the padding
        const int bx = kTile + l.padU, by = kTile + l.padV + ph + kGapAbove;
        Inset(frame, bx, by, pw, kInsetHeight);
        g_textDrawn = text[0] && DrawGameText(frame, text, bx, by, pw, kInsetHeight);
    }
    core::output::SetBackground(screen.cx, screen.cy, pw, ph + below, native, outer.px.data(), outer.w * 2,
                                frame.px.data(), frame.w * 2);
    std::memcpy(g_composed, key, sizeof(key));
    std::snprintf(g_composedText, sizeof(g_composedText), "%s", text);
    core_log("fullscreen: background for a %dx%d block (picture %dx%d) on %ldx%ld, %s, %s around: outer %dx%d "
             "units, frame %dx%d units%s%s",
             pw, ph + below, pw, ph, screen.cx, screen.cy, l.native ? "own size" : "as large as fits", around,
             l.outerU, l.outerV, l.frameU, l.frameV,
             below > 0 ? (g_textDrawn ? ", status panel with the line in the game's font" : ", status panel") : "",
             labels ? " (labels)" : "");
}

// ---------------------------------------------------------------------------------------------
// The picture: the whole 1024x768 frame, or a loading picture's real area

RECT g_picture{0, 0, 0, 0}; // empty: the whole frame
DWORD g_stageStart = 0;     // debug.loading_stage_ms: when the current loading stage's picture was loaded ...
int g_stageLines = 0;       // ... and how many status lines it has drawn (the loader draws one before and one after
                            // presenting the stage)
bool g_pending = false;     // a loading picture whose real area is still to be found ...
int g_pendingX = 0, g_pendingY = 0, g_pendingIndex = 0; // ... drawn at (x, y)
void Update();

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

// Finds the pending loading picture's real area; logs the result once.
void DetectPending(const char *when)
{
    if (!g_pending) {
        return;
    }
    RECT r{};
    bool found = FindRealArea(g_pendingX, g_pendingY, 800, 600, &r);
    g_picture = r;
    if (found) {
        g_pending = false;
    }
    core_log("fullscreen: loading picture %d at (%d,%d), %s: real area [%ld,%ld,%ld,%ld] (%ldx%ld)%s", g_pendingIndex,
             g_pendingX, g_pendingY, when, r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top,
             found ? "" : " -- not drawn yet, the whole 800x600 meanwhile");
}

// The loader's status line, drawn by the output (runs guarded; see TextGdi_detour).
// The loader's status line: the game's bytes (for its font) and UTF-8 (for the output's text layer).
char g_status[256] = "";
char g_statusUtf8[512] = "";

void StatusLine(const char *text)
{
    std::snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
    emperor::text::GameToUtf8(g_status, g_statusUtf8, sizeof(g_statusUtf8));
    DetectPending("at its status line"); // the loader draws the line right after the picture, before presenting
    Update();                            // framed: composes the status panel with the line in the game's font
    static long logs = 0;
    if (++logs <= 12) {
        core_log("fullscreen: status line \"%s\" (%s)", g_statusUtf8,
                 g_textDrawn ? "in the status panel, game font" : "drawn by the output instead of GDI");
    }
}

// The feature was switched off by its guard: the game's own behaviour from now on.
void FallBack()
{
    core::display::SetFraming(core::output::Framing{});
    core::output::SetCaption("", 0);
}

void __cdecl TextGdi_detour(const char *text)
{
    if (!core::diag::Guarded(g_feature, [](void *t) { StatusLine(static_cast<const char *>(t)); },
                             const_cast<char *>(text))) {
        FallBack();
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

void Update()
{
    const core_config *cfg = core_config_get();
    SIZE mode = core::display::ModeSize();
    const bool loading = g_picture.right > g_picture.left;
    const Look look = loading ? g_loadingLook : g_menuLook;
    const bool on = g_installed && core::diag::Enabled(g_feature) && look != kBlack &&
                    emperor::resolution::FrontEnd() && *kScreenMode == 3 && mode.cx == 1024 && mode.cy == 768 &&
                    *kGameScreen != kScreenVideo;
    core::output::Framing f{};
    f.on = on;
    f.labels = cfg->frame_labels != 0;
    f.native = look == kNative || look == kNativeCity;
    f.picture = loading ? g_picture : RECT{0, 0, mode.cx, mode.cy};
    f.below = loading ? kBelow : 0; // the status panel under a loading picture (docs/archive/resolution_ui_r2a.md §10)
    if (on && g_menuLook == kNativeCity && !f.labels && core::diag::Enabled(g_cityFeature)) {
        // Made ahead, from the first front-end picture on (the start-up loading screens): the menus then show the
        // city at once instead of parchment for the time the picture takes to read and blur (R2d.1 follow-up).
        const SIZE client = core::display::ClientSize();
        RequestBackdrop(client.cx, client.cy);
    }
    if (on) {
        Compose(core::display::ClientSize(), f.picture.right - f.picture.left, f.picture.bottom - f.picture.top,
                f.below, g_status, f.labels, look, loading);
    }
    core::display::SetFraming(f);
    // The status line: in the status panel when the background has it (the game's font), otherwise drawn by the
    // output (unframed: where the game's GDI line went; framed without the font: on a strip over the picture).
    const bool inPanel = on && loading && g_textDrawn && std::strcmp(g_status, g_composedText) == 0;
    core::output::SetCaption(inPanel ? "" : g_statusUtf8, (*kScreenHeight - 16) / 2 + 250);
}

struct ArtLoaded {
    int index, x, y;
};

void ArtLoadedBody(const ArtLoaded &a)
{
    if (core::diag::Enabled(g_cityFeature) && emperor::resolution::FrontEnd()) { // not the China map's picture
        core::diag::Guarded(g_cityFeature, [](void *) { CityLeft(); }, nullptr);
    }
    if (a.index >= kLoadFirst && a.index <= kLoadLast) {
        g_pending = true;
        g_pendingX = a.x;
        g_pendingY = a.y;
        g_pendingIndex = a.index;
        g_stageStart = GetTickCount();
        g_stageLines = 0;
        DetectPending("after loading");
    }
    else {
        g_pending = false;
        g_stageStart = 0;
        g_picture = RECT{0, 0, 0, 0};
        g_status[0] = g_statusUtf8[0] = '\0'; // the loading is over: no status line any more
    }
    Update();
}

} // namespace

void OnArtLoaded(int index, int x, int y)
{
    if (!g_installed || !core::diag::Enabled(g_feature)) {
        return;
    }
    ArtLoaded a{index, x, y};
    if (!core::diag::Guarded(g_feature, [](void *p) { ArtLoadedBody(*static_cast<ArtLoaded *>(p)); }, &a)) {
        FallBack();
    }
}

void OnGameEnd()
{
    if (g_installed && core::diag::Enabled(g_cityFeature)) {
        core::diag::Guarded(g_cityFeature, [](void *) { CityLeft(true); }, nullptr);
    }
}

void OnPresent()
{
    if (g_installed && InterlockedExchange(&g_backdropReady, 0)) {
        OnLoop(); // the menus have their own loop, without game_loop_step: compose here
    }
}

void OnLoop()
{
    if (!g_installed || !core::diag::Enabled(g_feature)) {
        return;
    }
    if (!core::diag::Guarded(g_feature, [](void *) { Update(); }, nullptr)) {
        FallBack();
    }
}

template <typename T, size_t N> int IndexOf(const char *const (&names)[N], const char *v, int fallback)
{
    for (size_t i = 0; i < N; ++i) {
        if (std::strcmp(names[i], v) == 0) {
            return static_cast<int>(i);
        }
    }
    return fallback;
}

// [display] fullscreen_pictures (R2a: framed | black) became loading_screen and menu_screens (R2d.1). Only black, a
// choice the player made, is carried over; framed was the default of the time, so the new defaults apply.
int MigrateFullscreenPictures(const char *old, core::settings::Migration *out, int max)
{
    if (_stricmp(old, "black") != 0 || max < 2) {
        return 0;
    }
    out[0] = core::settings::Migration{"display", "loading_screen", "black"};
    out[1] = core::settings::Migration{"display", "menu_screens", "black"};
    return 2;
}

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    core::settings::Retire("display", "fullscreen_pictures", &MigrateFullscreenPictures);
    int i = core::settings::Register(Item{
        "display", "loading_screen", "Loading screens",
        "The pictures shown while a game loads. native: their own size, centered, black around; framed: as large as "
        "fits, in a fret border on parchment; black: the whole 1024x768 frame scaled to fit, black bars.",
        Type::Choice, "native", Effect::Live, true, true, 0, 0, 0, nullptr, kLoadingLooks, kLoadingLabels, 3});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_loadingLook = kLoadingLook[IndexOf<Look>(kLoadingLooks, v, 0)];
        if (g_installed) {
            OnLoop();
        }
        return true;
    });
    i = core::settings::Register(Item{
        "display", "menu_screens", "Menu screens",
        "Main menu, emperor and campaign choice, briefings, high scores, victory and defeat (the game's 1024x768 "
        "pictures). native_city: their own size in a fret border, your city blurred around them (menu_background.png "
        "or .bmp in the game folder if you put one there, otherwise the city you saw last); native: their own size on "
        "parchment; framed: as large as fits; black: the whole frame scaled, black bars.",
        Type::Choice, "native_city", Effect::Live, true, true, 0, 0, 0, nullptr, kMenuLooks, kMenuLabels, 4});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_menuLook = kMenuLook[IndexOf<Look>(kMenuLooks, v, 0)];
        if (g_installed) {
            OnLoop();
        }
        return true;
    });
}

bool Attach()
{
    if (!Matches(kTextGdi)) {
        core_log("fullscreen: %s at 0x%lx does not match; framed pictures not installed", kTextGdi.name,
                 kTextGdi.address);
        return false;
    }
    Install(kTextGdi, &TextGdi_detour, o_TextGdi);
    g_installed = true;
    core::settings::OnChange(core::settings::Find("debug", "frame_labels"),
                             [](int, const char *, bool) { OnLoop(); return true; });
    emperor::scroll::SetCityDrawn(&KeepCity);
    core_log("fullscreen: installed (display.loading_screen %s, display.menu_screens %s; the loading screens' status "
             "line is drawn by the output; panel tiles from DATA's China_Interface pack)",
             kLoadingLooks[g_loadingLook == kNative ? 0 : g_loadingLook == kFramed ? 1 : 2],
             kMenuLooks[g_menuLook == kNativeCity ? 0 : g_menuLook == kNative ? 1 : g_menuLook == kFramed ? 2 : 3]);
    return true;
}

bool BorderPicture(int w, int h, std::vector<WORD> *out)
{
    if (w < 2 * kTile || h < 2 * kTile || w > 512 * kTile || h > 512 * kTile || !LoadTiles()) {
        return false;
    }
    Image im(w, h, 0);
    Frame(im, false);
    *out = std::move(im.px);
    return true;
}

} // namespace emperor::fullscreen
