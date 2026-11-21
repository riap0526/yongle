// See city_picture.h. Every address was checked in the disassembly. The pictures themselves (keys, requests, the
// strips, the minimap's crop, file names, settings) are the core's (core/capture/city_picture, shared with Pericles);
// this file gives it Emperor's variables, the drawing and the player's city.
#include "games/emperor/city_picture.h"

#include "core/capture/city_picture.h"
#include "core/diag/guard.h"
#include "core/display/city_strips.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "games/emperor/scroll.h"
#include "games/emperor/site.h"
#include "games/emperor/zoom.h"

#include <cstdio>
#include <cstring>

namespace emperor::city_picture {

namespace {

namespace cp = core::capture::city_picture;

// minimap_draw(x, y, w, h): __cdecl, the caller pops (checked by disassembly)
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

// The player's city for the file names [confirmed: 0x5004be-0x5004cd, city_get 0x481f10
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

bool g_attached = false;

template <typename T> T *At(DWORD address)
{
    return reinterpret_cast<T *>(address + g_imageShift);
}

// The player's city's name as the game gives it; nullptr (logged) when there is none.
const unsigned char *CityName()
{
    const int index = *At<int>(kPlayerCity);
    if (index < 0 || index >= kCityCount) {
        core_log("picture: the player's city index is %d (not 0..%d): file names start with \"city\"", index,
                 kCityCount - 1);
        return nullptr;
    }
    const auto *city = At<unsigned char>(kCities) + static_cast<size_t>(index) * kCityStride;
    const unsigned char text = city[kCityTextAt];
    if (text == 0xfe && std::memchr(city + kCityNameAt, 0, kCityNameSize) == nullptr) {
        core_log("picture: the player's city %d has its own name, not ended within %d bytes: file names start with "
                 "\"city\"",
                 index, kCityNameSize);
        return nullptr;
    }
    if (!g_cityNameOk) {
        return nullptr; // logged at Attach
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
        return nullptr; // logged by the guard
    }
    const unsigned char *name = call.name;
    if (!name) {
        core_log("picture: the game gave no name for the player's city %d: file names start with \"city\"", index);
        return nullptr;
    }
    static int logged = -1;
    if (logged != index * 256 + text) { // once per city
        logged = index * 256 + text;
        char what[80];
        std::snprintf(what, sizeof(what), "the player's city is %d of the empire, name %s %u:", index,
                      text == 0xfe ? "of its own (+8), marker" : "text 21 /", static_cast<unsigned>(text));
        cp::LogText(what, name, 64);
    }
    return name;
}

// The city screens without a window (no picture with
// any window, message, dialog or map open, the same in both games): the city (1), with a company selected (0x48) and
// controlling a god (0x49). Not the sidebar panels' dialogs (7) any more.
bool CityScreen()
{
    const char s = *kGameScreen;
    return s == 1 || s == 0x48 || s == 0x49;
}

int GameScreen()
{
    return static_cast<unsigned char>(*kGameScreen);
}

void TileSize(int *tileWidth, int *halfHeight)
{
    *tileWidth = *kTileWidth;
    *halfHeight = *kTileHalfHeight;
}

int ViewRows()
{
    return *kHeightRows;
}

void BeginCityDrawing(core::diag::Restore &r, core::diag::Restore &r2, const core::display::StripPlan &p, BYTE *buf,
                      int pitchPx)
{
    r.Set(kDrawBuffer.ptr(), buf);
    r.Set(kScreenPitch.ptr(), pitchPx * 2);
    r.Set(kDdWidth.ptr(), pitchPx);
    r.Set(kDdHeight.ptr(), p.bufH);
    r.Set(kClipLeft.ptr(), 0);
    r.Set(kClipTop.ptr(), 0);
    r.Set(kClipRight.ptr(), p.clipRight);
    r.Set(kClipBottom.ptr(), p.clipBottom);
    r.Set(kWidthTiles.ptr(), p.cols + 1);
    r.Set(kHeightRows.ptr(), p.viewRows + 2);
    r.Set(kOriginX.ptr(), 0);
    r.Set(kOriginY.ptr(), p.topPx);
    r.Save(kViewDrawnX.ptr());
    r.Save(kViewDrawnY.ptr());
    r.Set(kOverlay.ptr(), 0);
    r.Set(kPreviewShown.ptr(), 0);
    r2.Set(kMarkers.ptr(), 0);
    r2.Save(kRenderCount.ptr());
    r2.Save(kCountdown.ptr());
}

using MinimapDraw_fn = void(__cdecl *)(int, int, int, int);

// Emperor's minimap draws the map's range only: the area stays the core's (the view table's rows).
void DrawMinimap(core::diag::Restore &r, BYTE *buf, int pitchPx, int rows, int x0, int y0, RECT *)
{
    r.Set(kDrawBuffer.ptr(), buf);
    r.Set(kScreenPitch.ptr(), pitchPx * 2);
    r.Set(kDdWidth.ptr(), pitchPx);
    r.Set(kDdHeight.ptr(), rows);
    r.Set(kOverlay.ptr(), 0);
    // the view's frame below the minimap, in the part of the buffer that is cut away
    r.Set(kViewDrawnX.ptr(), 0);
    r.Set(kViewDrawnY.ptr(), kViewTableRows + 8);
    Function<MinimapDraw_fn>(kMinimapDraw)(x0, y0, kViewTableCols, kViewTableRows);
}

// What the drawings change on their own, copied for the picture's duration: the city's animation timers (held at 0
// meanwhile: no animation moves on between the strips) and the ambient sound's tile counts; the minimap's position,
// size and origin and the screen clip.
BYTE g_timers[kTimerFlagCount];
int g_ambient[kAmbientCountCount];
int g_minimapState[kMinimapStateCount], g_clip[4];

void Hold(cp::Drawing d)
{
    if (d == cp::Drawing::City) {
        std::memcpy(g_timers, At<BYTE>(kTimerFlags), sizeof(g_timers));
        std::memcpy(g_ambient, At<int>(kAmbientCounts), sizeof(g_ambient));
        std::memset(At<BYTE>(kTimerFlags), 0, kTimerFlagCount);
    }
    else {
        std::memcpy(g_minimapState, At<int>(kMinimapState), sizeof(g_minimapState));
        std::memcpy(g_clip, At<int>(kScreenClip), sizeof(g_clip));
    }
}

void Drawn(cp::Drawing)
{
}

void Release(cp::Drawing d)
{
    if (d == cp::Drawing::City) {
        std::memcpy(At<BYTE>(kTimerFlags), g_timers, sizeof(g_timers));
        std::memcpy(At<int>(kAmbientCounts), g_ambient, sizeof(g_ambient));
    }
    else {
        std::memcpy(At<int>(kMinimapState), g_minimapState, sizeof(g_minimapState));
        std::memcpy(At<int>(kScreenClip), g_clip, sizeof(g_clip));
    }
}

// Yongle draws a picture in any city_render, as the game's own drawing (no drawing state of its own to keep).
bool DrawingNow()
{
    return true;
}

bool AroundDrawing(void (*fn)(void *), void *ctx)
{
    fn(ctx);
    return true;
}

const cp::CityPictureGame kGame{
    &emperor::scroll::Working,
    &CityScreen,
    &GameScreen,
    &emperor::zoom::ViewLimits,
    &TileSize,
    kViewTableCols,
    kViewTableRows,
    &ViewRows,
    &BeginCityDrawing,
    &emperor::scroll::DrawCityForPicture,
    &DrawMinimap,
    &Hold,
    &Drawn,
    &Release,
    &DrawingNow,
    &AroundDrawing,
    &CityName,
};

} // namespace

void RegisterSettings()
{
    cp::RegisterSettings();
    // Only in test builds (the maps are desert outside the playable range, so there was nothing to choose).
    core::settings::Retire("ui", "city_picture_background", nullptr);
    core::settings::Retire("debug", "city_picture_name_probe", nullptr); // test builds only
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
    cp::Install(kGame);
    g_attached = true;
    return true;
}

bool Available()
{
    return g_attached && cp::Available();
}

void RequestCity()
{
    cp::RequestCity();
}

void RequestMinimap()
{
    cp::RequestMinimap();
}

} // namespace emperor::city_picture
