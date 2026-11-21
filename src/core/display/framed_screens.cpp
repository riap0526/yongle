// Moved from Emperor's layer (games/emperor/fullscreen.cpp) for Zeus; the layout of the panel
// tiles is the same in both games (Emperor's panel_draw_tiles 0x5bed30 and inset_panel_draw_tiles 0x5bf080, Zeus's
// 0x59ee90 and 0x59f2a0): corners 0, 0xb, 0x84, 0x8f; top edge 1..10, bottom 0x85..0x8e; left 0xc + 12 r, right
// 0x17 + 12 r; inside 0xd + 12 r + c. The inset panel: 7 per row, corners 0, 6, 0x2a, 0x30.
#include "core/display/framed_screens.h"

#include "core/assets/image_file.h"
#include "core/assets/sg3.h"
#include "core/capture/screenshot.h"
#include "core/config.h"
#include "core/diag/guard.h"
#include "core/display/framing.h"
#include "core/display/resolution_switch.h"
#include "core/display/scaled_rect.h"
#include "core/display/tile_layout.h"
#include "core/display/virtual_display.h"
#include "core/game.h"
#include "core/log.h"
#include "core/output/sdl_output.h"
#include "core/platform/affinity.h"
#include "core/platform/files.h"
#include "core/settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <utility>

