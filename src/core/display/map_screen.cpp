// The world map screen at resolutions larger than the map (map_screen.h). Taken from Emperor's china_map.cpp
// when Zeus's world map needed the same; the game's addresses stay in each game layer.
#include "core/display/map_screen.h"

#include "core/diag/perf.h"
#include "core/display/framed_screens.h"
#include "core/display/scaled_rect.h"
#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace core::display::map_screen {

namespace {

constexpr int kBorder = 16;        // the fret border, in map pixels
constexpr double kBlurSigma = 3.0; // the backdrop's blur, in screen pixels (playtest: mockup bg_sigma3_bright55)
constexpr int kBrightness = 141;   // the backdrop's brightness, /256 (55 %)

std::optional<MapGame> g_game;
core::diag::Feature g_unconfigured{"map screen layout (core/display/map_screen)"};
core::diag::Feature *g_feature = &g_unconfigured; // the game layer's (Configure)
int g_phase = -1;

// kOriginal (the game's own drawing, top left) is no longer a choice (hardly anyone would pick
// it; a file that has it gets native); it stays for the offline tests and as the way through when the layout is off.
enum Mode { kScaled, kNative, kOriginal };
int g_mode = kNative;   // the picture's own size by default
int g_scalePercent = 0; // for scaled, 0 = fit (as large as the map area allows)
int g_fitPercent = 0;   // the fit of the map last shown, in percent (0: none shown yet)

// What is around the map: the same map blurred and darkened, or the city as it was when the map opened, as it is (the
// map like a window over it). That city is what the game leaves in its draw buffer: the map screen does not redraw
// outside the map. It is kept when the first map frame after a city frame is drawn and put back on every frame, so
// nothing is left behind. The city is still: the game does not draw it while the map is shown. (Blurring it would take
// about 0.1 s at 2560x1440, a visible stop when the map opens: not done)
enum Background { kMapBackground, kCityBackground };
int g_background = kMapBackground;

// Our buffers.
BYTE *g_layer = nullptr; // the game draws the map here: same layout as the back buffer
size_t g_layerSize = 0;
std::vector<WORD> g_canvas; // border + map, (map + 2 border) pixels square
int g_canvasW = 0, g_canvasH = 0, g_border = 0;
std::vector<WORD> g_backdrop; // the map area's background
std::vector<WORD> g_shrunk;   // border + map shrunk (a map taller than the area)
int g_backdropKey[5] = {};
std::vector<WORD> g_cityBackdrop; // the map area of the draw buffer when the map opened
int g_cityW = 0, g_cityH = 0;
int g_cityFrames = -1;            // framed::CityFramesDrawn() when it was kept

// Where the map is shown now (for the mouse); valid only for the layout it was computed for.
struct ShownMap {
    bool valid;
    int ddW, ddH, sidebarX;
    int x, y;         // the framed map's (border + map) top left on the screen
    double scale;
    int border;       // in map pixels
    int canvasW, canvasH, mapW, mapH;
} g_shown{};

bool g_deferTop = false; // inside our map drawing call: the top bar piece waits
bool g_tallLogged = false; // the map taller than the area: logged once
bool g_topPending = false;
int g_topArg = 0;
long g_logs = 0;
constexpr long kLogs = 12;

const MapGame &Game()
{
    return *g_game;
}

void Log(const char *fmt, int a = 0, int b = 0, int c = 0, int d = 0, int e = 0, int f = 0)
{
    if (++g_logs <= kLogs) {
        char line[256];
        std::snprintf(line, sizeof(line), "%s: %s", Game().logPrefix, fmt);
        core_log(line, a, b, c, d, e, f);
    }
}

// The layout the composition needs: the whole map at its own size inside the area (so the game's scroll offsets
// are 0). mapW / mapH 0: no map loaded yet (the game loads it inside its drawing): checked after the drawing.
struct Layout {
    int ddW, ddH, pitch, sidebarX, mapW, mapH;
};

// The whole map must fit the area's width; a map only taller than the area is drawn whole all the same (DrawHeight)
// and shown smaller (Scale).
bool Fits(const Layout &l)
{
    return l.mapW > 0 && l.mapH > 0 && l.mapW <= l.sidebarX;
}

// The game's height while it draws the map (and reads the mouse on it): the screen's, or the map's when it is taller
// than the area (the game then draws all of it, with no scroll, as on a screen high enough).
int DrawHeight(int ddH, int mapH)
{
    const int whole = Game().topBarH + mapH;
    return mapH > 0 && whole > ddH ? whole : ddH;
}

// The game's map drawing with its height (and its clip's bottom) raised to h for the call, through r.
void DrawAt(core::diag::Restore &r, int h)
{
    const MapGame &g = Game();
    if (h > *g.ddHeight) {
        r.Set(g.ddHeight.ptr(), h);
        if (*g.clipBottom < h) {
            r.Set(g.clipBottom.ptr(), h);
        }
    }
    g.drawMap(r);
}

bool CurrentLayout(Layout *l)
{
    const MapGame &g = Game();
    l->ddW = *g.ddWidth;
    l->ddH = *g.ddHeight;
    l->pitch = *g.pitch;
    l->sidebarX = *g.sidebarX;
    l->mapW = *g.mapW;
    l->mapH = *g.mapH;
    const bool unknown = l->mapW == 0 && l->mapH == 0;
    return g_mode != kOriginal && g.takeOver() && l->ddW > 0 && l->ddH > g.topBarH && l->pitch >= l->ddW * 2 &&
           l->sidebarX > 0 && l->sidebarX <= l->ddW && (unknown || Fits(*l)) && *g.drawBuffer;
}

bool LayerBuffer(size_t size)
{
    if (size <= g_layerSize && g_layer) {
        return true;
    }
    if (g_layer) {
        VirtualFree(g_layer, 0, MEM_RELEASE);
    }
    g_layer = static_cast<BYTE *>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_layerSize = g_layer ? size : 0;
    return g_layer != nullptr;
}

// The map's pixels in our layer: (0, top bar), map size, rows of the back buffer's pitch.
core::display::Pixels16 MapPixels(const Layout &l)
{
    return core::display::Pixels16{
        reinterpret_cast<const WORD *>(g_layer + static_cast<size_t>(Game().topBarH) * l.pitch), l.pitch / 2, l.mapW,
        l.mapH};
}

void Canvas(const Layout &l)
{
    const int b = kBorder;
    if (g_canvasW != l.mapW + 2 * b || g_canvasH != l.mapH + 2 * b) {
        g_canvasW = l.mapW + 2 * b;
        g_canvasH = l.mapH + 2 * b;
        g_border = b;
        if (!core::display::framed::BorderPicture(g_canvasW, g_canvasH, &g_canvas)) {
            Log("no fret border (the panel tiles could not be read): the map without it");
            g_canvasW = l.mapW;
            g_canvasH = l.mapH;
            g_border = 0;
            g_canvas.assign(static_cast<size_t>(g_canvasW) * g_canvasH, 0);
        }
    }
    const core::display::Pixels16 m = MapPixels(l);
    for (int y = 0; y < l.mapH; ++y) {
        std::memcpy(&g_canvas[static_cast<size_t>(y + g_border) * g_canvasW + g_border],
                    m.px + static_cast<size_t>(y) * m.stride, static_cast<size_t>(l.mapW) * 2);
    }
}

void Backdrop(const Layout &l, int areaW, int areaH)
{
    const int key[5] = {*Game().mapNumber, l.mapW, l.mapH, areaW, areaH};
    if (std::memcmp(key, g_backdropKey, sizeof(key)) == 0 && g_backdrop.size() == static_cast<size_t>(areaW) * areaH) {
        return;
    }
    std::memcpy(g_backdropKey, key, sizeof(key));
    core::display::BlurredBackdrop(MapPixels(l), areaW, areaH, kBlurSigma, kBrightness, &g_backdrop);
    Log("backdrop for map %d (%dx%d) over the %dx%d map area", key[0], l.mapW, l.mapH, areaW, areaH);
}

double Scale(int areaW, int areaH, int mapW, int mapH)
{
    // A map taller (or wider) than the area: as large as the map itself fits, in either mode (its border cut).
    const double mapFit = std::fmin(static_cast<double>(areaW) / mapW, static_cast<double>(areaH) / mapH);
    if (mapFit < 1.0) {
        g_fitPercent = static_cast<int>(std::floor(mapFit * 100 + 1e-9));
        return mapFit;
    }
    if (g_mode == kNative) {
        g_fitPercent = static_cast<int>(std::floor(
            std::fmin(static_cast<double>(areaW) / g_canvasW, static_cast<double>(areaH) / g_canvasH) * 100 + 1e-9));
        return 1.0;
    }
    double s = std::fmin(static_cast<double>(areaW) / g_canvasW, static_cast<double>(areaH) / g_canvasH);
    g_fitPercent = static_cast<int>(std::floor(s * 100 + 1e-9));
    if (g_scalePercent > 0) { // a chosen scale, never larger than fits
        s = std::fmin(g_scalePercent / 100.0, s);
    }
    return s < 1.0 ? 1.0 : s;
}

// Backdrop and the framed map into `target` (the buffer the game draws to outside our call).
void Compose(const Layout &l, BYTE *target, bool backdrop)
{
    const int top = Game().topBarH;
    const int areaW = l.sidebarX, areaH = l.ddH - top;
    Canvas(l);
    const double s = Scale(areaW, areaH, l.mapW, l.mapH);
    const int tw = static_cast<int>(std::lround(g_canvasW * s)), th = static_cast<int>(std::lround(g_canvasH * s));
    const int tx = (areaW - tw) / 2, ty = top + (areaH - th) / 2;
    const core::display::Target16 dst{reinterpret_cast<WORD *>(target), l.pitch / 2, l.ddW, l.ddH};
    const RECT area{0, top, areaW, l.ddH};
    if (backdrop) {
        const std::vector<WORD> *bg = &g_backdrop;
        const size_t areaSize = static_cast<size_t>(areaW) * areaH;
        if (g_background == kCityBackground && Game().cityBackground() && g_cityW == areaW && g_cityH == areaH &&
            g_cityBackdrop.size() == areaSize) {
            bg = &g_cityBackdrop;
        }
        if (bg == &g_backdrop) { // also when no city was kept for this size (e.g. the resolution changed on the map)
            Backdrop(l, areaW, areaH);
        }
        for (int y = 0; y < areaH; ++y) {
            std::memcpy(dst.px + static_cast<size_t>(y + top) * dst.stride, &(*bg)[static_cast<size_t>(y) * areaW],
                        static_cast<size_t>(areaW) * 2);
        }
    }
    const core::display::Pixels16 canvas{g_canvas.data(), g_canvasW, g_canvasW, g_canvasH};
    if (s < 1.0) {
        // Smaller than its own size (a map taller than the area): each pixel the average of the canvas area it
        // covers (ScaleRgb565 only enlarges), then put in place inside the area.
        g_shrunk.assign(static_cast<size_t>(tw) * th, 0);
        core::display::ShrinkRgb565(canvas, tw, th, true, core::display::Target16{g_shrunk.data(), tw, tw, th});
        const int left = std::max(static_cast<int>(area.left), tx), right = std::min(static_cast<int>(area.right), tx + tw);
        const int top0 = std::max(static_cast<int>(area.top), ty), bottom = std::min(static_cast<int>(area.bottom), ty + th);
        for (int y = top0; y < bottom && left < right; ++y) {
            std::memcpy(dst.px + static_cast<size_t>(y) * dst.stride + left,
                        &g_shrunk[static_cast<size_t>(y - ty) * tw + (left - tx)], static_cast<size_t>(right - left) * 2);
        }
    }
    else {
        const bool whole = std::fabs(s - std::round(s)) < 1e-9;
        core::display::ScaleRgb565(canvas, s, !whole, dst, tx, ty, area);
    }
    g_shown = ShownMap{true, l.ddW, l.ddH, l.sidebarX, tx, ty, s, g_border, g_canvasW, g_canvasH, l.mapW, l.mapH};
}

// The real mouse in the game's own map coordinates while the map is shown by us. Outside the map but inside the
// map area: the game layer's empty point (nothing is there: a click deselects, as on the empty map). Elsewhere
// (sidebar, top bar): false, the real position.
bool MapMouse(int mx, int my, int *gx, int *gy)
{
    const MapGame &g = Game();
    const ShownMap &s = g_shown;
    if (!s.valid || s.ddW != *g.ddWidth || s.ddH != *g.ddHeight || s.sidebarX != *g.sidebarX) {
        return false;
    }
    int u, v; // the pixel of border + map drawn there (the same rounding as the drawing)
    if (core::display::ScaledToSource(mx, my, s.x, s.y, s.scale, s.canvasW, s.canvasH, &u, &v) && u >= s.border &&
        v >= s.border && u < s.border + s.mapW && v < s.border + s.mapH) {
        *gx = u - s.border; // the game's origin is (0, top bar)
        *gy = v - s.border + g.topBarH;
        return true;
    }
    if (mx >= 0 && mx < s.sidebarX && my >= g.topBarH && my < s.ddH) {
        g.emptyPoint(s.mapW, s.mapH, s.sidebarX, gx, gy);
        return true;
    }
    return false;
}

struct Call {
    Layout l;
    BYTE *target;
    core::diag::Restore r;
};

// The game's drawing as it drew it, from our layer to the screen (its own map box).
void AsDrawn(const Call &c)
{
    const RECT box = Game().gameBox(c.l.ddW, c.l.ddH, c.l.sidebarX, *Game().mapW, *Game().mapH);
    const int right = box.right < c.l.ddW ? static_cast<int>(box.right) : c.l.ddW;
    const int bottom = box.bottom < c.l.ddH ? static_cast<int>(box.bottom) : c.l.ddH;
    const int left = box.left > 0 ? static_cast<int>(box.left) : 0;
    for (int y = box.top > 0 ? static_cast<int>(box.top) : 0; y < bottom && left < right; ++y) {
        std::memcpy(c.target + static_cast<size_t>(y) * c.l.pitch + left * 2,
                    g_layer + static_cast<size_t>(y) * c.l.pitch + left * 2, static_cast<size_t>(right - left) * 2);
    }
}

void DrawLayer(void *p)
{
    const MapGame &g = Game();
    Call &c = *static_cast<Call *>(p);
    const int top = g.topBarH;
    int drawH = DrawHeight(c.l.ddH, c.l.mapH);
    if (!LayerBuffer(static_cast<size_t>(c.l.pitch) * drawH)) {
        g.drawMap(c.r);
        c.r.Undo();
        return;
    }
    const int cityFrames = core::display::framed::CityFramesDrawn();
    if (cityFrames != g_cityFrames && g.cityBackground()) {
        // The first map frame after the city: its map area still shows the city.
        g_cityFrames = cityFrames;
        const int areaW = c.l.sidebarX, areaH = c.l.ddH - top;
        g_cityBackdrop.resize(static_cast<size_t>(areaW) * areaH);
        for (int y = 0; y < areaH; ++y) {
            std::memcpy(&g_cityBackdrop[static_cast<size_t>(y) * areaW],
                        c.target + static_cast<size_t>(y + top) * c.l.pitch, static_cast<size_t>(areaW) * 2);
        }
        g_cityW = areaW;
        g_cityH = areaH;
        Log("the city kept from the draw buffer for the %dx%d map area", areaW, areaH);
    }
    c.r.Set(g.drawBuffer.ptr(), g_layer);
    c.r.Set(&g_deferTop, true);
    g_topPending = false;
    DrawAt(c.r, drawH);
    c.r.Undo();
    if (c.l.mapW == 0 && c.l.mapH == 0) { // the first map: loaded by the drawing
        c.l.mapW = *g.mapW;
        c.l.mapH = *g.mapH;
        if (!Fits(c.l)) {
            Log("map %dx%d does not fit the %dx%d map area: drawn the game's way", c.l.mapW, c.l.mapH, c.l.sidebarX,
                c.l.ddH - top);
            AsDrawn(c);
            g_shown.valid = false;
            c.l.mapW = -1; // composed: no
        }
        else if (DrawHeight(c.l.ddH, c.l.mapH) > drawH) {
            // Taller than the area: drawn again, whole (the first drawing stopped at the screen's bottom).
            drawH = DrawHeight(c.l.ddH, c.l.mapH);
            if (!LayerBuffer(static_cast<size_t>(c.l.pitch) * drawH)) {
                c.r.Set(g.drawBuffer.ptr(), c.target); // our layer is gone: the game's own drawing on the screen
                g.drawMap(c.r);
                c.r.Undo();
                g_shown.valid = false;
                return;
            }
            c.r.Set(g.drawBuffer.ptr(), g_layer);
            c.r.Set(&g_deferTop, true);
            DrawAt(c.r, drawH);
            c.r.Undo();
        }
    }
    if (c.l.mapW > 0 && drawH > c.l.ddH && !g_tallLogged) {
        g_tallLogged = true;
        Log("map %dx%d taller than the %dx%d map area: drawn whole (%d rows) and shown smaller", c.l.mapW, c.l.mapH,
            c.l.sidebarX, c.l.ddH - top, drawH);
    }
    if (c.l.mapW < 0) {
        // shown as drawn above
    }
    else if (*g.originX != 0 || *g.originY != top || *g.scrollX > 0 || *g.scrollX < -g.scrollSlack ||
             *g.scrollY > 0 || *g.scrollY < -g.scrollSlack || *g.mapW != c.l.mapW || *g.mapH != c.l.mapH) {
        // Not the layout we expected (a new picture, or the game scrolls): show what it drew, as it drew it.
        Log("unexpected map state (origin %d,%d scroll %d,%d map %dx%d): drawn the game's way", *g.originX,
            *g.originY, *g.scrollX, *g.scrollY, *g.mapW, *g.mapH);
        AsDrawn(c);
        g_shown.valid = false;
    }
    else {
        core::diag::perf::Scope timed(g_phase);
        Compose(c.l, c.target, true);
    }
    if (g_topPending) {
        g_topPending = false;
        g.topBar(g_topArg);
    }
}

// The game's own drawing, with what the game layer sets around it undone afterwards.
void GameWay()
{
    core::diag::Restore r;
    Game().drawMap(r);
    r.Undo();
}

// Screen 0x34: the markers go into our layer (onto the map the background drew there), the sidebar to the screen;
// then the map is shown again.
void CampaignLayer(Call &c, void (*original)())
{
    const MapGame &g = Game();
    core::diag::perf::Scope timed(g_phase);
    const int pitch = c.l.pitch, x0 = c.l.sidebarX, bytes = (c.l.ddW - x0) * 2;
    for (int y = 0; y < c.l.ddH; ++y) { // the sidebar's strip as it is on the screen
        std::memcpy(g_layer + static_cast<size_t>(y) * pitch + x0 * 2,
                    c.target + static_cast<size_t>(y) * pitch + x0 * 2, bytes);
    }
    int gx = 0, gy = 0;
    c.r.Set(g.drawBuffer.ptr(), g_layer);
    if (MapMouse(*g.mouseX, *g.mouseY, &gx, &gy)) {
        c.r.Set(g.mouseX.ptr(), gx);
        c.r.Set(g.mouseY.ptr(), gy);
    }
    original();
    c.r.Undo();
    for (int y = 0; y < c.l.ddH; ++y) {
        std::memcpy(c.target + static_cast<size_t>(y) * pitch + x0 * 2,
                    g_layer + static_cast<size_t>(y) * pitch + x0 * 2, bytes);
    }
    Compose(c.l, c.target, false);
}

} // namespace

