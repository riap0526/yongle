// Offline test of core/scroll/picture (not the game): smooth scrolling's picture on a fake game -- its variables in an
// array of ours (through core::site::g_imageShift, as test_camera does), a fake city drawing that records what it sees
// and draws marks into draw_buffer, fake conversions with the games' bounds (no tile outside origin + viewport), no
// zoom. Checked: the drawing into our buffer with the grid clip made larger by
// the remainder and the extent, shown offset into the gap left of the sidebar and on the last row, nothing on the
// sidebar or below the screen; a clip past the screen's bottom shown to the bottom only; the drawing plain without a
// remainder or an extent; conversions moved and made larger (not again while drawing), points outside the city as
// shown are no tile; the Game paused width; the city as shown for the menus; the drag selection's start corner (the
// remainder and 2 px per column) and its mouse kept to the city; a drawing for a picture without the remainder; and a
// fault while the game draws: put back, off, drawn once more with drawCityAgain (only then), the next drawings with
// drawCity. The scenarios run in order in one process (the fault last: a feature switched off stays off).
#include "core/diag/guard.h"
#include "core/log.h"
#include "core/scroll/camera.h"
#include "core/scroll/picture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

// What core/scroll/picture calls outside the code under test.
namespace core::capture {
bool SaveOne(const void *, int, int, int, const char *, const char *) { return true; }
} // namespace core::capture
namespace core::display::framed {
int g_skipped = 0;
void NoteCitySkipped(const char *, int, int, int, int) { ++g_skipped; }
} // namespace core::display::framed

namespace camera = core::scroll::camera;
namespace picture = core::scroll::picture;

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

// The fake game's memory: one 4-byte slot per variable at 0x500000 + 4 x index.
enum Slot {
    kViewX, kViewY, kTileW, kTileHalfH, kCursorX, kCursorY, kDesktopW, kDesktopH, kKeyL, kKeyR, kKeyU, kKeyD,
    kScrolling, kSpeed, kFrame, kOriginX, kOriginY, kCols, kRows, kClipL, kClipT, kClipR, kClipB, kDrawBuffer,
    kMouseX, kMouseY, kPitch, kDdW, kDdH, kMouseMode, kCursorTile, kSlots
};
// After the variables, 64 bytes: the ground's height of each tile (64 tiles).
int g_mem[kSlots + 16];
constexpr DWORD kBase = 0x500000, kHeightsAt = kBase + 4 * kSlots;
signed char *Heights()
{
    return reinterpret_cast<signed char *>(&g_mem[kSlots]);
}
camera::IntVar V(Slot s)
{
    return camera::IntVar{kBase + 4 * static_cast<DWORD>(s)};
}
int &M(Slot s)
{
    return g_mem[s];
}

constexpr int kW = 1920, kH = 1080, kSidebar = 1734;
constexpr WORD kSentinel = 0x0f0f;
std::vector<WORD> g_back;

WORD Back(int x, int y)
{
    return g_back[static_cast<size_t>(y) * kW + x];
}

// The city viewport as viewport_init leaves it: tiles 58 x 15 (a column 60 px, a row pair 30 px) from (0, 30).
void Viewport(int cols, int rows)
{
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
}

void BackBuffer(int h)
{
    g_back.assign(static_cast<size_t>(kW) * (h + 64), kSentinel);
    M(kDrawBuffer) = static_cast<int>(reinterpret_cast<DWORD>(g_back.data()));
    M(kPitch) = kW * 2;
    M(kDdW) = kW;
    M(kDdH) = h;
}

bool ViewportIsGame(int cols, int rows)
{
    return M(kOriginX) == 0 && M(kOriginY) == 30 && M(kCols) == cols && M(kRows) == rows &&
           M(kClipR) == 60 * cols - 2 && M(kClipB) == 30 + (rows - 1) * 15 &&
           M(kDrawBuffer) == static_cast<int>(reinterpret_cast<DWORD>(g_back.data()));
}

// The fake city drawing (as the game's city_render): what it sees, marks drawn into draw_buffer, a conversion asked
// while drawing, a fault on purpose.
struct Seen {
    int draws = 0, again = 0;
    DWORD buffer = 0;
    int clipR = 0, clipB = 0, cols = 0, rows = 0;
    int mouseTile = -1, mouseOx = 0, mouseCols = 0;
} g_seen;
struct Mark {
    int x, y;
    WORD v;
};
Mark g_marks[4];
int g_markCount = 0;
bool g_askMouse = false, g_fault = false;
volatile int *volatile g_null = nullptr;

