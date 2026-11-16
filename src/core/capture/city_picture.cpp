// See city_picture.h. Moved from Yongle's games/emperor/city_picture.cpp (0.2.0) so Zeus has the same city picture
// too; the game's addresses stay in each game layer.
//
// Changed from Yongle's: the guard covers one strip (or one minimap
// drawing) at a time, inside the game layer's aroundDrawing (Zeus's drawing that never counts is outside our guard, so
// a fault in a strip still runs its put-back); a picture is drawn only when the game layer says it can be now (Zeus:
// in a counting frame, so every strip starts from the same state); the minimap is drawn twice on two key colors and a
// pixel is "not drawn" only when it kept both (the game may draw any color itself).
#include "core/capture/city_picture.h"

#include "core/capture/big_picture.h"
#include "core/capture/screenshot.h"
#include "core/diag/guard.h"
#include "core/display/city_strips.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/scroll/picture.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"
#include "core/ui/notice.h"
#include "core/ui/panel.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core::capture::city_picture {

namespace {

namespace notice = core::ui::notice;

// The minimap's drawing area before each of its two drawings: two colors that differ in every bit.
constexpr std::uint16_t kKeys[2] = {0xF81F, 0x07E0};
constexpr int kOverlapRows = 4; // half rows drawn again by the next strip
// No room above the view's top row: the picture starts where the topmost view
// does, as the player sees it.
constexpr int kTopRows = 0;
constexpr int kNameBytes = 64; // a city name is read up to this many bytes

core::diag::Feature g_feature{"city picture (core/capture/city_picture)"};
const CityPictureGame *g_game = nullptr;
std::atomic<bool> g_busy{false}; // a picture is being written

enum class Kind { None, City, Minimap };
Kind g_pending = Kind::None;

core::display::ActionKey g_cityKey{0x79 /* F10 */, 0, nullptr};
core::display::ActionKey g_minimapKey{0x79, core::ui::kModShift, nullptr};
int g_minimapScale = 2;
int g_stripRows = 64;
int g_minimapDifferences = 0; // the last minimap's pixels drawn differently by its two drawings

// In the game's own notice boxes (on our caption line when those are not available).
void Notice(const char *text)
{
    notice::Post(text);
}

// An error or something to do first: in red, as the game's own warnings.
void Warn(const char *text)
{
    notice::Post(text, true);
}

std::atomic<int> g_saving{0}; // the kind being written (Kind), for the message when it is done

// The file names' first part: the player's city as the game names it, letters and digits kept, '-' for anything else
// between them; "city" when there is no city or no usable name.
void CityStem(char *out, std::size_t size)
{
    std::snprintf(out, size, "city");
    const unsigned char *name = g_game ? g_game->cityName() : nullptr;
    if (!name) {
        return; // logged by the game layer
    }
    char stem[33];
    std::size_t n = 0;
    bool gap = false;
    for (int i = 0; i < kNameBytes && name[i] != 0 && n + 2 < sizeof(stem); ++i) {
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

void Done(bool ok, const char *text)
{
    core_log("picture: %s %s", ok ? "saved:" : "not saved:", text ? text : "");
    if (!ok) {
        Warn(notice::kPictureNotSaved);
    }
    else {
        Notice(g_saving.load() == static_cast<int>(Kind::Minimap) ? notice::kMinimapPictureSaved
                                                                   : notice::kCityPictureSaved);
    }
    g_busy = false;
}

void Request(Kind kind)
{
    const char *what = kind == Kind::City ? "city picture" : "minimap picture";
    if (!g_game || !core::diag::Enabled(g_feature) || !g_game->drawingWorks()) {
        core_log("picture: %s key pressed, but the pictures are off (see above)", what);
        Warn(notice::kPictureUnavailable);
        return;
    }
    if (core::ui::IsOpen()) { // the game is paused: the city is not drawn until the panel closes
        Warn(notice::kPictureClosePanel);
        return;
    }
    if (!g_game->cityScreen()) {
        core_log("picture: %s key pressed outside the city (screen 0x%02x)", what,
                 static_cast<unsigned>(g_game->gameScreen()) & 0xff);
        Warn(notice::kPictureOnlyCity);
        return;
    }
    if (g_busy.load()) {
        core_log("picture: %s key pressed while the last picture is still being written", what);
        Warn(notice::kPictureBusy);
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
// The city in strips

struct CityCall {
    core::display::StripInput in;
    core::display::StripPlan plan;
    int orientation;
    BYTE *buf;
    int pitchPx;
    core::capture::BigPicture *pic;
    core::diag::Restore r;  // the game's variables (put back by the guard when abandoned)
    core::diag::Restore r2; // the counters and our hooks' state (put back by the caller in every case)
};

struct StripCall {
    CityCall *c;
    int k;
    bool ok;
};

// One strip: our buffer black, the game's drawing, its kept rows copied into the picture.
void DrawOneStrip(void *v)
{
    const StripCall &s = *static_cast<StripCall *>(v);
    CityCall &c = *s.c;
    const core::display::StripPlan &p = c.plan;
    const core::display::Strip strip = core::display::StripAt(c.in, p, s.k);
    auto *px = reinterpret_cast<std::uint16_t *>(c.buf);
    std::memset(c.buf, 0, static_cast<std::size_t>(c.pitchPx) * p.bufH * 2); // black where the game draws nothing
    g_game->drawStrip(c.in.left, strip.viewY, c.orientation);
    for (int y = 0; y < strip.rows; ++y) {
        std::memcpy(c.pic->Row(strip.toY + y), px + static_cast<std::size_t>(strip.fromY + y) * c.pitchPx,
                    static_cast<std::size_t>(p.imageW) * 2);
    }
}

// Inside the game's drawing state (aroundDrawing): our guard around the one strip, so the game layer's own put-back
// still runs after a fault (the guard puts r back first).
void GuardedStrip(void *v)
{
    auto *s = static_cast<StripCall *>(v);
    s->ok = core::diag::Guarded(g_feature, &DrawOneStrip, s, &s->c->r);
}

// The city drawn with keepRows half rows per strip; nullptr (logged, the player told) when it could not be.
core::capture::BigPicture *DrawCity(int orientation, int keepRows)
{
    CityCall c{};
    c.orientation = orientation;
    int left, right, top, bottom, mapRows, tileWidth, halfHeight;
    g_game->viewLimits(&left, &right, &top, &bottom, &mapRows);
    g_game->tileSize(&tileWidth, &halfHeight);
    c.in = core::display::StripInput{left,       right,    top,          bottom,  mapRows, tileWidth + 2,
                                     halfHeight, keepRows, kOverlapRows, kTopRows};
    if (!core::display::PlanStrips(c.in, &c.plan)) {
        core_log("picture: no city to draw (columns %d..%d, half rows %d..%d of %d, tiles %d x %d)", left, right, top,
                 bottom, mapRows, tileWidth, halfHeight);
        Warn(notice::kPictureNotSaved);
        return nullptr;
    }
    const core::display::StripPlan &p = c.plan;
    c.pitchPx = (p.bufW + 15) & ~15;
    const std::size_t bytes = static_cast<std::size_t>(c.pitchPx) * 2 * p.bufH;
    core_log("picture: city %d x %d px (%.1f MB): map columns %d..%d, half rows %d..%d (map %d), tiles %d x %d; %d "
             "strip(s) of %d half rows kept (view %d x %d, +%d drawn again), buffer %d x %d px (%.1f MB)",
             p.imageW, p.imageH, static_cast<double>(p.imageW) * p.imageH * 2 / (1024.0 * 1024.0), left, right, top,
             bottom, mapRows, tileWidth, halfHeight, p.count, p.keepRows, p.cols, p.viewRows, p.viewRows - p.keepRows,
             c.pitchPx, p.bufH, bytes / (1024.0 * 1024.0));
    c.pic = core::capture::BigPicture::Create(p.imageW, p.imageH);
    c.buf = c.pic ? static_cast<BYTE *>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
                  : nullptr;
    if (!c.buf) {
        if (c.pic) {
            core_log("picture: no memory for the drawing buffer (%.1f MB)", bytes / (1024.0 * 1024.0));
        }
        delete c.pic;
        Warn(notice::kPictureNoMemory);
        return nullptr;
    }
    g_game->hold(Drawing::City);
    g_game->beginCityDrawing(c.r, c.r2, p, c.buf, c.pitchPx);
    core::scroll::picture::PictureDrawing(c.r2, p.clipRight, p.clipBottom);
    const DWORD start = GetTickCount();
    bool ok = true, putBack = true;
    for (int k = 0; k < p.count && ok && putBack; ++k) {
        StripCall s{&c, k, false};
        putBack = g_game->aroundDrawing(&GuardedStrip, &s);
        ok = !putBack || s.ok;
    }
    const DWORD drawMs = GetTickCount() - start;
    g_game->drawn(Drawing::City);
    c.r.Undo(); // the drawing's changes were for its duration only (already undone when it was abandoned)
    c.r2.Undo();
    g_game->release(Drawing::City);
    VirtualFree(c.buf, 0, MEM_RELEASE);
    if (!putBack) {
        delete c.pic;
        core_log("picture: the city picture was not drawn (the game's drawing state cannot be put back now)");
        Warn(notice::kPictureNotSaved);
        return nullptr;
    }
    if (!ok) {
        delete c.pic;
        core_log("picture: the city picture is off for the rest of the run (an error while drawing; the game's "
                 "state was put back)");
        Warn(notice::kPictureNotSaved);
        return nullptr;
    }
    core_log("picture: the city drawn in %lu ms", static_cast<unsigned long>(drawMs));
    return c.pic;
}

void Save(core::capture::BigPicture *pic, const char *kind, int scale, Kind which)
{
    core::capture::BigOptions o;
    o.kind = kind;
    o.scale = scale;
    o.transparent = false;
    o.done = &Done;
    g_busy = true;
    g_saving = static_cast<int>(which);
    core::capture::SaveBig(pic, o);
}

// [debug] city_picture_seam_check: the same moment drawn a second time in as few strips as the map allows (nothing
// moves between the two: the animations are held still in both), saved as city-fewest_...; the two pictures must be
// equal (checked by comparing the two files).
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
    Save(pic, stem, 1, Kind::City);
    if (fewest) {
        std::snprintf(kind, sizeof(kind), "%s-fewest", stem);
        Save(fewest, kind, 1, Kind::City);
    }
}

// ---------------------------------------------------------------------------------------------
// The minimap

struct MinimapCall {
    BYTE *buf;
    int pitchPx, rows, x0, y0;
    RECT area; // where the map can be (the game layer may narrow it)
    core::diag::Restore r;
    bool ok;
};

void DrawMinimapOnce(void *v)
{
    auto *c = static_cast<MinimapCall *>(v);
    g_game->drawMinimap(c->r, c->buf, c->pitchPx, c->rows, c->x0, c->y0, &c->area);
}

void GuardedMinimap(void *v)
{
    auto *c = static_cast<MinimapCall *>(v);
    c->ok = core::diag::Guarded(g_feature, &DrawMinimapOnce, c, &c->r);
}

void TakeMinimap()
{
    MinimapCall c{};
    c.x0 = 8;
    c.y0 = 8;
    c.pitchPx = (c.x0 + 2 * g_game->viewTableCols + 32 + 15) & ~15;
    const int viewRows = g_game->viewRows();
    c.rows = c.y0 + g_game->viewTableRows + 8 + (viewRows > 0 ? viewRows : 0) + 16;
    const std::size_t n = static_cast<std::size_t>(c.pitchPx) * c.rows;
    auto *drawings = static_cast<std::uint16_t *>(std::malloc(n * 2 * 2)); // the two drawings, one after the other
    if (!drawings) {
        Warn(notice::kPictureNoMemory);
        return;
    }
    bool ok = true, putBack = true;
    g_game->hold(Drawing::Minimap);
    for (int d = 0; d < 2 && ok && putBack; ++d) {
        std::uint16_t *px = drawings + n * d;
        for (std::size_t i = 0; i < n; ++i) {
            px[i] = kKeys[d];
        }
        c.buf = reinterpret_cast<BYTE *>(px);
        c.area = RECT{0, c.y0, c.pitchPx, c.y0 + g_game->viewTableRows};
        c.ok = false;
        putBack = g_game->aroundDrawing(&GuardedMinimap, &c);
        ok = !putBack || c.ok;
        if (d == 1 || !ok || !putBack) {
            g_game->drawn(Drawing::Minimap);
        }
        c.r.Undo();
    }
    g_game->release(Drawing::Minimap);
    if (!putBack || !ok) {
        std::free(drawings);
        core_log(!putBack ? "picture: the minimap picture was not drawn (the game's drawing state cannot be put back "
                            "now)"
                          : "picture: the pictures are off for the rest of the run (an error while drawing the "
                            "minimap; the game's state was put back)");
        Warn(notice::kPictureNotSaved);
        return;
    }
    // What was drawn: a pixel either drawing changed from its key, in the map's part only (its view frame lies below
    // it; a filler around the map is outside it).
    const std::uint16_t *a = drawings, *b = drawings + n;
    const RECT area{c.area.left < 0 ? 0 : c.area.left, c.area.top < 0 ? 0 : c.area.top,
                    c.area.right > c.pitchPx ? c.pitchPx : c.area.right, c.area.bottom > c.rows ? c.rows : c.area.bottom};
    int x1 = c.pitchPx, y1 = c.rows, x2 = -1, y2 = -1, differences = 0;
    for (int y = area.top; y < area.bottom; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * c.pitchPx;
        for (int x = area.left; x < area.right; ++x) {
            const std::uint16_t pa = a[row + x], pb = b[row + x];
            if (pa == kKeys[0] && pb == kKeys[1]) {
                continue;
            }
            differences += pa != pb ? 1 : 0;
            x1 = x < x1 ? x : x1;
            x2 = x > x2 ? x : x2;
            y1 = y < y1 ? y : y1;
            y2 = y > y2 ? y : y2;
        }
    }
    g_minimapDifferences = differences;
    if (differences) {
        core_log("picture: the minimap's two drawings differ in %d pixel(s): the first one is saved", differences);
    }
    if (x2 < 0) {
        std::free(drawings);
        core_log("picture: the minimap drew nothing");
        Warn(notice::kPictureNotSaved);
        return;
    }
    const int w = x2 - x1 + 1, h = y2 - y1 + 1;
    core::capture::BigPicture *pic = core::capture::BigPicture::Create(w, h);
    if (!pic) {
        std::free(drawings);
        Warn(notice::kPictureNoMemory);
        return;
    }
    for (int y = 0; y < h; ++y) {
        std::uint16_t *out = pic->Row(y);
        const std::size_t row = static_cast<std::size_t>(y1 + y) * c.pitchPx + x1;
        for (int x = 0; x < w; ++x) {
            const bool drawn = !(a[row + x] == kKeys[0] && b[row + x] == kKeys[1]);
            out[x] = drawn ? a[row + x] : 0; // the corners outside the map: black
        }
    }
    std::free(drawings);
    core_log("picture: minimap %d x %d px (at (%d,%d) of the %d x %d drawing), scale %d", w, h, x1, y1, c.pitchPx,
             c.rows, g_minimapScale);
    char stem[48], kind[64];
    CityStem(stem, sizeof(stem));
    std::snprintf(kind, sizeof(kind), "%s_minimap", stem);
    Save(pic, kind, g_minimapScale, Kind::Minimap);
}

// After every city_render: a picture asked for is made now (the frame has been drawn as usual), once the game layer
// says a drawing can be made in this one.
void AfterCity(int orientation)
{
    if (g_pending == Kind::None || !g_game) {
        return;
    }
    if (!core::diag::Enabled(g_feature) || g_busy.load()) {
        g_pending = Kind::None;
        return;
    }
    if (!g_game->drawingWorks()) { // switched off after the key was pressed (a fault in the city drawing's hooks)
        g_pending = Kind::None;
        core_log("picture: the picture asked for is not drawn: the city drawing's hooks were switched off since");
        Warn(notice::kPictureNotSaved);
        return;
    }
    if (!g_game->drawingNow()) {
        return; // asked again at the next city_render
    }
    const Kind kind = g_pending;
    g_pending = Kind::None;
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
    core::ui::Hotkey k[core::ui::kMaxKeys] = {{fallbackVk, fallbackMods}, {0, 0}};
    core::ui::ParseHotkeyList(v, v + std::strlen(v), k); // one key, or two (the second the alternative)
    key->vk = k[0].vk;
    key->mods = k[0].mods;
    key->vk2 = k[1].vk;
    key->mods2 = k[1].mods;
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

} // namespace

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "ui", "city_picture_key", "Full city screenshot: key",
        "Saves the whole city as one picture (PNG) in shots\\ in {product}'s folder, at the game's own size and "
        "without the interface; the game stops for a moment while it is drawn. Change: press the new key, alone or "
        "with Ctrl, Shift or Alt.",
        Type::Hotkey, "F10", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, &ApplyCityKey);
    i = core::settings::Register(Item{
        "ui", "minimap_picture_key", "Minimap screenshot: key",
        "Saves the minimap of the whole map as a picture (PNG) in shots\\ in {product}'s folder. Change: press the "
        "new key, alone or with Ctrl, Shift or Alt.",
        Type::Hotkey, "Shift+F10", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, &ApplyMinimapKey);
    i = core::settings::Register(Item{"ui", "minimap_picture_scale", "Minimap screenshot: size",
                                      "How large the minimap screenshot saved with its key is, in times the minimap "
                                      "in the game (not the minimap beside the city).",
                                      Type::Choice, "2",
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
}

void Install(const CityPictureGame &g)
{
    g_game = &g;
    g_cityKey.fire = &FireCity;
    g_minimapKey.fire = &FireMinimap;
    core::display::AddActionKey(&g_cityKey);
    core::display::AddActionKey(&g_minimapKey);
    core::scroll::picture::SetAfterCity(&AfterCity);
    char city[40], minimap[40];
    core::ui::FormatHotkey(core::ui::Hotkey{g_cityKey.vk, g_cityKey.mods}, city, sizeof(city));
    core::ui::FormatHotkey(core::ui::Hotkey{g_minimapKey.vk, g_minimapKey.mods}, minimap, sizeof(minimap));
    core_log("picture: city picture (%s) and minimap picture (%s) installed; minimap x%d", city, minimap,
             g_minimapScale);
}

bool Available()
{
    return g_game != nullptr;
}

void RequestCity()
{
    FireCity();
}

void RequestMinimap()
{
    FireMinimap();
}

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

void AfterCityForTest(int orientation)
{
    AfterCity(orientation);
}

void ResetForTest()
{
    g_game = nullptr;
    g_pending = Kind::None;
    g_busy = false;
    g_feature.off = false;
    g_minimapDifferences = 0;
}

void StemForTest(char *out, std::size_t size)
{
    CityStem(out, size);
}

int MinimapDifferencesForTest()
{
    return g_minimapDifferences;
}

} // namespace core::capture::city_picture