const char *const kModes[3] = {"scaled", "native", "original"};
const char *const kModeLabels[2] = {"scaled (larger than its own size)", "native (its own size, centered)"};
const char *const kBackgrounds[2] = {"map", "city"};
const char *const kBackgroundLabels[2] = {"map (the same map, blurred and darkened)",
                                          "city (your city as you left it)"};

void Configure(const MapGame &game)
{
    g_game.emplace(game);
    g_feature = game.feature;
    if (g_phase < 0) {
        g_phase = core::diag::perf::Register(game.phaseName, core::diag::perf::Thread::Game);
    }
}

void Draw()
{
    Call c{};
    if (!core::diag::Enabled(*g_feature) || !CurrentLayout(&c.l)) {
        g_shown.valid = false;
        GameWay();
        return;
    }
    c.target = *Game().drawBuffer;
    if (!core::diag::Guarded(*g_feature, &DrawLayer, &c, &c.r)) {
        g_shown.valid = false;
        g_topPending = false;
        GameWay(); // the game's own way from now on
    }
}

bool TopBar(int arg)
{
    if (g_deferTop) {
        g_topPending = true;
        g_topArg = arg;
        return true;
    }
    return false;
}

void WithMapMouse(void (*fn)(void *), void *ctx)
{
    const MapGame &g = Game();
    int gx = 0, gy = 0;
    if (!core::diag::Enabled(*g_feature) || !MapMouse(*g.mouseX, *g.mouseY, &gx, &gy)) {
        fn(ctx);
        return;
    }
    struct Ctx {
        void (*fn)(void *);
        void *ctx;
        int gx, gy;
        core::diag::Restore r;
    } c{fn, ctx, gx, gy, {}};
    if (!core::diag::Guarded(
            *g_feature,
            [](void *p) {
                Ctx &c = *static_cast<Ctx *>(p);
                c.r.Set(Game().mouseX.ptr(), c.gx);
                c.r.Set(Game().mouseY.ptr(), c.gy);
                // A map taller than the area: the game's height as when it was drawn (its input checks the mouse
                // against the height: the map's lower rows would not answer otherwise).
                const int h = DrawHeight(*Game().ddHeight, g_shown.mapH);
                if (h > *Game().ddHeight) {
                    c.r.Set(Game().ddHeight.ptr(), h);
                }
                c.fn(c.ctx);
                c.r.Undo();
            },
            &c, &c.r)) {
        g_shown.valid = false;
        fn(ctx);
    }
}

