// See city_picture.h. Evidence for every address: docs/archive/city_export.md sections 9 and 10, docs/symbols.md.
#include "games/emperor/city_picture.h"

#include "core/capture/big_picture.h"
#include "core/capture/screenshot.h"
#include "core/diag/guard.h"
#include "core/display/city_strips.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/output/sdl_output.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"
#include "core/ui/panel.h"
#include "core/ui/settings_view.h"
#include "games/emperor/scroll.h"
#include "games/emperor/site.h"
#include "games/emperor/zoom.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace emperor::city_picture {

namespace {

// minimap_draw(x, y, w, h): __cdecl, the caller pops (tools/recon/callconv.py; city_export.md section 10.2)
const Site kMinimapDraw{"minimap_draw", 0x46e730, {0xa1, 0xdc, 0x6d, 0x15, 0x01, 0x8b, 0x4c, 0x24}};

const Var<BYTE *> kDrawBuffer{0x1156df8}; // draw_buffer
const Var<int> kScreenPitch{0x10de000};  // bytes per row
const Var<int> kDdWidth{0x10de008};
const Var<int> kDdHeight{0x10de004};
const Var<int> kClipLeft{0xadf89c};      // the city's clip (viewport_init)
const Var<int> kClipTop{0xadf894};
const Var<int> kClipRight{0xadf898};     // exclusive
const Var<int> kClipBottom{0xadf890};    // exclusive
const Var<int> kWidthTiles{0xadf8c4};
const Var<int> kHeightRows{0xadf8c0};
const Var<int> kTileWidth{0xadf8b8};
const Var<int> kTileHalfHeight{0xadf8ac};
const Var<int> kOriginX{0xadf8f0};
const Var<int> kOriginY{0xadf8ec};
const Var<int> kViewDrawnX{0xadf8d8};    // city_render's vx / vy (mouse_to_grid and the minimap read them)
const Var<int> kViewDrawnY{0xadf8d4};
const Var<int> kOverlay{0x115f738};      // the overlay shown (0 none)
const Var<int> kPreviewShown{0x101d0f0}; // build_preview_draw draws only when this is not 0
const Var<int> kMarkers{0xc05804};       // city_render_markers' debug markers
const Var<int> kRenderCount{0xb53a1c};   // city_render counts its calls
const Var<int> kCountdown{0xb46a70};     // city_render counts it down
const Var<char> kGameScreen{0x10de045};
constexpr DWORD kTimerFlags = 0x8901cc;    // frame_timer_flags: 51 bytes
constexpr int kTimerFlagCount = 51;
constexpr DWORD kAmbientCounts = 0x14019f4; // the tile counts around the view's middle: 10 ints
constexpr int kAmbientCountCount = 10;
constexpr DWORD kMinimapState = 0xadf810;   // minimap_draw's position, size and origin: 12 ints
constexpr int kMinimapStateCount = 12;
constexpr DWORD kScreenClip = 0x10c7318;    // screen_clip_rect: 4 ints
constexpr int kViewTableCols = 0xe7, kViewTableRows = 0x1c9;

// The player's city for the file names (sections 11.5 and 11.7) [confirmed: 0x5004be-0x5004cd, city_get 0x481f10
// (__thiscall, ecx 0xc058d0, pops its one argument), 0x592360]: its index into the empire's 22 cities is the int at
// 0x130f960 + 0x2b38, the cities are 0x3c00 bytes each from 0xc07b80. Its name: city_name_get 0x4451c0 (ecx = the
// city, no stack arguments, returns the text): +7 is the index into EmperorText.eng's group 21 (the city names), or
// 0xfe for the city's own name at +8 (32 bytes).
constexpr DWORD kPlayerCity = 0x130f960 + 0x2b38;
constexpr DWORD kCities = 0xc07b80;
constexpr int kCityStride = 0x3c00, kCityCount = 0x16;
constexpr int kCityTextAt = 7, kCityNameAt = 8, kCityNameSize = 0x20;
const Site kCityNameGet{"city_name_get", 0x4451c0, {0x8a, 0x41, 0x07, 0x3c, 0xfe, 0x75, 0x04, 0x8d}};
using CityNameGet_fn = const char *(__fastcall *)(const void *city); // ecx = city; edx unused
bool g_cityNameOk = false; // kCityNameGet matched at Attach

constexpr std::uint16_t kKey = 0xF81F; // magenta: the minimap's drawing area before it is drawn (cropped to it)
constexpr int kOverlapRows = 4;        // half rows drawn again by the next strip (section 9.2)
// No room above the view's top row (section 11.3): the game's maps are desert outside the playable range, so the half
// rows above it show desert the player never sees; the picture starts where the topmost view does.
constexpr int kTopRows = 0;

core::diag::Feature g_feature{"city picture (games/emperor/city_picture)"};
bool g_attached = false;
std::atomic<bool> g_busy{false}; // a picture is being written

enum class Kind { None, City, Minimap };
Kind g_pending = Kind::None;

core::display::ActionKey g_cityKey{0x79 /* F10 */, 0, nullptr};
core::display::ActionKey g_minimapKey{0x79, core::ui::kModShift, nullptr};
int g_minimapScale = 2;
int g_stripRows = 64;

template <typename T> T *At(DWORD address)
{
    return reinterpret_cast<T *>(address + g_imageShift);
}

void Notice(const char *text)
{
    core::output::SetNotice(text, 5000);
}

// A game text of at most size bytes, for the log: printable characters as they are, others as \xNN; "(none)" when it
// does not end within size.
void LogText(const char *what, const unsigned char *p, int size)
{
    char out[300];
    int n = 0, i = 0;
    for (; i < size && p[i] != 0 && n < static_cast<int>(sizeof(out)) - 5; ++i) {
        n += p[i] >= 0x20 && p[i] < 0x7f ? std::snprintf(out + n, sizeof(out) - n, "%c", p[i])
                                         : std::snprintf(out + n, sizeof(out) - n, "\\x%02x", p[i]);
    }
    out[n] = 0;
    core_log("picture: %s \"%s\"%s", what, out, i < size && p[i] == 0 ? "" : " (not ended)");
}

// The file names' first part (section 11.5): the player's city as the game names it, letters and digits kept, '-' for
// anything else between them; "city" when there is no city or no usable name.
void CityStem(char *out, size_t size)
{
    std::snprintf(out, size, "city");
    const int index = *At<int>(kPlayerCity);
    if (index < 0 || index >= kCityCount) {
        core_log("picture: the player's city index is %d (not 0..%d): file names start with \"city\"", index,
                 kCityCount - 1);
        return;
    }
    const auto *city = At<unsigned char>(kCities) + static_cast<size_t>(index) * kCityStride;
    const unsigned char text = city[kCityTextAt];
    if (text == 0xfe && std::memchr(city + kCityNameAt, 0, kCityNameSize) == nullptr) {
        core_log("picture: the player's city %d has its own name, not ended within %d bytes: file names start with "
                 "\"city\"",
                 index, kCityNameSize);
        return;
    }
    if (!g_cityNameOk) {
        return; // logged at Attach
    }
    struct NameCall {
        const void *city;
        const unsigned char *name;
    } call{city, nullptr};
    static core::diag::Feature nameFeature{"city name for picture file names (games/emperor/city_picture)"};
    if (!core::diag::Enabled(nameFeature) ||
        !core::diag::Guarded(nameFeature,
                             [](void *v) {
                                 auto *c = static_cast<NameCall *>(v);
                                 c->name = reinterpret_cast<const unsigned char *>(
                                     Function<CityNameGet_fn>(kCityNameGet)(c->city));
                             },
                             &call)) {
        return; // logged by the guard
    }
    const unsigned char *name = call.name;
    if (!name) {
        core_log("picture: the game gave no name for the player's city %d: file names start with \"city\"", index);
        return;
    }
    static int logged = -1;
    if (logged != index * 256 + text) { // once per city
        logged = index * 256 + text;
        char what[80];
        std::snprintf(what, sizeof(what), "the player's city is %d of the empire, name %s %u:", index,
                      text == 0xfe ? "of its own (+8), marker" : "text 21 /", static_cast<unsigned>(text));
        LogText(what, name, 64);
    }
    char stem[kCityNameSize + 1];
    size_t n = 0;
    bool gap = false;
    for (int i = 0; i < 64 && name[i] != 0 && n + 2 < sizeof(stem); ++i) {
        const unsigned char c = name[i];
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!keep) {
            gap = n > 0;
            continue;
        }
        if (gap) {
            stem[n++] = '-';
            gap = false;
        }
        stem[n++] = static_cast<char>(c);
    }
    stem[n] = 0;
    if (n == 0) {
        core_log("picture: no usable city name: file names start with \"city\"");
        return;
    }
    std::snprintf(out, size, "%s", stem);
}

