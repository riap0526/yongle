// Offline test of core/scroll/zoom (not the game): the city's zoom on a fake game -- the picture's and the camera's
// variables in an array of ours (through core::site::g_imageShift, as test_picture does), a fake layout that asks the
// zoom for the view (as the games' viewport hooks), a fake city drawing that fills what it may draw and records what
// it sees. Checked: nothing before Install; the wheel (high-resolution steps added up, the
// other way starts again, 25 a notch, wheel up zooms in) and the middle button over the city as shown on a city screen
// only, not while dragging or during a military drag selection; the cap and the map's limit; another map laid out
// again (OnViewClamp); the drawn view written after a relayout (not by Off); the zoomed city drawn into our buffer and
// shrunk / enlarged onto the city's part of the screen only; a view larger than the map not drawn; a scaling that
// fails switches the zoom off (laid out at 100, the game's drawing not made again). One process, in order; the zoom
// is put back to rest between the parts (ResetForTest).
#include "core/diag/guard.h"
#include "core/display/city_zoom.h"
#include "core/log.h"
#include "core/scroll/camera.h"
#include "core/scroll/picture.h"
#include "core/scroll/zoom.h"
#include "core/settings/settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

// What the code under test calls outside it.
namespace core::capture {
bool SaveOne(const void *, int, int, int, const char *, const char *) { return true; }
} // namespace core::capture
namespace core::display::framed {
void NoteCitySkipped(const char *, int, int, int, int) {}
} // namespace core::display::framed
namespace core::display {
bool (*g_filter)(UINT, WPARAM, int, int) = nullptr;
void SetMouseFilter(bool (*filter)(UINT, WPARAM, int, int))
{
    g_filter = filter;
}
} // namespace core::display
namespace core::ui::notice {
int g_zoomShown = -1;
void Zoom(int percent)
{
    g_zoomShown = percent;
}
} // namespace core::ui::notice
namespace core::settings {
Apply g_apply[4] = {};
int g_items = 0;
int Register(const Item &)
{
    return g_items++;
}
void OnChange(int index, Apply apply)
{
    if (index >= 0 && index < 4) {
        g_apply[index] = apply;
    }
}
} // namespace core::settings

namespace camera = core::scroll::camera;
namespace picture = core::scroll::picture;
namespace zoom = core::scroll::zoom;
namespace cd = core::display;

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

enum Slot {
    kViewX, kViewY, kTileW, kTileHalfH, kCursorX, kCursorY, kDesktopW, kDesktopH, kKeyL, kKeyR, kKeyU, kKeyD,
    kScrolling, kSpeed, kFrame, kOriginX, kOriginY, kCols, kRows, kClipL, kClipT, kClipR, kClipB, kDrawBuffer,
    kMouseX, kMouseY, kPitch, kDdW, kDdH, kMouseMode, kCursorTile, kDrawnX, kDrawnY, kRedraw, kSidebarX, kSlots
};
int g_mem[kSlots + 16];
constexpr DWORD kBase = 0x500000, kHeightsAt = kBase + 4 * kSlots;
camera::IntVar V(Slot s)
{
    return camera::IntVar{kBase + 4 * static_cast<DWORD>(s)};
}
int &M(Slot s)
{
    return g_mem[s];
}

constexpr int kW = 1920, kH = 1080, kSidebar = 1734, kShowH = kH - 30;
constexpr WORD kSentinel = 0x0f0f, kCity = 0x1234;
constexpr int kMiddle = 0x72; // viewport_init writes the drawn view as the map's middle
std::vector<WORD> g_back;

WORD Back(int x, int y)
{
    return g_back[static_cast<size_t>(y) * kW + x];
}

void BackBuffer()
{
    g_back.assign(static_cast<size_t>(kW) * (kH + 64), kSentinel);
    M(kDrawBuffer) = static_cast<int>(reinterpret_cast<DWORD>(g_back.data()));
    M(kPitch) = kW * 2;
    M(kDdW) = kW;
    M(kDdH) = kH;
}