int FakeMouseToGrid(int);
const picture::PictureGame *g_game = nullptr;

void Draw()
{
    g_seen.buffer = static_cast<DWORD>(M(kDrawBuffer));
    g_seen.clipR = M(kClipR);
    g_seen.clipB = M(kClipB);
    g_seen.cols = M(kCols);
    g_seen.rows = M(kRows);
    WORD *buf = reinterpret_cast<WORD *>(g_seen.buffer);
    for (int i = 0; i < g_markCount; ++i) {
        buf[g_marks[i].y * (M(kPitch) / 2) + g_marks[i].x] = g_marks[i].v;
    }
    if (g_askMouse) {
        g_seen.mouseTile = picture::MouseToGrid(*g_game, 0, &FakeMouseToGrid);
    }
    if (g_fault) {
        g_fault = false;
        (void)*g_null; // a fault inside the game's drawing
    }
}

void FakeDrawCity(int, int, int)
{
    ++g_seen.draws;
    Draw();
}

void FakeDrawCityAgain(int, int, int)
{
    ++g_seen.again;
    Draw();
}

// The conversions, with the games' bounds: no tile (0) outside origin + viewport.
struct ConvSeen {
    int calls = 0, ox = 0, oy = 0, cols = 0, rows = 0, x = 0, y = 0;
} g_conv;

int Tile(int x, int y)
{
    ++g_conv.calls;
    g_conv.ox = M(kOriginX);
    g_conv.oy = M(kOriginY);
    g_conv.cols = M(kCols);
    g_conv.rows = M(kRows);
    g_conv.x = x;
    g_conv.y = y;
    if (x < M(kOriginX) || x >= M(kOriginX) + (M(kTileW) + 2) * M(kCols) || y < M(kOriginY) ||
        y >= M(kOriginY) + M(kTileHalfH) * M(kRows)) {
        return 0;
    }
    return 7;
}

int FakeScreenToGrid(int x, int y, int)
{
    return Tile(x, y);
}

int FakeMouseToGrid(int)
{
    const int r = Tile(M(kMouseX), M(kMouseY));
    g_seen.mouseOx = g_conv.ox;
    g_seen.mouseCols = g_conv.cols;
    return r;
}

void FakeGridToScreen(int, int, int *x, int *y) // the origin (the view's first tile)
{
    *x = M(kOriginX);
    *y = M(kOriginY);
}

int FakeCityWidth()
{
    return (M(kTileW) + 2) * M(kCols);
}

int g_dragSeen[2] = {};
void FakeDrag(int *drag)
{
    g_dragSeen[0] = drag[2];
    g_dragSeen[1] = drag[3];
}

int g_clamps = 0, g_fallBacks = 0;
void FakeClamp()
{
    ++g_clamps;
}

core::diag::Feature g_feature{"smooth scrolling (test_picture)"};
const camera::CameraGame kCamera{
    "test",
    &g_feature,
    camera::CameraVars{V(kViewX), V(kViewY), V(kTileW), V(kTileHalfH), V(kCursorX), V(kCursorY), V(kDesktopW),
                       V(kDesktopH), V(kKeyL), V(kKeyR), V(kKeyU), V(kKeyD), V(kScrolling), V(kSpeed), V(kFrame)},
    &FakeClamp,
    [] {},
    [](unsigned) { return false; },
    [] {
        ++g_fallBacks;
        picture::Reset();
    },
    [] { return false; }, // no zoom in this test
    [] {},
    [](double *, double *) { return false; },
};