bool CityScreen()
{
    const char s = *kGameScreen;
    return s == 1 || s == 7 || s == 0x48 || s == 0x49;
}

void Done(bool ok, const char *text)
{
    char line[300];
    if (ok) {
        std::snprintf(line, sizeof(line), "Picture saved: %s", text);
    }
    else {
        std::snprintf(line, sizeof(line), "The picture could not be saved (details in the log).");
    }
    Notice(line);
    g_busy = false;
}

void Request(Kind kind)
{
    const char *what = kind == Kind::City ? "city picture" : "minimap picture";
    if (!g_attached || !core::diag::Enabled(g_feature) || !emperor::scroll::Working()) {
        core_log("picture: %s key pressed, but the pictures are off (see above)", what);
        Notice("Pictures are not available now (details in the log).");
        return;
    }
    if (core::ui::IsOpen()) { // the game is paused: the city is not drawn until the panel closes
        Notice("Close the settings panel first.");
        return;
    }
    if (!CityScreen()) {
        core_log("picture: %s key pressed outside the city (screen 0x%02x)", what,
                 static_cast<unsigned char>(*kGameScreen));
        Notice("Pictures can only be taken while the city is shown.");
        return;
    }
    if (g_busy.load()) {
        core_log("picture: %s key pressed while the last picture is still being written", what);
        Notice("The last picture is still being saved.");
        return;
    }
    g_pending = kind;
    core_log("picture: %s asked for", what);
}

