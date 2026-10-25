// Common screen sizes for the panel's resolution choice (R2b.1-9, docs/archive/resolution_ui_r2b1.md): 16:9, 16:10, 21:9,
// 4:3, 5:4 and 3:2 sizes up to 4K, of which the panel lists those that fit the screen (not larger than its native
// size, not smaller than the game's minimum), by default only those with the screen's aspect. Nothing here knows a
// game: the minimum is the caller's.
#pragma once

namespace core::display {

struct CommonSize {
    int w, h;
    const char *aspect; // "16:9", "16:10", "21:9", "4:3", "5:4", "3:2", or nullptr for the screen's own size
    bool native;        // the screen's native size
    bool heavy;         // 3200x1800 or more: the game draws far more each frame
};

// Fills out (at most max entries) with the sizes that fit, smallest first (by width, then height); the native size
// is always among them when it fits the minimum. allAspects: every aspect, else only the native one's (within 2%).
// Returns the count.
int CommonSizes(int nativeW, int nativeH, int minW, int minH, bool allAspects, CommonSize *out, int max);

// "16:9" and so on for any size (the nearest of the six within 2%), nullptr when none is near.
const char *AspectName(int w, int h);

} // namespace core::display
