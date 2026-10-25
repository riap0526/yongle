// The logical resolution a game draws, from the screen's native resolution and the settings
// (docs/resolution_ui/display_scale.md §6.1, Augustus's display scale model):
//   - fixed WxH: that size (the output scales it to the screen, black bars where the aspect differs);
//   - scaled: native / scale (100..250 %, steps of 5), used as given. scale = auto (R4): a screen taller than 1080 px
//     is drawn exactly 1080 high at its own aspect (2560x1440 -> 1920x1080, 4K -> 1920x1080), at any ratio, not only
//     the 5 % steps (scale_percent is then the ratio rounded); a smaller screen at its own size.
// Either way the result is kept at least as large as the game's minimum (a scale is lowered to the largest step
// that still fits it; a native screen smaller than the minimum gets the minimum). No game knowledge here: the
// minimum is the caller's.
#pragma once

#include "core/config.h"

namespace core::display {

struct LogicalSize {
    int w = 0, h = 0;
    int scale_percent = 100; // the scale in effect (100 for a fixed size; auto: native height / 1080, rounded)
    bool limited = false;    // the requested size or scale had to be changed to fit the minimum
};

LogicalSize ChooseLogicalSize(int native_w, int native_h, core_resolution_kind kind, int fixed_w, int fixed_h,
                              int scale_percent, int min_w, int min_h);

// The game's minimum logical size, set by the game layer (for previews, e.g. in our panel). 0 x 0 until set.
void SetGameMinimum(int min_w, int min_h);
void GameMinimum(int *min_w, int *min_h);

// Describes a setting for logs and the hotkey ("1024x768", "scaled auto", "scaled 150%").
void DescribeSetting(core_resolution_kind kind, int fixed_w, int fixed_h, int scale_percent, char *out, int size);

} // namespace core::display