// The fake game's map and screens.
int g_mapCols = 200, g_mapRows = 400, g_clampRows = 396;
bool g_cityScreen = true;
int g_relayouts = 0, g_clamps = 0, g_hookedClamps = 0;
bool g_ownLayout = false; // the game's own layout (its sizes, an editor): ResetView, not ViewFor

// The layout (as the games' viewport hooks): 28 x 71 at 100%, the zoomed view otherwise; viewport_init's variables.
void Layout()
{
    ++g_relayouts;
    int cols = 28, rows = 71;
    if (g_ownLayout) {
        zoom::ResetView();
    }
    else {
        zoom::ViewFor(kSidebar, kShowH, 60, 15, &cols, &rows);
    }
    M(kTileW) = 58;
    M(kTileHalfH) = 15;
    M(kOriginX) = 0;
    M(kOriginY) = 30;
    M(kCols) = cols;
    M(kRows) = rows;
    M(kClipL) = 0;
    M(kClipT) = 30;
    M(kClipR) = 60 * cols - 2;
    M(kClipB) = 30 + (rows - 1) * 15;
    M(kDrawnX) = kMiddle;
    M(kDrawnY) = kMiddle;
}

// The fake city drawing: what it sees, and the whole clip filled with kCity (as the game draws the city there).
struct Seen {
    int draws = 0, again = 0, cols = 0, rows = 0;
    DWORD buffer = 0;
} g_seen;

void Fill()
{
    g_seen.buffer = static_cast<DWORD>(M(kDrawBuffer));
    g_seen.cols = M(kCols);
    g_seen.rows = M(kRows);
    WORD *buf = reinterpret_cast<WORD *>(g_seen.buffer);
    const int pitch = M(kPitch) / 2;
    for (int y = M(kClipT); y < M(kClipB) && y < M(kDdH); ++y) {
        for (int x = M(kClipL); x < M(kClipR) && x < M(kDdW); ++x) {
            buf[static_cast<size_t>(y) * pitch + x] = kCity;
        }
    }
}

void FakeDrawCity(int, int, int)
{
    ++g_seen.draws;
    Fill();
}

void FakeDrawAgain(int, int, int)
{
    ++g_seen.again;
    Fill();
}

core::diag::Feature g_scrollFeature{"smooth scrolling (test_zoom)"};
const camera::CameraGame kCamera{
    "test",
    &g_scrollFeature,
    camera::CameraVars{V(kViewX), V(kViewY), V(kTileW), V(kTileHalfH), V(kCursorX), V(kCursorY), V(kDesktopW),
                       V(kDesktopH), V(kKeyL), V(kKeyR), V(kKeyU), V(kKeyD), V(kScrolling), V(kSpeed), V(kFrame)},
    [] { ++g_hookedClamps; }, // the game layer's hook (the camera clamps through it)
    [] {},
    [](unsigned) { return false; },
    [] {
        picture::Reset();
        zoom::Off("smooth scrolling was switched off after an error");
    },
    &zoom::CameraMoving,
    &zoom::CameraStep,
    &zoom::CameraScale,
};

const picture::PictureGame kPicture{
    "test",
    &kCamera,
    picture::PictureVars{V(kOriginX), V(kOriginY), V(kCols), V(kRows), V(kClipL), V(kClipT), V(kClipR), V(kClipB),
                         core::site::Var<BYTE *>{kBase + 4 * kDrawBuffer}, V(kMouseX), V(kMouseY), V(kPitch), V(kDdW),
                         V(kDdH), core::site::Var<unsigned char>{kBase + 4 * kMouseMode}, V(kCursorTile),
                         core::site::Var<signed char>{kHeightsAt}},
    0x4d,
    30,
    64,
    0x401111,
    0x402222,
    &FakeDrawCity,
    &FakeDrawAgain,
    &zoom::Percent,
    &zoom::BufferSize,
    &zoom::RenderZoomed,
    &zoom::DragDrawZoom,
    &zoom::DragLinesMap,
};