void FireCity()
{
    Request(Kind::City);
}

void FireMinimap()
{
    Request(Kind::Minimap);
}

// ---------------------------------------------------------------------------------------------
// The city in strips (section 4)

struct CityCall {
    core::display::StripInput in;
    core::display::StripPlan plan;
    int orientation;
    BYTE *buf;
    int pitchPx;
    core::capture::BigPicture *pic;
    core::diag::Restore r;  // the game's variables (put back by the guard when abandoned)
    core::diag::Restore r2; // the counters and our hooks' state (put back by the caller in every case)
    std::int64_t drawMs;
};

void DrawStrips(CityCall &c)
{
    const core::display::StripPlan &p = c.plan;
    c.r.Set(kDrawBuffer.ptr(), c.buf);
    c.r.Set(kScreenPitch.ptr(), c.pitchPx * 2);
    c.r.Set(kDdWidth.ptr(), c.pitchPx);
    c.r.Set(kDdHeight.ptr(), p.bufH);
    c.r.Set(kClipLeft.ptr(), 0);
    c.r.Set(kClipTop.ptr(), 0);
    c.r.Set(kClipRight.ptr(), p.clipRight);
    c.r.Set(kClipBottom.ptr(), p.clipBottom);
    c.r.Set(kWidthTiles.ptr(), p.cols + 1);
    c.r.Set(kHeightRows.ptr(), p.viewRows + 2);
    c.r.Set(kOriginX.ptr(), 0);
    c.r.Set(kOriginY.ptr(), p.topPx);
    c.r.Save(kViewDrawnX.ptr());
    c.r.Save(kViewDrawnY.ptr());
    c.r.Set(kOverlay.ptr(), 0);
    c.r.Set(kPreviewShown.ptr(), 0);
    c.r2.Set(kMarkers.ptr(), 0);
    c.r2.Save(kRenderCount.ptr());
    c.r2.Save(kCountdown.ptr());
    emperor::scroll::PictureDrawing(c.r2, p.clipRight, p.clipBottom);
    std::memset(At<BYTE>(kTimerFlags), 0, kTimerFlagCount); // no animation moves on between the strips
    const DWORD start = GetTickCount();
    for (int k = 0; k < p.count; ++k) {
        const core::display::Strip s = core::display::StripAt(c.in, p, k);
        auto *px = reinterpret_cast<std::uint16_t *>(c.buf);
        const size_t n = static_cast<size_t>(c.pitchPx) * p.bufH;
        std::memset(c.buf, 0, n * 2); // black where the game draws nothing, as on the screen
        emperor::scroll::DrawCityForPicture(c.in.left, s.viewY, c.orientation);
        for (int y = 0; y < s.rows; ++y) {
            std::memcpy(c.pic->Row(s.toY + y), px + static_cast<size_t>(s.fromY + y) * c.pitchPx,
                        static_cast<size_t>(p.imageW) * 2);
        }
    }
    c.drawMs = GetTickCount() - start;
}