namespace core::display::framed {

namespace {

using core::display::FrameLayout;
using core::display::kTile;

constexpr int kPanelTiles = 0x90;
constexpr int kInsetTiles = 49;

std::optional<FramedGame> g_game;
bool g_installed = false;
core::diag::Feature g_feature{"framed full-screen pictures (core/display/framed_screens)"};

const char *Prefix()
{
    return g_game ? g_game->logPrefix : "fullscreen";
}

// ---------------------------------------------------------------------------------------------
// The panel tiles, read once from the player's own interface pack

struct Tiles {
    bool ok = false;
    bool failed = false;
    WORD px[kPanelTiles][kTile * kTile];
    WORD inset[kInsetTiles][kTile * kTile];
};
Tiles g_tiles;

bool ReadTiles(core::assets::Sg3Pack &pack, int first, int count, WORD (*out)[kTile * kTile], const char *what,
               long *transparent)
{
    std::vector<WORD> px;
    for (int i = 0; i < count; ++i) {
        int w = 0, h = 0;
        if (!pack.Size(first + i, &w, &h) || w != kTile || h != kTile || !pack.Decode565(first + i, kTransparent, &px)) {
            core_log("%s: interface pack entry %d is not a 16x16 %s tile (%dx%d): framed pictures without background",
                     Prefix(), first + i, what, w, h);
            return false;
        }
        std::memcpy(out[i], px.data(), sizeof(out[i]));
        for (WORD v : px) {
            *transparent += v == kTransparent;
        }
    }
    return true;
}

bool LoadTiles()
{
    if (g_tiles.ok || g_tiles.failed || !g_game) {
        return g_tiles.ok;
    }
    g_tiles.failed = true; // tried once
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (len > 0 && path[len - 1] != L'\\') {
        --len;
    }
    path[len] = L'\0';
    if (len + std::wcslen(g_game->interfacePack) + 1 >= MAX_PATH) {
        return false;
    }
    std::wcscat(path, g_game->interfacePack);
    const int panel = g_game->panelFirst, inset = g_game->insetFirst;
    core::assets::Sg3Pack pack;
    if (!pack.Open(path) || pack.Count() < panel + kPanelTiles || pack.Count() < inset + kInsetTiles) {
        core_log("%s: no dialog panel tiles (%ls): framed pictures without background", Prefix(), path);
        return false;
    }
    long transparent = 0;
    if (!ReadTiles(pack, panel, kPanelTiles, g_tiles.px, "panel", &transparent) ||
        !ReadTiles(pack, inset, kInsetTiles, g_tiles.inset, "inset", &transparent)) {
        return false;
    }
    g_tiles.ok = true;
    g_tiles.failed = false;
    core_log("%s: %d dialog panel and %d inset panel tiles read from %ls (entries %d..%d, %d..%d; %ld transparent "
             "pixels; in memory only)",
             Prefix(), kPanelTiles, kInsetTiles, path, panel, panel + kPanelTiles - 1, inset, inset + kInsetTiles - 1,
             transparent);
    return true;
}

// ---------------------------------------------------------------------------------------------
// The background for a layout (frame image, parchment)

int Luminance(WORD p)
{
    int r = (p >> 11) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
    return (r * 299 + g * 587 + b * 114) / 1000;
}

constexpr WORD kLabelOut = 0x001F, kLabelDark = 0xF800, kLabelBorder = 0x07E0, kLabelPad = 0xFFE0;
constexpr int kDarkLine = 45; // the border's dark lines (luminance 26 in Emperor's tiles)

void PasteTile(Canvas &im, int tile, int x, int y, bool labels)
{
    im.Paste(g_tiles.px[tile], kTile, kTile, x, y, labels);
}

// The game's panel pattern (the inside of its dialog panels) over the whole canvas.
void Pattern(Canvas &im)
{
    for (int r = 0; r * kTile < im.h; ++r) {
        for (int c = 0; c * kTile < im.w; ++c) {
            PasteTile(im, 0xd + (r % 10) * 12 + (c % 10), c * kTile, r * kTile, false);
        }
    }
}

// Border of whole tiles, the last ones flush with the far edges, over the panel pattern (or label colours). fill =
// false: the border only, over what im holds; w x h (from the top left): a border of that size instead of im's.
void Frame(Canvas &im, bool labels, bool fill = true, int w = 0, int h = 0)
{
    w = w > 0 ? w : im.w;
    h = h > 0 ? h : im.h;
    if (!labels && fill) {
        Pattern(im);
    }
    core::display::tiles::WalkNine(core::display::tiles::kPanel, 0, 0, w, h, false,
                                   [&](int i, int x, int y) { PasteTile(im, i, x, y, labels); });
}

// The inset panel (7 tiles per row: corner, 5 cycling, corner; middle rows cycling over 5), whole tiles with the last
// row and column flush with the far edges.
void Inset(Canvas &im, int x0, int y0, int w, int h)
{
    core::display::tiles::WalkNine(core::display::tiles::kInset, x0, y0, w, h, true, [&](int i, int x, int y) {
        im.Paste(g_tiles.inset[i], kTile, kTile, x, y, false);
    });
}

// ---------------------------------------------------------------------------------------------
// How the two kinds of front-end screens are shown

// Every look shows the picture at its own size times the city's scale (CityScale), or as large as fits when that is
// too large; the settings only choose the design around it. kNative: the fret border, the panel pattern around (the
// loading screens: black around); kNativeCity: the fret border, the player's city blurred around; kFramed (menus,
// as a player suggested): the fret border, black around, as the loading screens; kOriginal: the
// game's whole frame, black around and no border, as the game looks on a wide screen without us, the loading screens'
// status line where the game draws it.
enum Look { kNative, kNativeCity, kOriginal, kFramed };
const char *const kLoadingLooks[] = {"framed", "original"};
const char *const kLoadingLabels[] = {"framed (in a fret border)", "original (the game's own picture, black around)"};
const Look kLoadingLook[] = {kNative, kOriginal};
const Look kMenuLook[] = {kNativeCity, kNative, kFramed, kOriginal};
Look g_loadingLook = kNative;
Look g_menuLook = kNativeCity;

// The city background: a picture of the player's city, blurred and darkened (chosen in a playtest: sigma 2,
// 65%).
int g_citySigma = 2;
int g_cityPercent = 65;
int g_citySettings = 0; // counts changes of the pair (game thread)
constexpr char kUserPicture[] = "menu_background";       // .png or .bmp, our folder or the game folder: the player's
constexpr char kAutoPicture[] = "menu_background_auto.png"; // the last city seen, saved when it is left
constexpr DWORD kCityShotMs = 3000;                       // how often the city on screen is kept while playing

core::diag::Feature g_cityFeature{"city background of the front-end screens (core/display/framed_screens)"};

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
    bool player = false;  // px is from the player's own menu_background picture (under lock)
    std::atomic<bool> busy{false}; // a worker is running (set by the game thread, cleared by the worker)
    int wantW = 0, wantH = 0, wantGeneration = -2, wantSettings = -1; // what the last job was for (game thread)
};
Backdrop g_backdrop;
volatile LONG g_backdropReady = 0; // a background was finished since the last OnPresent
int g_cityFrames = 0;              // counts the city frames given to KeepCity (game thread; CityFramesDrawn)

struct BackdropJob {
    int w, h;
    std::vector<WORD> shot; // a copy of the city on screen, or empty
    int shotW, shotH;
    int sigma, percent;
    const char *prefix;
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
    bool player = false;
    // Our files' folder (core/platform/files: the game folder, or Documents\My Games\<product> when the game folder is
    // protected); the player's own picture may also be in the game folder.
    const std::wstring folder = core_files_folder();
    const std::wstring game = GameFolder();
    for (const std::wstring &where : {folder, game}) {
        for (const wchar_t *ext : {L".png", L".bmp"}) { // the player's own picture first
            std::wstring path = where + std::wstring(kUserPicture, kUserPicture + sizeof(kUserPicture) - 1) + ext;
            if (!player && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
                core::assets::LoadPicture565(path.c_str(), job->w, job->h, &src, &sw, &sh)) {
                from = "menu_background (the player's picture)";
                player = true;
            }
        }
        if (player || where == game) {
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
        core::display::BlurredBackdrop(core::display::Pixels16{src.data(), sw, sw, sh}, job->w, job->h, job->sigma,
                                       job->percent * 256 / 100, &out);
        core_log("%s: city background %dx%d from %s (%dx%d), blur %d, brightness %d%%, %lu ms", job->prefix, job->w,
                 job->h, from, sw, sh, job->sigma, job->percent, GetTickCount() - start);
    }
    else {
        core_log("%s: no city picture yet (no %s.png/.bmp, no city seen, no %s): the panel pattern around the menus",
                 job->prefix, kUserPicture, kAutoPicture);
    }
    AcquireSRWLockExclusive(&g_backdrop.lock);
    InterlockedExchange(&g_backdropReady, 1); // put in place at the next present (OnPresent) or loop
    g_backdrop.px.swap(out);
    g_backdrop.w = g_backdrop.px.empty() ? 0 : job->w;
    g_backdrop.h = g_backdrop.px.empty() ? 0 : job->h;
    g_backdrop.player = player && !g_backdrop.px.empty();
    ++g_backdrop.version;
    ReleaseSRWLockExclusive(&g_backdrop.lock);
    delete job;
    return 0;
}

DWORD WINAPI BackdropThread(void *param)
{
    core::platform::affinity::OurThread("city background");
    DWORD r = MakeBackdrop(param);
    g_backdrop.busy = false;
    return r;
}

// Game thread: makes sure a background for w x h from the newest source with the current blur and brightness is made
// or being made.
void RequestBackdrop(int w, int h)
{
    if (w <= 0 || h <= 0 || !core::diag::Enabled(g_cityFeature) || g_backdrop.busy) {
        return;
    }
    if (g_backdrop.wantW == w && g_backdrop.wantH == h && g_backdrop.wantGeneration == g_shot.generation &&
        g_backdrop.wantSettings == g_citySettings) {
        return;
    }
    auto *job = new BackdropJob{w, h, g_shot.px, g_shot.w, g_shot.h, g_citySigma, g_cityPercent, Prefix()};
    g_backdrop.wantW = w;
    g_backdrop.wantH = h;
    g_backdrop.wantGeneration = g_shot.generation;
    g_backdrop.wantSettings = g_citySettings;
    g_backdrop.busy = true;
    HANDLE th = CreateThread(nullptr, 0, &BackdropThread, job, 0, nullptr);
    if (!th) {
        MakeBackdrop(job);
        g_backdrop.busy = false;
        return;
    }
    CloseHandle(th);
}

// A front-end screen comes up (game thread): the city seen last becomes the background, and is saved for the next run.
// wait: written before returning (the game is ending).
// Why no city was kept, for one summary line: written once, only when the city was
// drawn but never kept in this run, so the skips of a normal start (the area still 0 before the first layout) do not
// show.
bool g_cityKeptEver = false;
int g_citySkips = 0;
char g_citySkipWhy[96] = "";
int g_citySkipArea[4] = {0, 0, 0, 0};
bool g_citySkipLogged = false;

void SkipSummary()
{
    if (g_cityKeptEver || g_citySkips == 0 || g_citySkipLogged) {
        return;
    }
    g_citySkipLogged = true;
    core_log("%s: no city kept for the menus in this run: %d city drawings skipped (last: %s; area %d,%d-%d,%d)",
             Prefix(), g_citySkips, g_citySkipWhy, g_citySkipArea[0], g_citySkipArea[1], g_citySkipArea[2],
             g_citySkipArea[3]);
}

void CityLeft(bool wait = false)
{
    if (!g_shot.unsaved || g_shot.px.empty()) {
        return;
    }
    g_shot.unsaved = false;
    ++g_shot.generation;
    core::capture::SaveAs(g_shot.px.data(), g_shot.w * 2, g_shot.w, g_shot.h, kAutoPicture, wait);
}

int g_composed[9] = {0, 0, 0, 0, -1, -1, -1, -1, -1}; // screen w, h, block w, h, labels, look, loading, background,
                                                       // scale x 1000
char g_composedText[256] = "";
bool g_textDrawn = false; // the last background has the status line in it

// The outer fill's size in units for this layout: the city background is made at that size (one unit = one screen
// pixel at scale 1; the output enlarges it by the layout's scale).
SIZE OuterUnits(SIZE screen, int pw, int bh, double scale)
{
    FrameLayout l{};
    if (!core::display::ComputeFrameLayout(screen.cx, screen.cy, pw, bh, &l, scale)) {
        return SIZE{0, 0};
    }
    return SIZE{l.outerU, l.outerV};
}

// Background for a framed block of pw x (ph + below) units: the picture, and when `below` > 0 the loading screen's
// status block under it with `text` (the game's bytes) in the game's font. Outside the frame: the panel pattern;
// black for a loading screen and kFramed; for kNativeCity the city background with the border along the screen edges, the panel
// pattern until that background is ready; for kOriginal black everywhere, no border.
void Compose(SIZE screen, int pw, int ph, int below, const char *text, bool labels, Look look, bool loading, double scale)
{
    FrameLayout l{};
    if (!core::display::ComputeFrameLayout(screen.cx, screen.cy, pw, ph + below, &l, scale)) {
        return;
    }
    int cityVersion = -1;
    if (look == kNativeCity && !labels && core::diag::Enabled(g_cityFeature)) {
        RequestBackdrop(l.outerU, l.outerV);
        AcquireSRWLockShared(&g_backdrop.lock);
        if (g_backdrop.w == l.outerU && g_backdrop.h == l.outerV) {
            cityVersion = g_backdrop.version;
        }
        ReleaseSRWLockShared(&g_backdrop.lock);
    }
    const int key[9] = {screen.cx, screen.cy, pw, ph + below, labels, look, loading, cityVersion,
                        static_cast<int>(scale * 1000 + 0.5)};
    if ((std::memcmp(key, g_composed, sizeof(key)) == 0 && std::strcmp(text, g_composedText) == 0) || !LoadTiles()) {
        return;
    }
    Canvas outer(l.outerU, l.outerV, kLabelOut), frame(l.frameU, l.frameV, kLabelPad);
    const char *around = "the panel pattern";
    const bool plain = look == kOriginal && !labels;
    if (plain) {
        std::fill(outer.px.begin(), outer.px.end(), WORD{0});
        std::fill(frame.px.begin(), frame.px.end(), WORD{0});
        around = "black, no border (original)";
    }
    else if (!labels) {
        bool city = false;
        if (cityVersion >= 0) { // made at the outer fill's size (OuterUnits)
            AcquireSRWLockShared(&g_backdrop.lock);
            if (g_backdrop.w == outer.w && g_backdrop.h == outer.h) {
                std::memcpy(outer.px.data(), g_backdrop.px.data(), outer.px.size() * 2);
                city = true;
            }
            ReleaseSRWLockShared(&g_backdrop.lock);
        }
        if (city) {
            // The border along the screen edges: the screen in units (the last unit may be partly off the screen).
            Frame(outer, false, false, static_cast<int>(screen.cx / l.s), static_cast<int>(screen.cy / l.s));
            around = "the city background";
        }
        else if (loading || look == kFramed) {
            std::fill(outer.px.begin(), outer.px.end(), WORD{0});
            around = "black";
        }
        else {
            Pattern(outer);
        }
    }
    if (!plain) {
        Frame(frame, labels);
    }
    g_textDrawn = false;
    const StatusBlock &st = g_game->status;
    if (below > 0 && !labels) { // the block starts inside the border and the padding
        const int bx = kTile + l.padU, by = kTile + l.padV + ph + st.gapAbove;
        Inset(frame, bx, by, pw, st.insetHeight);
        g_textDrawn = text[0] && st.draw && st.draw(frame, text, bx, by, pw, st.insetHeight);
    }
    core::output::SetBackground(screen.cx, screen.cy, pw, ph + below, scale, outer.px.data(), outer.w * 2,
                                frame.px.data(), frame.w * 2);
    std::memcpy(g_composed, key, sizeof(key));
    std::snprintf(g_composedText, sizeof(g_composedText), "%s", text);
    core_log("%s: background for a %dx%d block (picture %dx%d) on %ldx%ld, %s %.2f, %s around: outer %dx%d units, "
             "frame %dx%d units%s%s",
             Prefix(), pw, ph + below, pw, ph, screen.cx, screen.cy,
             l.native ? "at the city's scale" : "as large as fits (the city's scale is larger):", l.s, around, l.outerU,
             l.outerV, l.frameU, l.frameV,
             below > 0 ? (g_textDrawn ? ", status block with the line in the game's font" : ", status block") : "",
             labels ? " (labels)" : "");
}

// ---------------------------------------------------------------------------------------------
// The picture: the whole frame, or a loading picture's area

RECT g_picture{0, 0, 0, 0}; // empty: the whole frame
char g_status[256] = "";    // the loader's status line: the game's bytes (for its font) ...
char g_statusUtf8[512] = ""; // ... and UTF-8 (for the output's text layer)

// The feature was switched off by its guard: the game's own behaviour from now on.
void FallBack()
{
    core::display::SetFraming(core::output::Framing{});
    core::output::SetCaption("", 0);
}

// The menus' look on this screen: city becomes the panel pattern where the game says so, unless the background is
// the player's own picture.
Look MenuLook()
{
    if (g_menuLook != kNativeCity) {
        return g_menuLook;
    }
    AcquireSRWLockShared(&g_backdrop.lock);
    const bool player = g_backdrop.player;
    ReleaseSRWLockShared(&g_backdrop.lock);
    const unsigned char screen = *g_game->gameScreen;
    const bool city = g_game->cityAround(screen, player);
    static int logged = -1;
    if (!city && screen != logged) {
        logged = screen;
        core_log("%s: screen 0x%02x: the panel pattern around instead of the city (before a mission or an editor)",
                 Prefix(), screen);
    }
    return city ? kNativeCity : kNative;
}

void Update()
{
    const core_config *cfg = core_config_get();
    const SIZE mode = core::display::ModeSize();
    const int nativeW = core_game()->native_width, nativeH = core_game()->native_height;
    const bool loading = g_picture.right > g_picture.left;
    const Look look = loading ? g_loadingLook : MenuLook();
    const bool on = g_installed && core::diag::Enabled(g_feature) && core::display::resolution::FrontEnd() &&
                    *g_game->screenMode == 3 && mode.cx == nativeW && mode.cy == nativeH &&
                    *g_game->gameScreen != g_game->videoScreen;
    const StatusBlock &st = g_game->status;
    core::output::Framing f{};
    f.on = on;
    f.labels = cfg->frame_labels != 0;
    const bool whole = look == kOriginal; // the whole frame, also for a loading picture (the game's own bands)
    f.scale = core::display::resolution::CityScale(); // own size times the city's scale
    f.picture = loading && !whole ? g_picture : RECT{0, 0, mode.cx, mode.cy};
    f.below = loading && !whole ? st.Below() : 0; // the status block under a loading picture
    f.gameCaption = whole && st.captionY != nullptr;
    if (on && g_menuLook == kNativeCity && !f.labels && core::diag::Enabled(g_cityFeature)) {
        // Made ahead, from the first front-end picture on (the start-up loading screens): the menus then show the
        // city at once instead of the panel pattern for the time the picture takes to read and blur.
        const SIZE units = OuterUnits(core::display::ClientSize(), f.picture.right - f.picture.left,
                                      f.picture.bottom - f.picture.top + f.below, f.scale);
        RequestBackdrop(units.cx, units.cy);
    }
    if (on) {
        Compose(core::display::ClientSize(), f.picture.right - f.picture.left, f.picture.bottom - f.picture.top,
                f.below, g_status, f.labels, look, loading, f.scale);
    }
    core::display::SetFraming(f);
    if (st.captionY) {
        // The status line: in the status block when the background has it (the game's font), otherwise drawn by the
        // output (unframed: where the game's own line went; framed without the font: on a strip over the picture).
        const bool inBlock = on && loading && g_textDrawn && std::strcmp(g_status, g_composedText) == 0;
        core::output::SetCaption(inBlock ? "" : g_statusUtf8, st.captionY());
    }
}

void LoopBody(void *)
{
    Update();
}

// What OnPresent last saw (game thread).
int g_presentScreen = -1;
SIZE g_presentClient{0, 0};
int g_presentScale = 0;

template <size_t N> int IndexOf(const char *const (&names)[N], const char *v, int fallback)
{
    for (size_t i = 0; i < N; ++i) {
        if (std::strcmp(names[i], v) == 0) {
            return static_cast<int>(i);
        }
    }
    return fallback;
}

template <size_t N> const char *LookName(const char *const (&names)[N], const Look (&looks)[N], Look look)
{
    for (size_t i = 0; i < N; ++i) {
        if (looks[i] == look) {
            return names[i];
        }
    }
    return "?";
}

} // namespace

void Canvas::Paste(const WORD *src, int sw, int sh, int x, int y, bool labels)
{
    for (int ty = 0; ty < sh; ++ty) {
        for (int tx = 0; tx < sw; ++tx) {
            WORD p = src[ty * sw + tx];
            int dx = x + tx, dy = y + ty;
            if (p == kTransparent || dx < 0 || dy < 0 || dx >= w || dy >= h) {
                continue;
            }
            px[static_cast<size_t>(dy) * w + dx] = labels ? (Luminance(p) < kDarkLine ? kLabelDark : kLabelBorder) : p;
        }
    }
}

const char *const kMenuChoices[4] = {"city", "panel", "framed", "original"};
const char *const kMenuLabels[4] = {"city (your city around)", "panel (the game's panel pattern around)",
                                    "framed (black around the border)",
                                    "original (the game's own picture, black around)"};

bool MenuScreensChanged(int, const char *value, bool)
{
    g_menuLook = kMenuLook[IndexOf(kMenuChoices, value, 0)];
    if (g_installed) {
        OnLoop();
    }
    return true;
}

void RegisterSettings(void (*registerMenuScreens)())
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "video", "loading_screen", "Loading screens",
        "The pictures shown while a game loads, at the display scale. framed: in a fret border; original: the game's "
        "own picture, black around.",
        Type::Choice, "framed", Effect::Live, true, true, 0, 0, 0, nullptr, kLoadingLooks, kLoadingLabels, 2});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_loadingLook = kLoadingLook[IndexOf(kLoadingLooks, v, 0)];
        if (g_installed) {
            OnLoop();
        }
        return true;
    });
    registerMenuScreens();
    // The menus' city background's blur and brightness (menu_screens = city). Made again on a worker thread.
    i = core::settings::Register(Item{
        "video", "background_blur", "City background: blur",
        "How much the picture of your city behind the menus (Menu screens = city) is blurred. 0: not blurred.",
        Type::Int, "2", Effect::Live, true, true, 0, 8, 1, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool initial) {
        g_citySigma = std::atoi(v);
        ++g_citySettings;
        if (!initial && g_installed) {
            OnLoop();
        }
        return true;
    });
    i = core::settings::Register(Item{
        "video", "background_brightness", "City background: brightness",
        "How bright the picture of your city behind the menus is, in percent of the city itself. 100 with blur 0: the "
        "city as it is.",
        Type::Int, "65", Effect::Live, true, true, 30, 100, 5, nullptr, nullptr, nullptr, 0, "%"});
    core::settings::OnChange(i, [](int, const char *v, bool initial) {
        g_cityPercent = std::atoi(v);
        ++g_citySettings;
        if (!initial && g_installed) {
            OnLoop();
        }
        return true;
    });
}