const zoom::ZoomGame kZoom{
    "test",
    &kPicture,
    V(kDrawnX),
    V(kDrawnY),
    core::site::Var<char>{kBase + 4 * kRedraw},
    V(kSidebarX),
    150,
    [](int *cols, int *rows, int *clampRows) {
        *cols = g_mapCols;
        *rows = g_mapRows;
        *clampRows = g_clampRows;
    },
    [] { return g_cityScreen; },
    &Layout,
    [] { ++g_clamps; }, // the game's own view_clamp
    [] { return core::diag::Enabled(g_scrollFeature); },
};

bool Wheel(int delta, int x = 500, int y = 500, WORD keys = 0)
{
    return cd::g_filter(WM_MOUSEWHEEL, MAKEWPARAM(keys, static_cast<WORD>(static_cast<short>(delta))), x, y);
}

int Expected(int percent)
{
    return cd::ZoomFor({percent, kSidebar, kShowH, 60, 15, g_mapCols, g_mapRows, g_clampRows, 150}).percent;
}

void Rest()
{
    zoom::ResetForTest();
    g_mapCols = 200;
    g_mapRows = 400;
    g_clampRows = 396;
    g_cityScreen = true;
    M(kMouseMode) = 0;
    M(kViewX) = 50;
    M(kViewY) = 100;
    M(kSidebarX) = kSidebar;
    BackBuffer();
    picture::SetCityExtent(kSidebar, kH);
    camera::SetRemainderForTest(kCamera, 0, 0);
}

// ---------------------------------------------------------------------------------------------

void BeforeInstall()
{
    Rest();
    Layout();
    picture::RenderCall c{0, 0, 0, false, {}};
    Check(zoom::Percent() == 100 && M(kCols) == 28 && M(kRows) == 71 && !zoom::CameraMoving() &&
              !zoom::RenderZoomed(c),
          "not installed: 100%, the layout's view, not moving, the usual drawing");
    core::settings::g_apply[0](0, "150", true); // city_zoom = 150 in the settings file
    Layout();
    Check(zoom::Percent() == 100 && M(kCols) == 28, "not installed, city_zoom 150: still 100%");
}

void WheelAndButton()
{
    Rest();
    zoom::Install(kZoom);
    Layout();
    Check(Wheel(60) && zoom::Percent() == 100 && !zoom::CameraMoving(), "half a notch (a fine wheel): taken, nothing yet");
    Check(Wheel(-60), "half a notch the other way: taken ...");
    zoom::CameraStep();
    Check(zoom::Percent() == 100, "... and started again (no notch)");
    Wheel(-60);
    const int relayouts = g_relayouts;
    zoom::CameraStep();
    Check(zoom::Percent() == 125 && g_relayouts == relayouts + 1 && core::ui::notice::g_zoomShown == 125,
          "the second half towards the player: 125%, laid out once, the zoom box shows 125");
    Check(M(kDrawnX) == M(kViewX) && M(kDrawnY) == M(kViewY), "after the relayout the drawn view is the view");
    Check(!Wheel(-120, 1800, 500), "over the sidebar: the game's");
    Check(!Wheel(-120, 500, 20), "over the top bar: the game's");
    g_cityScreen = false;
    Check(!Wheel(-120), "not a city screen: the game's");
    g_cityScreen = true;
    Check(Wheel(-120, 500, 500, MK_RBUTTON), "the right button held: taken ...");
    zoom::CameraStep();
    Check(zoom::Percent() == 125, "... no zoom");
    M(kMouseMode) = 0x4d;
    Check(Wheel(-120), "a military drag selection: taken ...");
    zoom::CameraStep();
    Check(zoom::Percent() == 125, "... no zoom");
    M(kMouseMode) = 0;
    Wheel(-360); // three notches: capped at 150
    zoom::CameraStep();
    Check(zoom::Percent() == 150, "three notches out: the cap (150)");
    Check(cd::g_filter(WM_MBUTTONDOWN, 0, 500, 500), "the middle button over the city: taken");
    zoom::CameraStep();
    Check(zoom::Percent() == 100 && M(kCols) == 28, "the middle button: back to 100%");
    Wheel(240);
    zoom::CameraStep();
    Check(zoom::Percent() == 50 && M(kCols) < 28, "two notches up (away from the player): 50%, a smaller view");
}

