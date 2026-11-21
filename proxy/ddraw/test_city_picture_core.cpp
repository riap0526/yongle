// Offline test of core/capture/city_picture (not the game): the city and minimap pictures on a fake game.
// The fake game keeps its variables in our memory, draws a strip by filling the
// buffer with each row's picture row number, and records the order of its callbacks. Checked: the four refusals
// (off, the panel open, not a city screen, still saving) and "not available" before Install; a request waits while the
// game says no drawing can be made now; the strips (count, the view's x and y, every picture row from the right strip);
// the order hold < begin < strips < drawn < put back < release, and r / r2 put back; a fault in a strip puts back,
// switches the pictures off and draws no more strips; a drawing the game cannot put back now is not saved and leaves
// the pictures on; the seam check's second picture; the minimap's two key colors (a pixel the game draws in either key
// color is kept, the corners outside the map black, the crop), two drawings that differ (the first one saved); the
// file name stem from the city's name. One process, in order; the pictures are put back to rest between the parts.
#include "core/capture/big_picture.h"
#include "core/capture/city_picture.h"
#include "core/diag/guard.h"
#include "core/display/city_strips.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "core/ui/notice.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cp = core::capture::city_picture;

// ---------------------------------------------------------------------------------------------
// What the code under test calls outside it

std::vector<core::display::ActionKey *> g_keys;
namespace core::display {
void AddActionKey(ActionKey *key)
{
    g_keys.push_back(key);
}
} // namespace core::display

bool g_panelOpen = false;
namespace core::ui {
bool IsOpen()
{
    return g_panelOpen;
}
} // namespace core::ui

std::vector<std::string> g_notices;
namespace core::ui::notice {
void Post(const char *text, bool red)
{
    g_notices.push_back(std::string(red ? "red:" : "") + text);
}
} // namespace core::ui::notice

namespace core::scroll::picture {
using AfterCity = void (*)(int orientation);
AfterCity g_after = nullptr;
void SetAfterCity(AfterCity fn)
{
    g_after = fn;
}
int g_pictureDrawing = 0; // "drawing into a buffer at 100%" (put back through r2)
void PictureDrawing(core::diag::Restore &r, int, int)
{
    r.Set(&g_pictureDrawing, 1);
}
} // namespace core::scroll::picture

struct Saved {
    core::capture::BigPicture *pic;
    std::string kind;
    int scale;
    void (*done)(bool, const char *);
};
std::vector<Saved> g_saved;
namespace core::capture {
bool SaveBig(BigPicture *pic, const BigOptions &o)
{
    g_saved.push_back(Saved{pic, o.kind, o.scale, o.done});
    return true;
}
} // namespace core::capture

core::settings::Apply g_apply[8] = {};
namespace core::settings {
int g_items = 0;
int Register(const Item &)
{
    return g_items++;
}
void OnChange(int index, Apply apply)
{
    if (index >= 0 && index < 8) {
        g_apply[index] = apply;
    }
}
} // namespace core::settings

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-100s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------------------------
// The fake game

constexpr int kTileWidth = 58, kHalfH = 15; // a column is 60 px
constexpr int kLeft = 10, kRight = 50, kTop = 20, kBottom = 120, kMapRows = 140;
constexpr int kTableCols = 40, kTableRows = 60;

bool g_works = true, g_cityScreen = true, g_now = true, g_canPutBack = true;
int g_screen = 1;
int g_faultStrip = -1;     // drawStrip faults on this call (0-based)
int g_minimapVariant = 0;  // 1: the second minimap drawing differs in one pixel
const char *g_name = "Agios Nikolaos!";

int g_drawBuffer = 0, g_marker = 5, g_stateA = 0; // the game's variables our callbacks change
BYTE *g_buf = nullptr;
int g_pitchPx = 0, g_bufH = 0;
std::vector<int> g_stripViewX, g_stripViewY;
int g_around = 0, g_aroundOk = 0;
std::string g_order;

