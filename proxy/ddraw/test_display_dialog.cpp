// Offline test of core/options/display_dialog (no DLL, no game): the scale steps and status lines; on a fake game
// layer, what the dialog draws, what its buttons set, and that Cancel or closing it without OK puts the resolution and
// scale back while OK keeps them; an error while drawing switches it off. The settings file used is the core's
// neutral engine.ini next to this exe (no game is set), removed at the end.
#include "core/config.h"
#include "core/diag/guard.h"
#include "core/game.h"
#include "core/options/display_dialog.h"
#include "core/settings/settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cs = core::settings;
namespace o = core::options::display_dialog;

// The display parts the dialog reads (core/display/virtual_display), as a 2560 x 1440 monitor in full screen.
namespace core::display {
SIZE NativeSize()
{
    return SIZE{2560, 1440};
}
bool Windowed()
{
    return false;
}
} // namespace core::display

namespace {

int g_failures = 0;
wchar_t g_path[MAX_PATH];

void Check(bool ok, const char *what)
{
    std::printf("%-72s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

std::string Value(const char *key)
{
    char v[cs::kValueSize];
    cs::Value(cs::Find("video", key), v, sizeof(v));
    return v;
}

// The fake game layer.
int g_closes = 0, g_hover = 0, g_behind = 0, g_ends = 0, g_panels = 0;
bool g_shown = true, g_behindNow = true, g_faultInPanel = false;
int g_w = 1896, g_h = 1066;
std::vector<int> g_buttons, g_arrows, g_hovered;
std::vector<std::string> g_texts;

void Close()
{
    ++g_closes;
    g_shown = false;
}
bool Shown()
{
    return g_shown;
}
void Size(int *w, int *h)
{
    *w = g_w;
    *h = g_h;
}
int Hover()
{
    return g_hover;
}
bool Behind()
{
    if (!g_behindNow) {
        return false;
    }
    ++g_behind;
    return true;
}
void Panel(int, int, int, int)
{
    ++g_panels;
    if (g_faultInPanel) {
        *static_cast<volatile int *>(nullptr) = 1;
    }
}
void Button(const o::Item &it, bool hovered)
{
    g_buttons.push_back(it.id);
    if (hovered) g_hovered.push_back(it.id);
}
void Arrow(const o::Item &it, bool)
{
    g_arrows.push_back(it.id);
}
void GameText(o::GameText t, int, int, int, o::TextLook)
{
    g_texts.push_back("#" + std::to_string(o::kTextIndex[static_cast<int>(t)]));
}
void Text(const char *s, int, int, int, o::TextLook)
{
    g_texts.push_back(s);
}
void End()
{
    ++g_ends;
}

const o::Game kGame(&Close, &Shown, &Size, &Hover, &Behind, &Panel, &Button, &Arrow, &GameText, &Text, &End);

bool Has(const char *s)
{
    for (const std::string &t : g_texts) {
        if (t == s) return true;
    }
    return false;
}

void DrawOnce()
{
    g_buttons.clear();
    g_arrows.clear();
    g_hovered.clear();
    g_texts.clear();
    o::Draw();
}

void PressAndApply(int id)
{
    o::HandlerOf(id)();
    cs::RunPending();
}

} // namespace

int main()
{
    // The pure parts.
    Check(o::MaxScale(2560, 1440, 1024, 768) == 185, "2560x1440: the largest scale is 185% (1383x778)");
    Check(o::MaxScale(1920, 1080, 1024, 768) == 140, "1920x1080: the largest scale is 140% (1371x771)");
    Check(o::MaxScale(1024, 768, 1024, 768) == 100, "1024x768: only 100%");
    Check(o::MaxScale(3840, 2160, 1024, 768) == 250, "3840x2160: up to 250% (1536x864)");
    Check(o::StepScale(250, 1, 3840, 2160, 1024, 768) == 250, "+ at 250% on 4K stays");
    Check(o::StepScale(0, 1, 2560, 1440, 1024, 768) == 135, "+ from auto (133% at 1440p) -> 135%");
    Check(o::StepScale(0, -1, 2560, 1440, 1024, 768) == 130, "- from auto -> 130%");
    Check(o::StepScale(0, 1, 3840, 2160, 1024, 768) == 205, "+ from auto (200% at 4K) -> 205%");
    Check(o::StepScale(125, 1, 2560, 1440, 1024, 768) == 130, "+ from 125% -> 130%");
    Check(o::StepScale(185, 1, 2560, 1440, 1024, 768) == 185, "+ at the largest scale stays");
    Check(o::StepScale(100, -1, 2560, 1440, 1024, 768) == 100, "- at 100% stays");
    Check(o::StepScale(250, -1, 2560, 1440, 1024, 768) == 180, "- from a scale too large: from the one in effect");
    char a[48], b[48];
    o::StatusLines(2048, 1152, true, 125, 125, a, sizeof(a), b, sizeof(b));
    Check(std::strcmp(a, "Resolution: 2048 x 1152") == 0 && std::strcmp(b, "Display scale: 125%") == 0,
          "status: scaled 125%");
    o::StatusLines(1896, 1066, true, 0, 135, a, sizeof(a), b, sizeof(b));
    Check(std::strcmp(b, "Display scale: Auto (135%)") == 0, "status: auto");
    o::StatusLines(1024, 768, false, 125, 125, a, sizeof(a), b, sizeof(b));
    Check(std::strcmp(a, "Resolution: 1024 x 768") == 0 && std::strcmp(b, "Display scale: off") == 0,
          "status: a fixed size");

    // The layout: nine buttons inside the panel, none overlapping.
    bool inside = true, apart = true;
    for (const o::Item &it : o::Items()) {
        inside = inside && it.x >= o::kPanelX && it.y >= o::kPanelY && it.x + it.w <= o::kPanelX + 16 * o::kPanelCols &&
                 it.y + it.h <= o::kPanelY + 16 * o::kPanelRows;
        for (const o::Item &other : o::Items()) {
            if (&other != &it && it.x < other.x + other.w && other.x < it.x + it.w && it.y < other.y + other.h &&
                other.y < it.y + it.h) {
                apart = false;
            }
        }
    }
    Check(inside && apart, "layout: the nine buttons inside the panel, none overlapping");
    Check(o::ItemOf(o::kMinus).arrow == o::Arrow::Down && o::ItemOf(o::kPlus).arrow == o::Arrow::Up &&
              o::ItemOf(o::kMinus).x < o::ItemOf(o::kPlus).x,
          "layout: smaller on the left, larger on the right");

    // Settings: the neutral engine.ini next to this exe.
    DWORD n = GetModuleFileNameW(nullptr, g_path, MAX_PATH);
    while (n > 0 && g_path[n - 1] != L'\\') --n;
    wcscpy(g_path + n, core_game()->ini_file);
    if (FILE *f = _wfopen(g_path, L"wb")) {
        std::fputs("[video]\r\nresolution = scaled\r\nscale = 125\r\nscaling = nearest\r\n", f);
        std::fclose(f);
    }
    core::diag::InstallExceptionLog();
    core_config_load(nullptr);

    Check(!o::Draw() && !o::Working(), "before Attach: nothing drawn");
    o::Attach(kGame);
    DrawOnce();
    Check(g_panels == 1 && g_ends == 1 && g_buttons.size() == 7 && g_arrows.size() == 2,
          "drawn: the panel, seven text buttons, two arrows, the end");
    Check(Has("#0") && Has("#2") && Has("#4") && Has("#5") && !Has("#1"),
          "drawn: the game's title, Windowed screen (in full screen), the game's own size, Cancel");
    Check(Has("100%") && Has("Auto") && Has("Scaling: nearest") && Has("OK") &&
              Has("Resolution: 1896 x 1066") && Has("Display scale: 125%"),
          "drawn: our buttons' text and the two status lines");
    g_hover = o::kOk;
    DrawOnce();
    Check(g_hovered.size() == 1 && g_hovered[0] == o::kOk, "the button under the mouse drawn as such");

    PressAndApply(o::kPlus);
    Check(Value("scale") == "130" && Value("resolution") == "scaled", "+: scale 130");
    PressAndApply(o::kNativeButton);
    Check(Value("resolution") == "1024x768", "1024 x 768: the game's own size");
    PressAndApply(o::kScalingButton);
    Check(Value("scaling") == "linear", "scaling: nearest -> linear (saved at once)");
    g_w = 2048;
    g_h = 1152;
    g_behindNow = false;
    DrawOnce();
    Check(g_behind == 0, "laid out again, the game cannot draw behind now: asked again later");
    g_behindNow = true;
    DrawOnce();
    Check(g_behind == 1, "... drawn behind at the next drawing");
    DrawOnce();
    Check(g_behind == 1, "... once");
    PressAndApply(o::kCancelButton);
    Check(g_closes == 1 && Value("resolution") == "scaled" && Value("scale") == "125",
          "Cancel: closed, resolution and scale put back");
    Check(Value("scaling") == "linear", "Cancel: the scaling (saved at once) stays");

    // Opened again: OK keeps.
    g_shown = true;
    o::OnLoop(); // still shown: nothing
    DrawOnce();
    PressAndApply(o::k100);
    PressAndApply(o::kOk);
    o::OnLoop();
    Check(g_closes == 2 && Value("scale") == "100", "OK: closed, scale 100 kept");

    // Opened again, left with Esc (the game closes it): put back at the next loop.
    g_shown = true;
    o::OnLoop();
    DrawOnce();
    PressAndApply(o::kAuto);
    Check(Value("scale") == "auto", "Auto: scale auto");
    g_shown = false;
    o::OnLoop();
    cs::RunPending();
    Check(Value("scale") == "100", "closed without OK: scale put back");

    // An error while drawing: switched off, Draw says so.
    g_shown = true;
    g_faultInPanel = true;
    Check(!o::Draw() && !o::Working(), "an error while drawing: switched off");
    g_faultInPanel = false;
    Check(!o::Draw(), "... and stays off");

    o::DetachForTest();
    DeleteFileW(g_path);
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