void Configure(const FramedGame &game)
{
    g_game.emplace(game);
}

void Install()
{
    if (!g_game) {
        return;
    }
    g_installed = true;
    core::settings::OnChange(core::settings::Find("debug", "frame_labels"), [](int, const char *, bool) {
        OnLoop();
        return true;
    });
    // The resolution setting (panel, Shift+F7) changes the scale: followed at once, also on a screen that is not
    // redrawn. OnLoop has its own guard: a fault here does not switch the resolution switching off.
    core::display::resolution::SetChangedListener(&OnLoop);
    core_log("%s: installed (video.loading_screen %s, video.menu_screens %s; panel tiles from the game's interface "
             "pack%s)",
             Prefix(), LookName(kLoadingLooks, kLoadingLook, g_loadingLook), LookName(kMenuChoices, kMenuLook, g_menuLook),
             g_game->status.Below() > 0 ? "; the loading screens' status line in a status block" : "");
}

bool Installed()
{
    return g_installed;
}

bool Guarded(void (*fn)(void *), void *arg)
{
    if (!g_installed || !core::diag::Enabled(g_feature)) {
        return false;
    }
    if (!core::diag::Guarded(g_feature, fn, arg)) {
        FallBack();
        return false;
    }
    return true;
}

void NoteCitySkipped(const char *why, int left, int top, int right, int bottom)
{
    ++g_citySkips;
    std::snprintf(g_citySkipWhy, sizeof(g_citySkipWhy), "%s", why ? why : "?");
    g_citySkipArea[0] = left;
    g_citySkipArea[1] = top;
    g_citySkipArea[2] = right;
    g_citySkipArea[3] = bottom;
}