void Order(const char *what)
{
    g_order += what;
    g_order += ' ';
}

bool Works() { return g_works; }
bool CityScreen() { return g_cityScreen; }
int Screen() { return g_screen; }
void Limits(int *l, int *r, int *t, int *b, int *m)
{
    *l = kLeft;
    *r = kRight;
    *t = kTop;
    *b = kBottom;
    *m = kMapRows;
}
void Tiles(int *w, int *h)
{
    *w = kTileWidth;
    *h = kHalfH;
}
int ViewRows() { return 30; }

void Begin(core::diag::Restore &r, core::diag::Restore &r2, const core::display::StripPlan &p, BYTE *buf,
           int pitchPx)
{
    Order("begin");
    r.Set(&g_drawBuffer, 1);
    r2.Set(&g_marker, 0);
    g_buf = buf;
    g_pitchPx = pitchPx;
    g_bufH = p.bufH;
}

void DrawStrip(int vx, int vy, int)
{
    const int call = static_cast<int>(g_stripViewY.size());
    g_stripViewX.push_back(vx);
    g_stripViewY.push_back(vy);
    if (g_drawBuffer != 1 || g_marker != 0 || core::scroll::picture::g_pictureDrawing != 1) {
        Order("unprepared");
    }
    if (call == g_faultStrip) {
        *static_cast<volatile int *>(nullptr) = 1;
    }
    // every buffer row holds the picture row it shows (topPx 0): (vy - top) x half row + the row
    auto *px = reinterpret_cast<std::uint16_t *>(g_buf);
    for (int y = 0; y < g_bufH; ++y) {
        const std::uint16_t v = static_cast<std::uint16_t>((vy - kTop) * kHalfH + y);
        for (int x = 0; x < g_pitchPx; ++x) {
            px[static_cast<size_t>(y) * g_pitchPx + x] = v;
        }
    }
}

// The minimap: a 20 x 10 block at (x0 + 4, y0 + 2) with two pixels in the key colors; outside it nothing -- or, with
// g_filler (as Zeus's), a filler colour on every other pixel of the view table's rows and the area narrowed to the
// block.
bool g_filler = false;

void DrawMinimap(core::diag::Restore &r, BYTE *buf, int pitchPx, int, int x0, int y0, RECT *area)
{
    r.Set(&g_drawBuffer, 2);
    static int calls = 0;
    ++calls;
    auto *px = reinterpret_cast<std::uint16_t *>(buf);
    if (g_filler) {
        for (int y = y0; y < y0 + kTableRows; ++y) {
            for (int x = 0; x < pitchPx; ++x) {
                px[static_cast<size_t>(y) * pitchPx + x] = 0x5555;
            }
        }
        *area = RECT{x0 + 4, y0 + 2, x0 + 24, y0 + 12};
    }
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 20; ++x) {
            px[static_cast<size_t>(y0 + 2 + y) * pitchPx + x0 + 4 + x] = static_cast<std::uint16_t>(0x100 + x);
        }
    }
    px[static_cast<size_t>(y0 + 2) * pitchPx + x0 + 4] = 0xF81F; // the game's own magenta
    px[static_cast<size_t>(y0 + 3) * pitchPx + x0 + 5] = 0x07E0; // and green
    px[static_cast<size_t>(y0 + 2) * pitchPx + x0 + 6] = 0;      // and black
    if (g_minimapVariant == 1 && calls % 2 == 0) {
        px[static_cast<size_t>(y0 + 5) * pitchPx + x0 + 9] = 0x1234;
    }
}

