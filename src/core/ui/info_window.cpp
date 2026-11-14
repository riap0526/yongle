#include "core/ui/info_window.h"

#include "core/log.h"
#include "core/settings/settings.h"

#include <cstring>

namespace core::ui::info_window {

const char *const kChoices[kChoiceCount] = {"original", "centered", "cursor"};
const char *const kChoiceLabels[kChoiceCount] = {"original (above or below, as the game does it)",
                                                 "centered (in the city area)", "cursor (beside the cursor)"};

namespace {

Mode g_mode = Mode::Original;
const char *g_prefix = "";

bool Apply(int, const char *v, bool)
{
    int i = 0;
    while (i < kChoiceCount - 1 && std::strcmp(v, kChoices[i]) != 0) {
        ++i;
    }
    Set(static_cast<Mode>(i));
    return true;
}

} // namespace

void Watch(int index, const char *logPrefix)
{
    g_prefix = logPrefix ? logPrefix : "";
    core::settings::OnChange(index, &Apply);
}

Mode Current()
{
    return g_mode;
}

void Set(Mode mode)
{
    g_mode = mode;
    core_log("%sinfo window position %s (from the next window opened)", g_prefix, kChoices[static_cast<int>(mode)]);
}

POINT Position(Mode mode, const Area &a, int w, int h, int mouseX, int mouseY)
{
    constexpr int kGap = 24; // between the cursor and the window
    auto clamp = [](int v, int lo, int hi) { return v > hi ? (hi > lo ? hi : lo) : v < lo ? lo : v; };
    POINT p{a.left, a.top};
    if (mode == Mode::Centered) {
        p.x = a.left + (a.right - a.left - w) / 2;
        p.y = a.top + (a.bottom - a.top - h) / 2;
    }
    else if (mode == Mode::Cursor) {
        p.x = mouseX + kGap + w <= a.right ? mouseX + kGap : mouseX - kGap - w;
        p.y = mouseY - h / 2;
    }
    p.x = clamp(static_cast<int>(p.x), a.left, a.right - w);
    p.y = clamp(static_cast<int>(p.y), a.top, a.bottom - h);
    return p;
}

} // namespace core::ui::info_window
