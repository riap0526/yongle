// Offline test of core/ui/notice (not the game): on a fake game layer, the texts posted wait until the next frame
// and are handed over in order (at most 4 per frame, cut to 99 characters, red kept); the latest zoom wins and is
// handed after the texts; before Install the texts go to our caption line (core/output SetNotice) and the zoom is
// dropped; while the game shows no boxes (a screen that clears them) the texts and the zoom wait and are handed over
// once it does (seen in testing); a fault in the game layer's box switches the feature off, after which
// the texts go to the caption line again and the frames hand nothing over. The scenarios run in order in one process (the fault last: a feature
// switched off stays off).
#include "core/diag/guard.h"
#include "core/log.h"
#include "core/ui/notice.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace notice = core::ui::notice;

// core/output's side (not linked): the caption line.
std::vector<std::string> g_caption;
namespace core::output {
void SetNotice(const char *utf8, unsigned)
{
    g_caption.push_back(utf8);
}
} // namespace core::output

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

struct Box {
    std::string text;
    bool red;
};
std::vector<Box> g_boxes;
std::vector<int> g_zooms;
std::vector<char> g_order; // 'b' a box, 'z' the zoom: the zoom after the texts of the same frame
volatile int *volatile g_null = nullptr;
bool g_faultNext = false;
bool g_shows = true; // the game shows its boxes on this frame's screen

bool ShowsBoxes()
{
    return g_shows;
}

void NewBox(const char *text, bool red)
{
    if (g_faultNext) {
        g_faultNext = false;
        (void)*g_null; // a fault inside the game layer's box, as a game function faulting would be
    }
    g_boxes.push_back(Box{text, red});
    g_order.push_back('b');
}

void ZoomBox(int percent)
{
    g_zooms.push_back(percent);
    g_order.push_back('z');
}

void Clear()
{
    g_boxes.clear();
    g_zooms.clear();
    g_order.clear();
    g_caption.clear();
}

} // namespace

int main()
{
    core_log_open(GetModuleHandleW(nullptr), "test_notice.log");
    core::diag::InstallExceptionLog();

    // Before Install: the caption line, no frame hands anything over, the zoom is dropped.
    notice::Post("before");
    notice::Zoom(75);
    notice::OnFrame();
    Check(!notice::Attached() && g_caption.size() == 1 && g_caption[0] == "before" && g_boxes.empty() &&
              g_zooms.empty(),
          "not installed: the text on the caption line, no box, the zoom dropped");
    Clear();

    notice::Install(notice::Game{"test: notice", notice::Game::Red::kNone, &NewBox, &ZoomBox, &ShowsBoxes});
    Check(notice::Attached(), "installed");

    // Waiting for the frame, in order, red kept.
    notice::Post("one");
    notice::Post("two", true);
    Check(g_boxes.empty() && g_caption.empty(), "posted: nothing shown before the frame");
    notice::OnFrame();
    Check(g_boxes.size() == 2 && g_boxes[0].text == "one" && !g_boxes[0].red && g_boxes[1].text == "two" &&
              g_boxes[1].red,
          "the frame: both boxes in order, the second red");
    notice::OnFrame();
    Check(g_boxes.size() == 2, "the next frame: nothing again");
    Clear();

    // At most 4 per frame; the rest are dropped (they are in the log).
    for (int k = 0; k < 6; ++k) {
        char t[16];
        std::snprintf(t, sizeof(t), "n%d", k);
        notice::Post(t);
    }
    notice::OnFrame();
    Check(g_boxes.size() == 4 && g_boxes[3].text == "n3", "six posted in one frame: the first four shown");
    Clear();

    // Cut to 99 characters.
    std::string longText(150, 'x');
    notice::Post(longText.c_str());
    notice::OnFrame();
    Check(g_boxes.size() == 1 && g_boxes[0].text.size() == static_cast<size_t>(notice::kTextSize - 1),
          "a 150-character text: cut to 99");
    Clear();

    // The zoom: the latest wins, after the texts of the same frame; once.
    notice::Zoom(50);
    notice::Zoom(75);
    notice::Post("with zoom");
    notice::OnFrame();
    Check(g_zooms.size() == 1 && g_zooms[0] == 75 && g_order.size() == 2 && g_order[0] == 'b' && g_order[1] == 'z',
          "two zooms in one frame: the latest, after the text");
    notice::OnFrame();
    Check(g_zooms.size() == 1, "the next frame: no zoom again");
    notice::Zoom(0);
    notice::OnFrame();
    Check(g_zooms.size() == 2 && g_zooms[1] == 0, "zoom 0 is a value, not nothing");
    Clear();

    // The zoom format (the same text in both games).
    char zoomText[32];
    std::snprintf(zoomText, sizeof(zoomText), notice::kZoomFormat, 125);
    Check(std::strcmp(zoomText, "Zoom: 125%") == 0, "the zoom box's text");

    // A screen where the game clears its boxes: the texts and the zoom wait, in order, until the city.
    g_shows = false;
    notice::Post("saved");
    notice::Post("busy", true);
    notice::Zoom(125);
    notice::OnFrame();
    notice::OnFrame();
    Check(g_boxes.empty() && g_zooms.empty(), "no boxes on this screen: nothing handed over, nothing dropped");
    g_shows = true;
    notice::OnFrame();
    Check(g_boxes.size() == 2 && g_boxes[0].text == "saved" && g_boxes[1].text == "busy" && g_boxes[1].red &&
              g_zooms.size() == 1 && g_zooms[0] == 125,
          "back on the city: both texts in order (red kept) and the zoom");
    Clear();

    // A fault in the game layer's box: the feature off, then the caption line; frames hand nothing over.
    g_faultNext = true;
    notice::Post("faults");
    notice::Post("after the fault, same frame");
    notice::OnFrame();
    Check(g_boxes.empty(), "the box faulted: abandoned (the rest of that frame too)");
    notice::Post("later");
    notice::Zoom(100);
    notice::OnFrame();
    Check(g_boxes.empty() && g_zooms.empty() && g_caption.size() == 1 && g_caption[0] == "later",
          "after the fault: the text on the caption line, no box, no zoom box");

    std::printf("%s\n", g_failures == 0 ? "test_notice: all passed" : "test_notice: FAILED");
    return g_failures == 0 ? 0 : 1;
}
