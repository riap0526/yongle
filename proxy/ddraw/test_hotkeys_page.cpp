// Offline test of core/options/hotkeys_page (not the game): the Hotkeys window's page from a fake action table (the
// sections in order with their titles, our five keys first in Global hotkeys, the overlays named by the game layer's
// callback, an empty name keeping the setting's own, a dry run that asks for no name), and the debug panel's note on
// a key in use (ours answered here, the others by the game layer's callback, none without one). Each game's own page
// is tested in its test_restore (key_page).
#include "core/display/common_sizes.h"
#include "core/display/logical_size.h"
#include "core/display/virtual_display.h"
#include "core/game.h"
#include "core/input/keys.h"
#include "core/options/hotkeys_page.h"
#include "core/options/setting_rows.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace k = core::input::keys;
namespace co = core::options;
namespace hp = core::options::hotkeys_page;

// Not under test (the key bindings' and the resolution rows' system parts).
namespace core::display {
LRESULT SendToGame(UINT, WPARAM, LPARAM) { return 0; }
void SetKeyTranslator(KeyTranslator) {}
void SetKeyStateFilter(KeyStateFilter) {}
SIZE NativeSize() { return SIZE{1920, 1080}; }
void GameMinimum(int *w, int *h) { *w = 1024; *h = 768; }
int CommonSizes(int, int, int, int, bool, CommonSize *, int) { return 0; }
} // namespace core::display

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

using core::settings::Effect;
using core::settings::Item;
using core::settings::Type;