void PictureShown(const RECT *loading)
{
    if (core::display::resolution::FrontEnd()) {
        SkipSummary(); // the menus would show the city now
    }
    if (core::diag::Enabled(g_cityFeature) && core::display::resolution::FrontEnd()) {
        core::diag::Guarded(g_cityFeature, [](void *) { CityLeft(); }, nullptr);
    }
    if (loading) {
        g_picture = *loading;
    }
    else {
        g_picture = RECT{0, 0, 0, 0};
        g_status[0] = g_statusUtf8[0] = '\0'; // the loading is over: no status line any more
    }
    Update();
}

void SetLoadingArea(const RECT &area)
{
    g_picture = area;
}

void SetStatus(const char *gameText, const char *utf8)
{
    std::snprintf(g_status, sizeof(g_status), "%s", gameText ? gameText : "");
    std::snprintf(g_statusUtf8, sizeof(g_statusUtf8), "%s", utf8 ? utf8 : "");
    Update();
}

bool StatusInBlock()
{
    return g_textDrawn;
}

void OnLoop()
{
    if (!g_installed || !core::diag::Enabled(g_feature)) {
        return;
    }
    if (!core::diag::Guarded(g_feature, &LoopBody, nullptr)) {
        FallBack();
    }
}

void OnPresent()
{
    if (!g_installed || !core::diag::Enabled(g_feature)) {
        return;
    }
    const int screen = *g_game->gameScreen;
    const bool ready = InterlockedExchange(&g_backdropReady, 0) != 0;
    const SIZE client = core::display::ClientSize();
    const int scale = static_cast<int>(core::display::resolution::CityScale() * 1000 + 0.5);
    if (ready || screen != g_presentScreen || client.cx != g_presentClient.cx || client.cy != g_presentClient.cy ||
        scale != g_presentScale) { // a new screen may want the panel pattern instead of the city (MenuLook)
        g_presentScreen = screen;
        g_presentClient = client;
        g_presentScale = scale;
        OnLoop();
    }
}