void CampaignDraw(void (*original)())
{
    const MapGame &g = Game();
    Call c{};
    const size_t size = static_cast<size_t>(*g.pitch) * *g.ddHeight;
    if (!core::diag::Enabled(*g_feature) || !CurrentLayout(&c.l) || c.l.mapW <= 0 || !g_shown.valid ||
        g_shown.ddW != c.l.ddW || g_shown.ddH != c.l.ddH || !g_layer || g_layerSize < size) {
        original();
        return;
    }
    c.target = *g.drawBuffer;
    struct CampaignCall {
        Call *c;
        void (*original)();
    } cc{&c, original};
    if (!core::diag::Guarded(
            *g_feature,
            [](void *p) {
                auto &cc = *static_cast<CampaignCall *>(p);
                CampaignLayer(*cc.c, cc.original);
            },
            &cc, &c.r)) {
        g_shown.valid = false;
        *g.redraw = 1; // the background draws the map the game's way again
        original();
    }
}

bool Shown()
{
    return g_shown.valid;
}

bool ApplyMode(int, const char *v, bool initial)
{
    g_mode = kNative;
    for (int i = 0; i < kModeChoices; ++i) {
        if (std::strcmp(v, kModes[i]) == 0) {
            g_mode = i;
        }
    }
    if (!initial && g_game) {
        *Game().redraw = 1; // redrawn in the new way (the campaign map's picture is drawn only on redraws)
    }
    return true;
}

bool ApplyScale(int, const char *v, bool initial)
{
    g_scalePercent = std::strcmp(v, "fit") == 0 ? 0 : std::atoi(v);
    if (!initial && g_game) {
        *Game().redraw = 1;
    }
    return true;
}

bool ApplyBackground(int, const char *v, bool initial)
{
    g_background = std::strcmp(v, "city") == 0 ? kCityBackground : kMapBackground;
    if (!initial && g_game) {
        *Game().redraw = 1;
    }
    return true;
}

int FitPercent()
{
    return g_fitPercent > 0 ? g_fitPercent : 400;
}

const char *ModeName()
{
    return kModes[g_mode];
}

const char *BackgroundName()
{
    return kBackgrounds[g_background];
}

int ScalePercent()
{
    return g_scalePercent;
}

void SetModeForTest(int mode, int scalePercent)
{
    g_mode = mode;
    g_scalePercent = scalePercent;
}

void SetBackgroundForTest(bool city)
{
    g_background = city ? kCityBackground : kMapBackground;
}

} // namespace core::display::map_screen