// The city drawn with keepRows half rows per strip; nullptr (logged, the player told) when it could not be.
core::capture::BigPicture *DrawCity(int orientation, int keepRows)
{
    CityCall c{};
    c.orientation = orientation;
    int left, right, top, bottom, mapRows;
    emperor::zoom::ViewLimits(&left, &right, &top, &bottom, &mapRows);
    c.in = core::display::StripInput{left,          right,         top,         bottom,  mapRows, *kTileWidth + 2,
                                     *kTileHalfHeight, keepRows, kOverlapRows, kTopRows};
    if (!core::display::PlanStrips(c.in, &c.plan)) {
        core_log("picture: no city to draw (columns %d..%d, half rows %d..%d of %d, tiles %d x %d)", left, right, top,
                 bottom, mapRows, *kTileWidth, *kTileHalfHeight);
        Notice("The picture could not be saved (details in the log).");
        return nullptr;
    }
    const core::display::StripPlan &p = c.plan;
    c.pitchPx = (p.bufW + 15) & ~15;
    const size_t bytes = static_cast<size_t>(c.pitchPx) * 2 * p.bufH;
    core_log("picture: city %d x %d px (%.1f MB): map columns %d..%d, half rows %d..%d (map %d), tiles %d x %d; %d "
             "strip(s) of %d half rows kept (view %d x %d, +%d drawn again), buffer %d x %d px (%.1f MB)",
             p.imageW, p.imageH, static_cast<double>(p.imageW) * p.imageH * 2 / (1024.0 * 1024.0), left, right, top,
             bottom, mapRows, *kTileWidth, *kTileHalfHeight, p.count, p.keepRows, p.cols, p.viewRows,
             p.viewRows - p.keepRows, c.pitchPx, p.bufH, bytes / (1024.0 * 1024.0));
    c.pic = core::capture::BigPicture::Create(p.imageW, p.imageH);
    c.buf = c.pic ? static_cast<BYTE *>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
                  : nullptr;
    if (!c.buf) {
        if (c.pic) {
            core_log("picture: no memory for the drawing buffer (%.1f MB)", bytes / (1024.0 * 1024.0));
        }
        delete c.pic;
        Notice("Not enough memory for the city picture; nothing was saved.");
        return nullptr;
    }
    BYTE timers[kTimerFlagCount];
    int ambient[kAmbientCountCount];
    std::memcpy(timers, At<BYTE>(kTimerFlags), sizeof(timers));
    std::memcpy(ambient, At<int>(kAmbientCounts), sizeof(ambient));
    const bool ok = core::diag::Guarded(g_feature, [](void *v) { DrawStrips(*static_cast<CityCall *>(v)); }, &c, &c.r);
    c.r.Undo(); // the drawing's changes were for its duration only (already undone when it was abandoned)
    c.r2.Undo();
    std::memcpy(At<BYTE>(kTimerFlags), timers, sizeof(timers));
    std::memcpy(At<int>(kAmbientCounts), ambient, sizeof(ambient));
    VirtualFree(c.buf, 0, MEM_RELEASE);
    if (!ok) {
        delete c.pic;
        core_log("picture: the city picture is off for the rest of the run (an error while drawing; the game's "
                 "state was put back)");
        Notice("The picture could not be saved (details in the log).");
        return nullptr;
    }
    core_log("picture: the city drawn in %lu ms", static_cast<unsigned long>(c.drawMs));
    return c.pic;
}