constexpr DWORD kStartCorner = 0x401111, kDrawCorner = 0x402222; // the drag corner's grid_to_screen calls
const picture::PictureGame kGame{
    "test",
    &kCamera,
    picture::PictureVars{V(kOriginX), V(kOriginY), V(kCols), V(kRows), V(kClipL), V(kClipT), V(kClipR), V(kClipB),
                         core::site::Var<BYTE *>{kBase + 4 * kDrawBuffer}, V(kMouseX), V(kMouseY), V(kPitch), V(kDdW),
                         V(kDdH), core::site::Var<unsigned char>{kBase + 4 * kMouseMode}, V(kCursorTile),
                         core::site::Var<signed char>{kHeightsAt}},
    0x4d,
    30,
    64,
    kStartCorner,
    kDrawCorner,
    &FakeDrawCity,
    &FakeDrawCityAgain,
    [] { return 100; },
    [](int *w, int *h) { *w = *h = 0; },
    [](picture::RenderCall &) { return false; },
    [](core::diag::Restore &) {},
    [](picture::ZoomMap *) { return false; },
};

// ---------------------------------------------------------------------------------------------

void Plain()
{
    Viewport(28, 71);
    BackBuffer(kH);
    picture::SetCityExtent(0, 0); // the game's own layout
    camera::SetRemainderForTest(kCamera, 0, 0);
    g_markCount = 0;
    picture::CityRender(kGame, 0, 0, 0);
    Check(g_seen.draws == 1 && g_seen.again == 0 && g_seen.buffer == static_cast<DWORD>(M(kDrawBuffer)) &&
              g_seen.clipR == 1678,
          "no remainder, no extent: the game draws plainly into the back buffer (drawCity once)");
    Check(picture::ScreenToGrid(kGame, 1700, 500, 0, &FakeScreenToGrid) == 0 && g_conv.calls == 1 &&
              g_conv.ox == 0 && g_conv.cols == 28,
          "... and converts plainly (the game decides: past its viewport no tile)");
    Check(picture::CityWidth(kGame, &FakeCityWidth) == 1680, "... the Game paused width the game's");
}

void RenderShifted()
{
    Viewport(28, 71);
    BackBuffer(kH);
    picture::SetCityExtent(kSidebar, kH);
    camera::SetRemainderForTest(kCamera, 5, 6);
    g_marks[0] = {1700 + 5, 500 + 6, 0x1111}; // the gap left of the sidebar
    g_marks[1] = {100 + 5, 1079 + 6, 0x2222}; // the last row
    g_markCount = 2;
    M(kMouseX) = 500;
    M(kMouseY) = 500;
    g_askMouse = true;
    const int draws = g_seen.draws;
    picture::CityRender(kGame, 0, 0, 0);
    g_askMouse = false;
    Check(g_seen.draws == draws + 1 && g_seen.again == 0 &&
              g_seen.buffer != static_cast<DWORD>(reinterpret_cast<DWORD>(g_back.data())),
          "with a remainder: drawn once, into our buffer");
    Check(g_seen.clipR == 1678 + 120 && g_seen.clipB == 1080 + 30 && g_seen.cols == 30 && g_seen.rows == 73,
          "the clip 2 columns (remainder + the 56 px gap) and a row pair larger, the viewport +2 / +2");
    Check(g_seen.mouseTile == 7 && g_seen.mouseOx == -5 && g_seen.mouseCols == 30,
          "a conversion while drawing: the origin moved, the viewport not made larger again");
    Check(ViewportIsGame(28, 71), "afterwards: draw_buffer, clip and viewport the game's");
    Check(Back(1700, 500) == 0x1111 && Back(100, 1079) == 0x2222,
          "shown offset by the remainder: (1705, 506) at (1700, 500) in the gap, (105, 1085) on the last row");
    Check(Back(kSidebar, 500) == kSentinel && Back(100, kH) == kSentinel && Back(5, 29) == kSentinel,
          "nothing on the sidebar, below the screen or on the top bar");
    int left, top, right, bottom;
    const char *why = nullptr;
    Check(picture::DrawnCity(kGame, &left, &top, &right, &bottom, &why) && left == 0 && top == 30 &&
              right == kSidebar && bottom == kH,
          "the city as shown (the menus' background): (0, 30) .. (1734, 1080)");
    Check(picture::CityWidth(kGame, &FakeCityWidth) == kSidebar, "the Game paused width: up to the sidebar");
}