void Hold(cp::Drawing d)
{
    Order(d == cp::Drawing::City ? "hold" : "hold-minimap");
    g_stateA = 7;
}
void Drawn(cp::Drawing) { Order("drawn"); }
void Release(cp::Drawing)
{
    Order(g_drawBuffer == 0 && g_marker == 5 && core::scroll::picture::g_pictureDrawing == 0 ? "release"
                                                                                             : "release-early");
    g_stateA = 0;
}
bool Now() { return g_now; }
bool Around(void (*fn)(void *), void *ctx)
{
    ++g_around;
    if (!g_canPutBack) {
        return false;
    }
    fn(ctx);
    ++g_aroundOk;
    return true;
}
const unsigned char *Name()
{
    return reinterpret_cast<const unsigned char *>(g_name);
}

const cp::CityPictureGame kGame{&Works,     &CityScreen, &Screen, &Limits, &Tiles,   kTableCols, kTableRows,
                                &ViewRows,  &Begin,      &DrawStrip, &DrawMinimap, &Hold, &Drawn, &Release,
                                &Now,       &Around,     &Name};

void Fresh()
{
    for (Saved &s : g_saved) {
        delete s.pic;
    }
    g_saved.clear();
    g_notices.clear();
    g_stripViewX.clear();
    g_stripViewY.clear();
    g_order.clear();
    g_around = g_aroundOk = 0;
    g_works = g_cityScreen = g_now = g_canPutBack = true;
    g_panelOpen = false;
    g_faultStrip = -1;
    g_minimapVariant = 0;
}

bool LastNotice(const char *text)
{
    return !g_notices.empty() && g_notices.back() == text;
}

std::string Red(const char *text)
{
    return std::string("red:") + text;
}

core::display::ActionKey *Key(bool minimap)
{
    return g_keys.size() == 2 ? g_keys[minimap ? 1 : 0] : nullptr;
}

void Press(bool minimap)
{
    if (core::display::ActionKey *k = Key(minimap)) {
        k->fire();
    }
}