void Save(core::capture::BigPicture *pic, const char *kind, int scale)
{
    core::capture::BigOptions o;
    o.kind = kind;
    o.scale = scale;
    o.transparent = false;
    o.done = &Done;
    g_busy = true;
    core::capture::SaveBig(pic, o);
}

// [debug] city_picture_seam_check: the same moment drawn a second time in as few strips as the map allows (nothing
// moves between the two: the animation timers are held still in both), saved as city-fewest_...; the two pictures
// must be equal (tools/verify/city_picture_compare.py).
bool g_seamCheck = false;

void TakeCity(int orientation)
{
    core::capture::BigPicture *pic = DrawCity(orientation, g_stripRows);
    if (!pic) {
        return;
    }
    core::capture::BigPicture *fewest = g_seamCheck ? DrawCity(orientation, 100000) : nullptr;
    char stem[48], kind[64];
    CityStem(stem, sizeof(stem));
    Save(pic, stem, 1);
    if (fewest) {
        std::snprintf(kind, sizeof(kind), "%s-fewest", stem);
        Save(fewest, kind, 1);
    }
}

// ---------------------------------------------------------------------------------------------
// The minimap (section 10)

using MinimapDraw_fn = void(__cdecl *)(int, int, int, int);

struct MinimapCall {
    BYTE *buf;
    int pitchPx, rows, x0, y0;
    core::diag::Restore r;
};

void DrawMinimap(MinimapCall &c)
{
    c.r.Set(kDrawBuffer.ptr(), c.buf);
    c.r.Set(kScreenPitch.ptr(), c.pitchPx * 2);
    c.r.Set(kDdWidth.ptr(), c.pitchPx);
    c.r.Set(kDdHeight.ptr(), c.rows);
    c.r.Set(kOverlay.ptr(), 0);
    // the view's frame below the minimap, in the part of the buffer that is cut away (section 10.3)
    c.r.Set(kViewDrawnX.ptr(), 0);
    c.r.Set(kViewDrawnY.ptr(), kViewTableRows + 8);
    Function<MinimapDraw_fn>(kMinimapDraw)(c.x0, c.y0, kViewTableCols, kViewTableRows);
}

void TakeMinimap()
{
    MinimapCall c{};
    c.x0 = 8;
    c.y0 = 8;
    c.pitchPx = (c.x0 + 2 * kViewTableCols + 32 + 15) & ~15;
    c.rows = c.y0 + kViewTableRows + 8 + (*kHeightRows > 0 ? *kHeightRows : 0) + 16;
    const size_t n = static_cast<size_t>(c.pitchPx) * c.rows;
    c.buf = static_cast<BYTE *>(std::malloc(n * 2));
    if (!c.buf) {
        Notice("Not enough memory for the minimap picture; nothing was saved.");
        return;
    }
    auto *px = reinterpret_cast<std::uint16_t *>(c.buf);
    for (size_t i = 0; i < n; ++i) {
        px[i] = kKey;
    }
    int state[kMinimapStateCount], clip[4];
    std::memcpy(state, At<int>(kMinimapState), sizeof(state));
    std::memcpy(clip, At<int>(kScreenClip), sizeof(clip));
    const bool ok = core::diag::Guarded(g_feature, [](void *v) { DrawMinimap(*static_cast<MinimapCall *>(v)); }, &c,
                                        &c.r);
    c.r.Undo();
    std::memcpy(At<int>(kMinimapState), state, sizeof(state));
    std::memcpy(At<int>(kScreenClip), clip, sizeof(clip));
    if (!ok) {
        std::free(c.buf);
        core_log("picture: the pictures are off for the rest of the run (an error while drawing the minimap; the "
                 "game's state was put back)");
        Notice("The picture could not be saved (details in the log).");
        return;
    }
    // what was drawn: the rows of the minimap only (its view frame lies below them)
    int x1 = c.pitchPx, y1 = c.rows, x2 = -1, y2 = -1;
    for (int y = c.y0; y < c.y0 + kViewTableRows; ++y) {
        const std::uint16_t *row = px + static_cast<size_t>(y) * c.pitchPx;
        for (int x = 0; x < c.pitchPx; ++x) {
            if (row[x] != kKey) {
                x1 = x < x1 ? x : x1;
                x2 = x > x2 ? x : x2;
                y1 = y < y1 ? y : y1;
                y2 = y > y2 ? y : y2;
            }
        }
    }
    if (x2 < 0) {
        std::free(c.buf);
        core_log("picture: the minimap drew nothing");
        Notice("The picture could not be saved (details in the log).");
        return;
    }
    const int w = x2 - x1 + 1, h = y2 - y1 + 1;
    core::capture::BigPicture *pic = core::capture::BigPicture::Create(w, h);
    if (!pic) {
        std::free(c.buf);
        Notice("Not enough memory for the minimap picture; nothing was saved.");
        return;
    }
    for (int y = 0; y < h; ++y) {
        std::uint16_t *out = pic->Row(y);
        std::memcpy(out, px + static_cast<size_t>(y1 + y) * c.pitchPx + x1, static_cast<size_t>(w) * 2);
        for (int x = 0; x < w; ++x) {
            out[x] = out[x] == kKey ? 0 : out[x]; // the corners outside the map: black
        }
    }
    std::free(c.buf);
    core_log("picture: minimap %d x %d px (at (%d,%d) of the %d x %d drawing), scale %d", w, h, x1, y1, c.pitchPx,
             c.rows, g_minimapScale);
    char stem[48], kind[64];
    CityStem(stem, sizeof(stem));
    std::snprintf(kind, sizeof(kind), "%s_minimap", stem);
    Save(pic, kind, g_minimapScale);
}