void Conversions()
{
    camera::SetRemainderForTest(kCamera, 5, 6);
    g_conv = {};
    Check(picture::ScreenToGrid(kGame, 10, 50, 0, &FakeScreenToGrid) == 7 && g_conv.ox == -5 && g_conv.oy == 24 &&
              g_conv.cols == 30 && g_conv.rows == 73 && ViewportIsGame(28, 71),
          "screen_to_grid: the origin moved, the viewport 2 columns (5 + the 56 px gap) and a row pair larger, for "
          "the call only");
    g_conv = {};
    Check(picture::ScreenToGrid(kGame, 1750, 500, 0, &FakeScreenToGrid) == 0 && g_conv.calls == 0,
          "a point on the sidebar: no tile, the game not asked");
    Check(picture::ScreenToGrid(kGame, 500, 25, 0, &FakeScreenToGrid) == 0 && g_conv.calls == 0,
          "a point on the top bar: no tile");
    Check(picture::ScreenToGrid(kGame, 1700, 500, 0, &FakeScreenToGrid) == 7 && g_conv.calls == 1,
          "a point in the gap: a tile (with a remainder)");
    // A large remainder and the gap together pass one column (52 + 56 > 60): the viewport is made larger by as many
    // columns as they need, so the gap's last pixel is a tile too (seen in testing).
    camera::SetRemainderForTest(kCamera, 52, 0);
    g_conv = {};
    Check(picture::ScreenToGrid(kGame, 1733, 500, 0, &FakeScreenToGrid) == 7 && g_conv.cols == 30 &&
              ViewportIsGame(28, 71),
          "a large remainder: the gap's last pixel (1733) a tile, the viewport 2 columns larger for the call");
    camera::SetRemainderForTest(kCamera, 5, 6);
    Check(picture::ScreenToGridRaw(kGame, 10, 50, 0, &FakeScreenToGrid) == 7 && g_conv.ox == -5 && g_conv.cols == 30,
          "screen_to_grid_unbounded: moved the same way");
    int sx = 0, sy = 0;
    picture::GridToScreen(kGame, 0x403333, 1, 2, &sx, &sy, &FakeGridToScreen);
    Check(sx == -5 && sy == 24 && ViewportIsGame(28, 71), "grid_to_screen outside the drawing: moved with the city");
    camera::SetRemainderForTest(kCamera, 0, 0);
    g_conv = {};
    M(kMouseX) = 1700;
    M(kMouseY) = 500;
    Check(picture::MouseToGrid(kGame, 0, &FakeMouseToGrid) == 7 && g_conv.ox == 0 && g_conv.cols == 29 &&
              ViewportIsGame(28, 71),
          "the gap at rest: a tile (the viewport a column larger, the origin not moved)");
}

void Drag()
{
    int drag[16] = {0, 0, 300, 400};
    camera::SetRemainderForTest(kCamera, 5, 6);
    M(kViewX) = 0;
    M(kMouseX) = 500;
    M(kMouseY) = 500;
    M(kMouseMode) = 0;
    picture::DragStart(kGame, drag, &FakeDrag);
    Check(drag[11] == 10 && drag[12] == 10, "a drag pressed on the city: the smallest frame 8 px (+ 2)");
    camera::SetRemainderForTest(kCamera, 12, 10);
    M(kViewX) = 2; // two columns scrolled since the start
    camera::SetRemainderForTest(kCamera, 12, 10);
    picture::DragAdjusted(kGame, drag, &FakeDrag, true);
    Check(g_dragSeen[0] == 300 - 7 - 4 && g_dragSeen[1] == 400 - 4 && drag[2] == 300 && drag[3] == 400,
          "the start corner moved by the remainder's change and 2 px per column, for the call only");
    M(kMouseMode) = 0x4d;
    M(kMouseX) = 1850; // dragged onto the sidebar
    M(kMouseY) = 600;
    g_conv = {};
    picture::MouseToGrid(kGame, 0, &FakeMouseToGrid);
    Check(g_conv.x == kSidebar - 1 && M(kMouseX) == 1850, "the drag's mouse kept to the city as shown, put back");
    M(kCursorTile) = 3;
    Heights()[3] = 2;
    int sx = 0, sy = 0;
    picture::GridToScreen(kGame, kDrawCorner, 1, 2, &sx, &sy, &FakeGridToScreen);
    Check(sx == kSidebar - 1 && sy == 600 + 2 * 30, "the frame's corner: the cursor (kept) plus the ground's height");
    M(kMouseMode) = 0;
    int side[16] = {};
    M(kMouseX) = 1850;
    picture::DragStart(kGame, side, &FakeDrag);
    Check(side[11] == 4 && side[12] == 4, "a drag pressed on the sidebar: the game's own smallest frame");
}