void MapLimit()
{
    Rest();
    zoom::Install(kZoom);
    core::settings::g_apply[0](0, "150", true);
    Layout();
    Check(zoom::Percent() == 150, "city_zoom 150 on a large map: 150%");
    g_mapCols = 60;
    g_mapRows = 100;
    g_clampRows = 93;
    const int relayouts = g_relayouts, clamps = g_clamps;
    zoom::OnViewClamp();
    Check(g_relayouts == relayouts + 1 && g_clamps == clamps + 1 && zoom::Percent() == Expected(150) &&
              zoom::Percent() < 150 && cd::ViewFitsMap(M(kCols), M(kRows), 60, 100, 93),
          "another map: laid out again (and clamped) at once, capped by the map, the view inside it");
    zoom::OnViewClamp();
    Check(g_relayouts == relayouts + 1, "the same map again: not laid out again");
    core::settings::g_apply[0](0, "100", false);
    Check(zoom::Percent() == 100 && M(kCols) == 28 && M(kRedraw) == 1, "city_zoom 100 from the panel: at once, redrawn");
}

// The build cost text in the city as shown; a loaded game starts at the starting zoom.
int g_seenClip[2] = {};
void FakeCostText()
{
    g_seenClip[0] = M(kClipR);
    g_seenClip[1] = M(kClipB);
}

void CostTextAndLoad()
{
    Rest();
    zoom::Install(kZoom);
    core::settings::g_apply[0](0, "125", true); // the starting zoom
    Layout();
    Wheel(240); // 125 -> 75
    zoom::CameraStep();
    const int clipR = M(kClipR), clipB = M(kClipB);
    Check(zoom::Percent() == 75 && clipR < kSidebar && clipB < kH, "75%: the city clip (the buffer's) smaller than the screen");
    zoom::ShownClipCall(&FakeCostText);
    Check(g_seenClip[0] == kSidebar && g_seenClip[1] == kH && M(kClipR) == clipR && M(kClipB) == clipB,
          "the cost text: drawn with the city as shown on the screen as its clip, the clip back afterwards");
    const int relayouts = g_relayouts;
    zoom::OnGameLoaded();
    Check(zoom::Percent() == 125 && g_relayouts == relayouts + 1 && M(kRedraw) == 1,
          "a game loaded: the starting zoom (125%) again, laid out at once");
    core::settings::g_apply[0](0, "100", false);
    zoom::ShownClipCall(&FakeCostText);
    Check(g_seenClip[0] == M(kClipR) && g_seenClip[1] == M(kClipB), "100%: the cost text with the game's own clip");
}

// A zoom asked while the game lays out its own view (its own sizes, an editor): laid out once when the map changes,
// not on every frame (the side-by-side review before acceptance).
void OwnLayout()
{
    Rest();
    zoom::Install(kZoom);
    core::settings::g_apply[0](0, "150", true);
    Layout();
    g_ownLayout = true;
    Layout(); // into the game's own layout at 150% asked
    g_mapCols = 120; // another map (the editor's)
    const int relayouts = g_relayouts;
    zoom::OnViewClamp();
    picture::RenderCall c{0, 0, 0, false, {}};
    for (int i = 0; i < 5; ++i) {
        zoom::OnViewClamp();
        zoom::RenderZoomed(c);
    }
    Check(zoom::Percent() == 100 && g_relayouts == relayouts + 1,
          "the game's own layout with 150% asked: laid out once for the new map, not again on every frame");
    g_ownLayout = false;
}