void KeepCity(const BYTE *back, int pitch, int left, int top, int right, int bottom)
{
    ++g_cityFrames;
    const DWORD now = GetTickCount();
    if (!g_installed || !core::diag::Enabled(g_cityFeature) || (g_shot.taken && now - g_shot.taken < kCityShotMs)) {
        return;
    }
    if (!back || left < 0 || top < 0 || right <= left || bottom <= top || right * 2 > pitch) {
        NoteCitySkipped(back ? "the city's area is not usable" : "no draw buffer", left, top, right, bottom);
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
            g_cityKeptEver = true;
        },
        &a);
    g_shot.taken = now;
}

void OnGameEnd()
{
    if (g_installed) {
        SkipSummary();
    }
    if (g_installed && core::diag::Enabled(g_cityFeature)) {
        core::diag::Guarded(g_cityFeature, [](void *) { CityLeft(true); }, nullptr);
    }
}

int CityFramesDrawn()
{
    return g_cityFrames;
}

bool BorderPicture(int w, int h, std::vector<WORD> *out)
{
    if (w < 2 * kTile || h < 2 * kTile || w > 512 * kTile || h > 512 * kTile || !LoadTiles()) {
        return false;
    }
    Canvas im(w, h, 0);
    Frame(im, false);
    *out = std::move(im.px);
    return true;
}

void ResetForTest()
{
    g_game.reset();
    g_installed = false;
    g_tiles.ok = g_tiles.failed = false;
    g_loadingLook = kNative;
    g_menuLook = kNativeCity;
    std::memset(g_composed, 0, sizeof(int) * 4);
    for (int i = 4; i < 9; ++i) {
        g_composed[i] = -1;
    }
    g_composedText[0] = '\0';
    g_picture = RECT{0, 0, 0, 0};
    g_status[0] = g_statusUtf8[0] = '\0';
    g_presentScreen = -1;
    g_presentClient = SIZE{0, 0};
    g_presentScale = 0;
    g_cityKeptEver = false;
    g_citySkips = 0;
    g_citySkipWhy[0] = '\0';
    g_citySkipLogged = false;
}

} // namespace core::display::framed
