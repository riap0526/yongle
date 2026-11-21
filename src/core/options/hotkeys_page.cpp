#include "core/options/hotkeys_page.h"

#include "core/input/keys.h"
#include "core/options/menu_text.h"
#include "core/options/setting_rows.h"
#include "core/settings/settings.h"
#include "core/ui/hotkey.h"

#include <cstdio>
#include <utility>

namespace core::options::hotkeys_page {

namespace keys = core::input::keys;
namespace mt = core::options::menu_text;
namespace sr = core::options::setting_rows;

Page Build(const KeyPageGame &game)
{
    Category c{"", {}};
    Row columns;
    columns.kind = Kind::Columns;
    columns.label = ""; // no "Action" (the titles are above the list)
    columns.rightLabel = mt::kColumnKey;
    columns.rightLabel2 = mt::kColumnAlternative;
    c.rows.push_back(std::move(columns));
    for (int s = 0; s < keys::kSectionCount; ++s) {
        sr::Header(c.rows, mt::KeySection(s));
        if (s == keys::kGlobal) { // ours first
            sr::Add(c.rows, "ui", "panel_key");
            sr::Add(c.rows, "debug", "screenshot_key");
            sr::Add(c.rows, "debug", "report_key");
            sr::Add(c.rows, "ui", "city_picture_key");
            sr::Add(c.rows, "ui", "minimap_picture_key");
        }
        for (int a = 0; a < keys::ActionCount(); ++a) {
            const keys::ActionInfo &info = keys::Action(a);
            if (info.section != s) {
                continue;
            }
            const size_t before = c.rows.size();
            sr::Add(c.rows, "keys", info.key);
            if (c.rows.size() == before || sr::Dry() || info.textGroup <= 0) {
                continue;
            }
            // The overlays: the game's own names, "See Water" as "Water overlay" (Augustus's).
            const std::string name = game.overlayName(info.textGroup, info.textIndex);
            if (!name.empty()) {
                char label[128];
                std::snprintf(label, sizeof(label), mt::kOverlayName, name.c_str());
                c.rows.back().label = label;
            }
        }
    }
    return Page{mt::kHotkeysTitle, {std::move(c)}};
}

std::string OverlayWord(const std::string &gameText)
{
    return gameText.compare(0, 4, "See ") == 0 ? gameText.substr(4) : gameText;
}

const char *KeyNote(unsigned vk, unsigned mods, GameKeyNote gameNote)
{
    static std::string text;
    const char *ours = nullptr;
    if ((vk == VK_F5 && mods == 0) || (vk == VK_RETURN && mods == core::ui::kModAlt)) {
        ours = mt::kKeyNoteWindow;
    }
    else if (vk == VK_F7 && mods == core::ui::kModShift) {
        ours = mt::kKeyNoteResolution;
    }
    if (ours) {
        text = core::settings::GameNames(ours);
        return text.c_str();
    }
    return gameNote ? gameNote(vk, mods) : nullptr;
}

} // namespace core::options::hotkeys_page