// After every city_render: a picture asked for is made now (the frame has been drawn as usual).
void AfterCity(int orientation)
{
    if (g_pending == Kind::None) {
        return;
    }
    const Kind kind = g_pending;
    g_pending = Kind::None;
    if (!core::diag::Enabled(g_feature) || g_busy.load()) {
        return;
    }
    if (kind == Kind::City) {
        TakeCity(orientation);
    }
    else {
        TakeMinimap();
    }
}

// ---------------------------------------------------------------------------------------------
// Settings

bool ApplyKey(core::display::ActionKey *key, unsigned fallbackVk, unsigned fallbackMods, const char *v)
{
    core::ui::Hotkey k{fallbackVk, fallbackMods};
    core::ui::ParseHotkey(v, v + std::strlen(v), &k);
    key->vk = k.vk;
    key->mods = k.mods;
    return true;
}

bool ApplyCityKey(int, const char *v, bool)
{
    return ApplyKey(&g_cityKey, 0x79, 0, v);
}

bool ApplyMinimapKey(int, const char *v, bool)
{
    return ApplyKey(&g_minimapKey, 0x79, core::ui::kModShift, v);
}

bool ApplyScale(int, const char *v, bool)
{
    const int s = std::atoi(v);
    g_minimapScale = s == 1 || s == 4 ? s : 2;
    return true;
}

bool ApplyStripRows(int, const char *v, bool)
{
    g_stripRows = std::atoi(v);
    return true;
}

const char *const kScale[] = {"1", "2", "4"};
const char *const kScaleLabels[] = {"1 (as in the game, the smallest)", "2 (twice as large)",
                                    "4 (four times as large)"};