void ClipPastBottom()
{
    Viewport(28, 67); // 1013 high: the clip at 1020, 7 rows past the screen (Zeus's layout)
    BackBuffer(1013);
    picture::SetCityExtent(kSidebar, 1013);
    camera::SetRemainderForTest(kCamera, 5, 6);
    g_marks[0] = {100 + 5, 1012 + 6, 0x3333};
    g_marks[1] = {100 + 5, 1013 + 6, 0x4444};
    g_markCount = 2;
    picture::CityRender(kGame, 0, 0, 0);
    Check(g_seen.clipB == 1020 + 30 && g_seen.buffer != static_cast<DWORD>(reinterpret_cast<DWORD>(g_back.data())),
          "a clip past the screen's bottom: drawn into our buffer all the same (one row pair more)");
    Check(Back(100, 1012) == 0x3333 && Back(100, 1013) == kSentinel,
          "shown to the screen's bottom only, nothing written below it");
    int left, top, right, bottom;
    const char *why = nullptr;
    Check(picture::DrawnCity(kGame, &left, &top, &right, &bottom, &why) && bottom == 1013,
          "the city as shown ends at the screen's bottom");
}

void PictureWithoutRemainder()
{
    Viewport(28, 71);
    BackBuffer(kH);
    picture::SetCityExtent(kSidebar, kH);
    camera::SetRemainderForTest(kCamera, 5, 6);
    core::diag::Restore r;
    picture::PictureDrawing(r, kW, kH);
    int sx = 0, sy = 0;
    picture::GridToScreen(kGame, 0x403333, 1, 2, &sx, &sy, &FakeGridToScreen);
    g_conv = {};
    M(kMouseX) = 500;
    M(kMouseY) = 500;
    const int tile = picture::MouseToGrid(kGame, 0, &FakeMouseToGrid);
    r.Undo();
    int px, py;
    camera::Remainder(&px, &py);
    Check(sx == 0 && sy == 30 && tile == 7 && g_conv.ox == 0 && g_conv.cols == 28,
          "a drawing for a picture (a figure portrait): its conversions the game's own");
    Check(px == 5 && py == 6, "afterwards the remainder back");
}

void Fault()
{
    Viewport(28, 71);
    BackBuffer(kH);
    picture::SetCityExtent(kSidebar, kH);
    camera::SetRemainderForTest(kCamera, 5, 6);
    g_markCount = 0;
    const int draws = g_seen.draws;
    g_fault = true;
    picture::CityRender(kGame, 0, 0, 0);
    Check(!core::diag::Enabled(g_feature) && g_fallBacks == 1, "a fault while the game draws: off, the fallback once");
    Check(ViewportIsGame(28, 71), "draw_buffer, clip and viewport put back");
    Check(g_seen.draws == draws + 1 && g_seen.again == 1 && g_seen.clipR == 1678,
          "drawn once more with drawCityAgain (only after the fault), the game's own clip");
    int px, py;
    camera::Remainder(&px, &py);
    Check(px == 0 && py == 0, "remainder 0");
    picture::CityRender(kGame, 0, 0, 0);
    Check(g_seen.draws == draws + 2 && g_seen.again == 1 && g_seen.clipR == 1678,
          "afterwards (off): drawn with drawCity, not as an extra drawing");
    g_conv = {};
    picture::ScreenToGrid(kGame, 10, 50, 0, &FakeScreenToGrid);
    Check(g_conv.ox == 0 && g_conv.cols == 28, "conversions pass through");
}

} // namespace

int main()
{
    DeleteFileA("test_picture.log");
    core_log_open(GetModuleHandleW(nullptr), "test_picture.log");
    core::diag::InstallExceptionLog();
    core::site::g_imageShift = reinterpret_cast<DWORD>(g_mem) - kBase;
    g_game = &kGame;
    M(kDesktopW) = kW;
    M(kDesktopH) = kH;
    M(kSpeed) = 50;
    camera::Start(kCamera, true);
    Plain();
    RenderShifted();
    Conversions();
    Drag();
    ClipPastBottom();
    PictureWithoutRemainder();
    Fault();
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