// In table order the overlay comes before the arrow: the page goes by section, then by table order.
const k::Def kDefs[] = {
    {Item{"keys", "overlay_water", "Overlay: water", "", Type::Hotkey, "W", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::Char, 'w', false, {"overlay_water", k::kOverlays, 14, 5}},
    {Item{"keys", "overlay_roads", "Overlay: roads", "", Type::Hotkey, "", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::Char, 'x', false, {"overlay_roads", k::kOverlays, 14, 19}},
    {Item{"keys", "scroll_up", "Scroll up", "", Type::Hotkey, "Up", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::KeyDown, VK_UP, false, {"scroll_up", k::kArrows, 0, 0}},
    {Item{"keys", "open_options", "Open Options", "", Type::Hotkey, "F6", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::Options, 0, false, {"open_options", k::kGlobal, 0, 0}},
    {Item{"keys", "pause", "Toggle pause", "", Type::Hotkey, "P", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::Char, 'p', false, {"pause", k::kCity, 0, 0}},
    {Item{"keys", "army_set_1", "Set army group 1", "", Type::Hotkey, "Ctrl+1", Effect::Live, false, true, 0, 0, 0, nullptr, nullptr, nullptr, 0},
     k::Send::KeyDown, '1', true, {"army_set_1", k::kArmy, 0, 0}},
};

volatile int g_ctrl = 0;
const k::KeysGame kGame(kDefs, static_cast<int>(sizeof(kDefs) / sizeof(kDefs[0])), &k::Never, &k::Never, &g_ctrl,
                        &k::NoOptions, &k::NoEscape, &k::NoCall, false);

int g_names = 0;
std::string OverlayName(int group, int index)
{
    ++g_names;
    if (group == 14 && index == 5) {
        return hp::OverlayWord("See Water");
    }
    return ""; // the roads: no game text
}

const hp::KeyPageGame kPageGame(&OverlayName);

const char *GameNote(unsigned vk, unsigned)
{
    return vk == 'P' ? "The game uses P." : nullptr;
}

const core_game_info kInfo = {"test", "Tester", "dev", L"Tester", L"test.ini", "test.ini", 1024, 768, nullptr,
                              nullptr, nullptr, nullptr, nullptr};

void Page()
{
    // Our keys, as the core's settings and the city pictures register them (here with the shared page names).
    const char *const ours[][2] = {{"ui", "panel_key"},         {"debug", "screenshot_key"}, {"debug", "report_key"},
                                   {"ui", "city_picture_key"}, {"ui", "minimap_picture_key"}};
    for (const auto &o : ours) {
        Item it{};
        it.section = o[0];
        it.key = o[1];
        it.label = o[1];
        it.help = "";
        it.type = Type::Hotkey;
        it.defaultValue = "";
        it.effect = Effect::Live;
        it.inPanel = it.editable = true;
        core::settings::Register(it);
    }
    k::RegisterSettings(&kGame);

    co::setting_rows::SetDry(true);
    const co::Page dry = hp::Build(kPageGame);
    co::setting_rows::SetDry(false);
    Check(g_names == 0 && co::setting_rows::Listed().size() == 5 + 6,
          "dry run: every key setting collected, no overlay name asked for");
    co::setting_rows::Clear();

    const co::Page p = hp::Build(kPageGame);
    Check(p.name == "Hotkeys" && p.categories.size() == 1, "the page: Hotkeys, one category");
    const auto &rows = p.categories[0].rows;
    std::string got;
    for (const co::Row &r : rows) {
        got += r.kind == co::Kind::Header ? "[" + r.label + "]" : r.kind == co::Kind::Columns ? std::string("|") : r.id;
        got += " ";
    }
    const std::string want = "| [Arrow keys] scroll_up [Global hotkeys] panel_key screenshot_key report_key "
                             "city_picture_key minimap_picture_key open_options [City hotkeys] pause [Building menus] "
                             "[Overlays] overlay_water overlay_roads [Bookmarks] [Army groups] army_set_1 ";
    if (got != want) {
        std::printf("  got: %s\n", got.c_str());
    }
    Check(got == want, "the page: the sections in order (empty ones too), ours first in Global hotkeys");
    Check(rows[0].rightLabel == "Hotkey" && rows[0].rightLabel2 == "Alternative" && rows[0].label.empty(),
          "the page: the column titles");
    const co::Row *water = nullptr, *roads = nullptr, *panel = nullptr;
    for (const co::Row &r : rows) {
        water = r.id == "overlay_water" ? &r : water;
        roads = r.id == "overlay_roads" ? &r : roads;
        panel = r.id == "panel_key" ? &r : panel;
    }
    Check(water && water->label == "Water overlay" && water->kind == co::Kind::Key, "an overlay: the game's name");
    Check(roads && roads->label == "Overlay: roads" && g_names == 2, "an overlay without a name: the setting's own");
    Check(panel && panel->label == "Debug panel" &&
              panel->help == "Opens and closes Tester's panel for testing and finding problems. Most settings are in "
                             "Options.",
          "our key: the shared name, {product} filled in");
    Check(hp::OverlayWord("See water") == "water" && hp::OverlayWord("Roads") == "Roads" &&
              hp::OverlayWord("See") == "See",
          "OverlayWord: only a leading \"See \" goes");
}

void Notes()
{
    using core::ui::kModAlt;
    using core::ui::kModShift;
    auto note = [](unsigned vk, unsigned mods, hp::GameKeyNote game) {
        const char *n = hp::KeyNote(vk, mods, game);
        return std::string(n ? n : "");
    };
    const std::string window =
        "Tester also uses this key to switch between window and full screen; that no longer works with it.";
    Check(note(VK_F5, 0, &GameNote) == window && note(VK_RETURN, kModAlt, &GameNote) == window,
          "note: F5 and Alt+Enter are ours");
    Check(note(VK_F7, kModShift, &GameNote) ==
              "Tester also uses this key to change the resolution; that no longer works with it.",
          "note: Shift+F7 is ours");
    Check(note(VK_F7, 0, &GameNote).empty() && note(VK_F5, kModShift, &GameNote).empty() &&
              note(VK_RETURN, 0, &GameNote).empty(),
          "note: F7, Shift+F5 and Enter alone are not ours (the game's callback: nothing)");
    Check(note('P', 0, &GameNote) == "The game uses P.", "note: other keys: the game's callback");
    Check(note('P', 0, nullptr).empty() && note(VK_F5, 0, nullptr) == window,
          "note: without a callback: ours only");
}

} // namespace

int main()
{
    core_game_set(&kInfo);
    Page();
    Notes();
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