// Keys the game itself uses (docs/archive/resolution_ui_p1.md section 12.5) and Yongle's own (F5, Alt+Enter,
// Shift+F7): shown in the panel when one of them is chosen as one of our keys.
const char *GameKeyNote(unsigned vk, unsigned mods)
{
    using core::ui::kModAlt;
    using core::ui::kModCtrl;
    using core::ui::kModShift;
    if ((vk == 0x74 && mods == 0) || (vk == VK_RETURN && mods == kModAlt)) {
        return "Yongle also uses this key to switch between window and full screen; that no longer works with it.";
    }
    if (vk == 0x76 && mods == kModShift) {
        return "Yongle also uses this key to change the resolution; that no longer works with it.";
    }
    if (mods == kModAlt && ((vk >= '1' && vk <= '5') || vk == 'T')) {
        return "The game also uses this key; it no longer reaches the game.";
    }
    if (mods == (kModCtrl | kModAlt) && vk == 'C') {
        return "The game also uses this key; it no longer reaches the game.";
    }
    if (vk == 0x71 || vk == 0x74 || vk == 0x75 || vk == 'R') {
        return "The game also reacts to this key itself.";
    }
    if ((mods & (kModCtrl | kModAlt)) == 0) {
        const bool used = (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || (vk >= 0x70 && vk <= 0x78) ||
                          vk == VK_SPACE || vk == VK_TAB || vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME ||
                          vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_NUMPAD2;
        if (used) {
            return "The game also uses this key; it no longer reaches the game.";
        }
    }
    return nullptr;
}

} // namespace

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "ui", "city_picture_key", "City picture: key",
        "Saves the whole city as one picture (PNG) in shots\\ in Yongle's folder, at the game's own size and without "
        "the interface; the game stops for a moment while it is drawn. Change: press the new key, alone or with Ctrl, "
        "Shift or Alt.",
        Type::Hotkey, "F10", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, &ApplyCityKey);
    i = core::settings::Register(Item{
        "ui", "minimap_picture_key", "Minimap picture: key",
        "Saves the minimap of the whole map as a picture (PNG) in shots\\ in Yongle's folder. Change: press the new "
        "key, alone or with Ctrl, Shift or Alt.",
        Type::Hotkey, "Shift+F10", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, &ApplyMinimapKey);
    // Only in test builds (the maps are desert outside the playable range, so there was nothing to choose).
    core::settings::Retire("ui", "city_picture_background", nullptr);
    i = core::settings::Register(Item{"ui", "minimap_picture_scale", "Minimap picture: size",
                                      "How many times larger than the minimap in the game.", Type::Choice, "2",
                                      Effect::Live, true, true, 0, 0, 0, nullptr, kScale, kScaleLabels, 3});
    core::settings::OnChange(i, &ApplyScale);
    i = core::settings::Register(Item{
        "debug", "city_picture_strip_rows", "City picture: half rows per strip (test)",
        "For checking the seams: the city is drawn in strips of this many half rows.", Type::Int, "64", Effect::Live,
        false, true, 4, 1000, 2, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, &ApplyStripRows);
    i = core::settings::Register(Item{
        "debug", "city_picture_seam_check", "City picture: seam check (test)",
        "on: the city picture is also drawn in as few strips as possible at the same moment (city-fewest_...).",
        Type::Bool, "off", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool) {
        g_seamCheck = std::strcmp(v, "on") == 0;
        return true;
    });
    core::settings::Retire("debug", "city_picture_name_probe", nullptr); // test builds only (section 11.6)
}

bool Attach(bool scrollAttached)
{
    if (!scrollAttached) {
        core_log("picture: the city and minimap pictures need the city drawing's hooks (not installed)");
        return false;
    }
    if (!Matches(kMinimapDraw)) {
        core_log("picture: %s at 0x%lx does not match; the city and minimap pictures are not installed",
                 kMinimapDraw.name, kMinimapDraw.address);
        return false;
    }
    g_cityNameOk = Matches(kCityNameGet);
    if (!g_cityNameOk) {
        core_log("picture: %s at 0x%lx does not match; picture file names start with \"city\"", kCityNameGet.name,
                 kCityNameGet.address);
    }
    g_cityKey.fire = &FireCity;
    g_minimapKey.fire = &FireMinimap;
    core::display::AddActionKey(&g_cityKey);
    core::display::AddActionKey(&g_minimapKey);
    core::ui::SetGameKeyNote(&GameKeyNote);
    emperor::scroll::SetAfterCity(&AfterCity);
    g_attached = true;
    char city[40], minimap[40];
    core::ui::FormatHotkey(core::ui::Hotkey{g_cityKey.vk, g_cityKey.mods}, city, sizeof(city));
    core::ui::FormatHotkey(core::ui::Hotkey{g_minimapKey.vk, g_minimapKey.mods}, minimap, sizeof(minimap));
    core_log("picture: city picture (%s) and minimap picture (%s) installed; minimap x%d", city, minimap,
             g_minimapScale);
    return true;
}

} // namespace emperor::city_picture