// Every picture row Y shows Y (the fake draws each row's number): no row from the wrong strip.
bool RowsRight(const core::capture::BigPicture *pic)
{
    for (int y = 0; y < pic->Height(); ++y) {
        const std::uint16_t *row = pic->Row(y);
        for (int x = 0; x < pic->Width(); ++x) {
            if (row[x] != static_cast<std::uint16_t>(y)) {
                std::printf("    row %d x %d: %u\n", y, x, row[x]);
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main()
{
    core_log_open(GetModuleHandleW(nullptr), "test_city_picture_core.log");
    core::diag::InstallExceptionLog(); // the guard catches the faults (as in the DLL)
    setvbuf(stdout, nullptr, _IONBF, 0);
    namespace notice = core::ui::notice;
    namespace sa = core::scroll::picture;

    // Settings: the five items.
    cp::RegisterSettings();
    Check(core::settings::g_items == 5, "RegisterSettings: five settings");

    // Before Install: the menu's request says "not available".
    cp::RequestCity();
    Check(g_notices.size() == 1 && g_notices[0] == Red(notice::kPictureUnavailable), "not installed: unavailable");
    Check(!cp::Available(), "not installed: Available is false");

    cp::Install(kGame);
    Check(cp::Available() && g_keys.size() == 2 && sa::g_after != nullptr,
          "Install: two keys and the after-city callback");
    Check(Key(false)->vk == 0x79 && Key(false)->mods == 0 && Key(true)->vk == 0x79 && Key(true)->mods != 0,
          "keys: F10 and Shift+F10");

    // The refusals.
    Fresh();
    g_works = false;
    Press(false);
    Check(LastNotice(Red(notice::kPictureUnavailable).c_str()), "drawing not working: unavailable");
    Fresh();
    g_panelOpen = true;
    Press(false);
    Check(LastNotice(Red(notice::kPictureClosePanel).c_str()), "panel open: close it first");
    Fresh();
    g_cityScreen = false;
    g_screen = 0x03;
    Press(true);
    Check(LastNotice(Red(notice::kPictureOnlyCity).c_str()), "not a city screen: only in the city");
    sa::g_after(0);
    Check(g_order.empty() && g_saved.empty(), "refused: nothing drawn at the next city_render");

    // A request waits while the game says no drawing can be made now.
    Fresh();
    g_now = false;
    Press(false);
    Check(g_notices.empty(), "asked for: no notice yet");
    sa::g_after(0);
    sa::g_after(0);
    Check(g_order.empty() && g_saved.empty(), "not now: nothing drawn, the request kept");
    g_now = true;
    sa::g_after(2);
    Check(g_saved.size() == 1, "now: one picture");
    core::display::StripInput in{kLeft, kRight, kTop, kBottom, kMapRows, kTileWidth + 2, kHalfH, 64, 4, 0};
    core::display::StripPlan plan;
    core::display::PlanStrips(in, &plan);
    bool views = static_cast<int>(g_stripViewY.size()) == plan.count;
    for (int k = 0; views && k < plan.count; ++k) {
        views = g_stripViewX[k] == kLeft && g_stripViewY[k] == core::display::StripAt(in, plan, k).viewY;
    }
    Check(views, "the strips: count, view x at the left limit, each strip's view y");
    Check(g_around == plan.count && g_aroundOk == plan.count, "each strip inside its own aroundDrawing");
    Check(!g_saved.empty() && g_saved[0].pic->Width() == plan.imageW && g_saved[0].pic->Height() == plan.imageH &&
              RowsRight(g_saved[0].pic),
          "the picture: its size, every row from the right strip");
    {
        const std::string want = "hold begin drawn release ";
        Check(g_order == want, "order: hold, begin, the strips, drawn, release (r and r2 put back before release)");
        if (g_order != want) {
            std::printf("    order: %s\n", g_order.c_str());
        }
    }
    Check(g_drawBuffer == 0 && g_marker == 5 && sa::g_pictureDrawing == 0 && g_stateA == 0,
          "after: the game's variables as before");
    Check(!g_saved.empty() && g_saved[0].kind == "Agios-Nikolaos" && g_saved[0].scale == 1,
          "file name: the city's letters and digits, '-' between");

    // Still saving: refused until the writer is done.
    g_notices.clear();
    Press(true);
    Check(LastNotice(Red(notice::kPictureBusy).c_str()), "still saving: busy");
    g_saved[0].done(true, "shots\\x.png");
    Check(LastNotice(notice::kCityPictureSaved), "done: city saved");

    // Strip rows 8 and the seam check: two pictures of the same moment, equal rows.
    Fresh();
    g_apply[3](3, "8", false);
    g_apply[4](4, "on", false);
    Press(false);
    sa::g_after(0);
    Check(g_saved.size() == 2 && g_saved[1].kind == "Agios-Nikolaos-fewest", "seam check: a second picture, -fewest");
    Check(g_saved.size() == 2 && RowsRight(g_saved[0].pic) && RowsRight(g_saved[1].pic) &&
              static_cast<int>(g_stripViewY.size()) > plan.count,
          "8 half rows a strip: more strips, both pictures right");
    g_saved[0].done(true, "a");
    g_saved[1].done(true, "b");
    g_apply[4](4, "off", false);

    // The drawing switched off after the key: the request dropped, not saved.
    Fresh();
    g_now = false;
    Press(false);
    sa::g_after(0);
    g_works = false;
    sa::g_after(0);
    g_works = g_now = true;
    sa::g_after(0);
    Check(g_saved.empty() && g_order.empty() && LastNotice(Red(notice::kPictureNotSaved).c_str()),
          "drawing off after the key: dropped, not saved");

    // A drawing the game cannot put back now: not saved, the pictures stay on.
    Fresh();
    g_canPutBack = false;
    Press(false);
    sa::g_after(0);
    Check(g_saved.empty() && LastNotice(Red(notice::kPictureNotSaved).c_str()) && g_stripViewY.empty(),
          "cannot put back: nothing drawn, not saved");
    Check(g_drawBuffer == 0 && g_marker == 5 && sa::g_pictureDrawing == 0 && g_stateA == 0,
          "cannot put back: the game's variables as before");
    g_canPutBack = true;
    g_notices.clear();
    Press(false);
    Check(g_notices.empty(), "cannot put back: the pictures are still on");
    sa::g_after(0);
    g_saved[0].done(true, "c");

    // The minimap: the game's own key colors kept, the crop, black outside.
    Fresh();
    g_apply[2](2, "4", false);
    Press(true);
    sa::g_after(0);
    Check(g_saved.size() == 1 && g_saved[0].kind == "Agios-Nikolaos_minimap" && g_saved[0].scale == 4,
          "minimap: saved, its file name, the scale");
    if (g_saved.size() == 1) {
        const core::capture::BigPicture *m = g_saved[0].pic;
        Check(m->Width() == 20 && m->Height() == 10, "minimap: cropped to what was drawn (20 x 10)");
        Check(m->Row(0)[0] == 0xF81F && m->Row(1)[1] == 0x07E0 && m->Row(0)[2] == 0 && m->Row(5)[7] == 0x107,
              "minimap: the game's magenta, green and black kept");
        g_saved[0].done(true, "m");
        Check(LastNotice(notice::kMinimapPictureSaved), "minimap: saved notice");
    }
    Check(cp::MinimapDifferencesForTest() == 0, "minimap: the two drawings equal");
    Check(g_around == 2 && g_order == "hold-minimap drawn release ", "minimap: two drawings, each in aroundDrawing");
    Check(g_drawBuffer == 0, "minimap: the game's variables as before");

    // A minimap that paints a filler around the map (Zeus's): cropped to the area the game layer gives.
    Fresh();
    g_filler = true;
    Press(true);
    sa::g_after(0);
    g_filler = false;
    Check(g_saved.size() == 1 && g_saved[0].pic->Width() == 20 && g_saved[0].pic->Height() == 10 &&
              g_saved[0].pic->Row(5)[7] == 0x107,
          "minimap with a filler: cropped to the game's area (20 x 10)");
    if (!g_saved.empty()) {
        g_saved[0].done(true, "mf");
    }

    // Two minimap drawings that differ: logged, the first saved.
    Fresh();
    g_minimapVariant = 1;
    Press(true);
    sa::g_after(0);
    Check(cp::MinimapDifferencesForTest() == 1 && g_saved.size() == 1 && g_saved[0].pic->Row(3)[5] == 0x105,
          "minimap drawings that differ: counted, the first saved");
    if (!g_saved.empty()) {
        g_saved[0].done(true, "m2");
    }

    // File name stems.
    char stem[48];
    g_name = "!!!";
    cp::StemForTest(stem, sizeof(stem));
    Check(std::strcmp(stem, "city") == 0, "stem: no letters -> city");
    g_name = nullptr;
    cp::StemForTest(stem, sizeof(stem));
    Check(std::strcmp(stem, "city") == 0, "stem: no name -> city");
    g_name = "  Mount  Olympus 2 ";
    cp::StemForTest(stem, sizeof(stem));
    Check(std::strcmp(stem, "Mount-Olympus-2") == 0, "stem: spaces between -> '-', none at the ends");
    g_name = "Agios Nikolaos!";

    // A fault in the third strip: put back, no more strips, the pictures off.
    Fresh();
    g_apply[3](3, "8", false);
    g_faultStrip = 2;
    Press(false);
    sa::g_after(0);
    Check(g_saved.empty() && LastNotice(Red(notice::kPictureNotSaved).c_str()), "fault: not saved");
    Check(g_stripViewY.size() == 3, "fault: no strip drawn after the one that faulted");
    Check(g_drawBuffer == 0 && g_marker == 5 && sa::g_pictureDrawing == 0 && g_stateA == 0 &&
              g_order == "hold begin drawn release ",
          "fault: the game's variables put back, released");
    g_notices.clear();
    Press(false);
    Check(LastNotice(Red(notice::kPictureUnavailable).c_str()), "fault: the pictures are off for the run");

    Fresh();
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
