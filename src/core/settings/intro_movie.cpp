#include "core/settings/intro_movie.h"

#include "core/settings/settings.h"

#include <cstring>

namespace core::settings::intro_movie {
namespace {

bool g_skip = false;

} // namespace

const char *const kChoices[kChoiceCount] = {"play", "skip"};
const char *const kChoiceLabels[kChoiceCount] = {"play (the game's intro movie at start-up)",
                                                 "skip (straight to the main menu)"};

void Watch(int index)
{
    core::settings::OnChange(index, [](int, const char *v, bool initial) {
        if (initial) {
            g_skip = std::strcmp(v, "skip") == 0;
        }
        return true;
    });
}

bool Skip()
{
    return g_skip;
}

} // namespace core::settings::intro_movie
