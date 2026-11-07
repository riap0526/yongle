#include "core/display/city_strips.h"

namespace core::display {

bool PlanStrips(const StripInput &in, StripPlan *out)
{
    *out = StripPlan{};
    const int mapCols = in.right - in.left;
    const int clampRows = in.bottom - in.top;
    if (mapCols < 2 || clampRows < 4 || in.mapRows < 4 || in.colPx <= 2 || in.halfH <= 0) {
        return false;
    }
    const int overlap = in.overlapRows < 2 ? 2 : (in.overlapRows + 1) & ~1;
    int keep = in.keepRows < 2 ? 2 : in.keepRows & ~1;
    // the view: at most what the clamp holds, and with the drawing's two half rows within the map's range
    int maxView = clampRows < in.mapRows - 2 ? clampRows : in.mapRows - 2;
    maxView &= ~1;
    if (maxView < 4) {
        return false;
    }
    const int shownRows = clampRows - 1; // half rows of picture: the view's top limit to the clamp's bottom
    if (keep + overlap > maxView) {
        keep = (maxView - overlap) & ~1;
    }
    if (keep < 2) {
        return false; // the overlap leaves nothing to keep
    }
    if (keep >= shownRows) { // one strip shows it all
        keep = shownRows;
    }
    out->cols = mapCols - 1;
    out->viewRows = keep + overlap < maxView ? keep + overlap : maxView;
    out->keepRows = keep;
    out->topPx = (in.topRows < 0 ? 0 : in.topRows & ~1) * in.halfH;
    out->imageW = mapCols * in.colPx - 2;
    out->imageH = out->topPx + shownRows * in.halfH;
    out->count = (shownRows + keep - 1) / keep;
    out->clipRight = (out->cols + 1) * in.colPx - 2;
    out->clipBottom = out->topPx + (out->viewRows + 1) * in.halfH;
    out->bufW = out->clipRight + 2;
    out->bufH = out->clipBottom + 4 * in.halfH; // spare rows below the clip, as the back buffer has
    return true;
}

Strip StripAt(const StripInput &in, const StripPlan &plan, int k)
{
    const int shownRows = in.bottom - in.top - 1;
    const int first = k * plan.keepRows; // the strip's first half row in the picture
    int rows = shownRows - first < plan.keepRows ? shownRows - first : plan.keepRows;
    int viewY = in.top + first;
    if (viewY + plan.viewRows > in.bottom) { // the last strip: the view as low as the clamp lets it go
        viewY = in.bottom - plan.viewRows;
    }
    viewY &= ~1;
    if (viewY < in.top) {
        viewY = in.top;
    }
    Strip s;
    s.viewY = viewY;
    s.fromY = plan.topPx + (in.top + first - viewY) * in.halfH;
    s.toY = plan.topPx + first * in.halfH;
    s.rows = rows * in.halfH;
    if (k == 0) { // with the room above the map's top row
        s.fromY -= plan.topPx;
        s.toY -= plan.topPx;
        s.rows += plan.topPx;
    }
    return s;
}

} // namespace core::display