void Drawing()
{
    Rest();
    zoom::Install(kZoom);
    core::settings::g_apply[0](0, "150", true);
    Layout();
    const int cols = M(kCols), rows = M(kRows);
    const DWORD back = reinterpret_cast<DWORD>(g_back.data());
    picture::CityRender(kPicture, 0, 0, 0);
    Check(g_seen.draws == 1 && g_seen.again == 0 && g_seen.buffer != back && g_seen.cols == cols + 1 &&
              g_seen.rows == rows + 2,
          "150%: drawn once into our buffer, the view a column and a row pair larger meanwhile");
    Check(static_cast<DWORD>(M(kDrawBuffer)) == back && M(kCols) == cols && M(kDdH) == kH && M(kPitch) == kW * 2,
          "afterwards draw_buffer, the viewport, dd_height and the pitch the game's");
    Check(Back(0, 30) == kCity && Back(kSidebar - 1, kH - 1) == kCity && Back(800, 500) == kCity,
          "shrunk onto the city's part of the screen, corner to corner");
    Check(Back(kSidebar, 500) == kSentinel && Back(500, 29) == kSentinel && Back(500, kH) == kSentinel,
          "nothing on the sidebar, the top bar or below the screen");
    core::settings::g_apply[0](0, "50", false);
    BackBuffer();
    picture::CityRender(kPicture, 0, 0, 0);
    Check(zoom::Percent() == 50 && g_seen.draws == 2 && Back(0, 30) == kCity && Back(kSidebar - 1, kH - 1) == kCity &&
              Back(kSidebar, 500) == kSentinel,
          "50%: enlarged onto the city's part of the screen only");
    // A view larger than the map (the viewport changed behind the zoom's back): not drawn this frame.
    core::settings::g_apply[0](0, "150", false);
    M(kCols) = 500;
    BackBuffer();
    picture::CityRender(kPicture, 0, 0, 0);
    Check(g_seen.draws == 2 && Back(500, 500) == kSentinel, "a view larger than the map: not drawn this frame");
    Layout();
    // The scaling fails: the zoom off, laid out at 100 at once (the drawn view is the next drawing's to write), the
    // game's drawing not made again.
    zoom::FailScalingForTest(true);
    const int relayouts = g_relayouts;
    picture::CityRender(kPicture, 0, 0, 0);
    Check(g_seen.draws == 3 && g_seen.again == 0 && zoom::Percent() == 100 && M(kCols) == 28 &&
              g_relayouts == relayouts + 1,
          "the scaling fails: drawn once, the zoom off at 100% and laid out at once");
    Check(M(kDrawnX) == kMiddle, "Off does not write the drawn view (as Yongle's)");
    Check(!Wheel(-120), "afterwards the wheel is the game's");
    core::settings::g_apply[0](0, "150", false);
    Check(zoom::Percent() == 100, "city_zoom from the panel: still 100% (off for the rest of the run)");
    zoom::FailScalingForTest(false);
}

} // namespace

int main()
{
    DeleteFileA("test_zoom.log");
    core_log_open(GetModuleHandleW(nullptr), "test_zoom.log");
    core::diag::InstallExceptionLog();
    core::site::g_imageShift = reinterpret_cast<DWORD>(g_mem) - kBase;
    M(kDesktopW) = kW;
    M(kDesktopH) = kH;
    M(kSpeed) = 50;
    zoom::RegisterSettings();
    camera::Start(kCamera, false); // whole tiles: the wheel's target at once
    BeforeInstall();
    WheelAndButton();
    MapLimit();
    CostTextAndLoad();
    OwnLayout();
    Drawing();
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
