#include "core/options/window.h"

#include <algorithm>
#include <cstdio>

namespace core::options {
namespace {

constexpr int kTile = 16;
constexpr int kVkEscape = 0x1b, kVkReturn = 0x0d;
constexpr int kBottomW[] = {160, 96, 80, 96}; // Restore defaults, Cancel, OK, Apply
constexpr int kBottomGap = 8, kHotkeysW = 112;
constexpr int kChoiceW = 200, kArrow = 24;
constexpr int kTipW = 230, kTipLine = 14;
// The tooltip beside the cursor, as tooltip_draw 0x50f800 places the game's: 20 to the right and 40 below the cursor
// (the cursor's picture is about 32 high); left of it, or above it, when it would leave the window.
constexpr int kTipDx = 20, kTipDy = 40, kTipAbove = 8;
constexpr int kLabelSlack = 16; // a name is hit this much past its end (its "*")
constexpr int kTipRedLine = 16, kTipRedW = 320; // the restart line's height (the red font is the normal size)

bool In(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

int BottomX(int k) // the right-aligned bottom buttons
{
    int x = kWidth - 16 - (kBottomW[0] + kBottomW[1] + kBottomW[2] + kBottomW[3] + 3 * kBottomGap);
    for (int i = 0; i < k; ++i) {
        x += kBottomW[i] + kBottomGap;
    }
    return x;
}

} // namespace

void Window::SetPages(std::vector<Page> pages)
{
    pages_ = std::move(pages);
    drafts_.assign(pages_.size(), {});
    for (size_t p = 0; p < pages_.size(); ++p) {
        drafts_[p].resize(pages_[p].categories.size());
        for (size_t c = 0; c < pages_[p].categories.size(); ++c) {
            drafts_[p][c].assign(pages_[p].categories[c].rows.size(), DraftValue{});
        }
    }
    page_ = category_ = first_ = 0;
}

void Window::Open(int page, int category)
{
    Cancel();
    page_ = std::clamp(page, 0, std::max(0, static_cast<int>(pages_.size()) - 1));
    category_ = pages_.empty() ? 0
                               : std::clamp(category, 0,
                                            std::max(0, static_cast<int>(pages_[page_].categories.size()) - 1));
    first_ = 0;
    hover_ = pressed_ = Target{};
    dragRow_ = -1;
    dragThumb_ = false;
    tipShown_ = false;
}

// ---------------------------------------------------------------------------------------------------------------
// Drafts

void Window::Cancel()
{
    for (size_t p = 0; p < pages_.size(); ++p) {
        for (size_t c = 0; c < pages_[p].categories.size(); ++c) {
            const auto &rows = pages_[p].categories[c].rows;
            for (size_t r = 0; r < rows.size(); ++r) {
                DraftValue &d = drafts_[p][c][r];
                d.value = rows[r].value.get ? rows[r].value.get(rows[r].value.ctx) : 0;
                d.toggle = rows[r].toggle.get ? rows[r].toggle.get(rows[r].toggle.ctx) : 0;
            }
        }
    }
}

bool Window::Dirty() const
{
    for (size_t p = 0; p < pages_.size(); ++p) {
        for (size_t c = 0; c < pages_[p].categories.size(); ++c) {
            const auto &rows = pages_[p].categories[c].rows;
            for (size_t r = 0; r < rows.size(); ++r) {
                const DraftValue &d = drafts_[p][c][r];
                if (rows[r].value.get && d.value != rows[r].value.get(rows[r].value.ctx)) {
                    return true;
                }
                if (rows[r].toggle.get && d.toggle != rows[r].toggle.get(rows[r].toggle.ctx)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void Window::Apply()
{
    for (size_t p = 0; p < pages_.size(); ++p) {
        for (size_t c = 0; c < pages_[p].categories.size(); ++c) {
            const auto &rows = pages_[p].categories[c].rows;
            for (size_t r = 0; r < rows.size(); ++r) {
                const Row &row = rows[r];
                const DraftValue &d = drafts_[p][c][r];
                if (row.toggle.get && row.toggle.set && d.toggle != row.toggle.get(row.toggle.ctx)) {
                    row.toggle.set(row.toggle.ctx, d.toggle);
                }
                if (row.value.get && row.value.set && d.value != row.value.get(row.value.ctx)) {
                    row.value.set(row.value.ctx, d.value);
                }
            }
        }
    }
    Cancel(); // drafts = what is in effect now (a value the owner adjusted shows as it is)
}

void Window::RestoreDefaults()
{
    if (pages_.empty()) {
        return;
    }
    const auto &rows = Current().rows;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (rows[r].value.get) {
            drafts_[page_][category_][r].value = rows[r].defaultValue;
        }
        if (rows[r].toggle.get) {
            drafts_[page_][category_][r].toggle = rows[r].defaultToggle;
        }
    }
}

int Window::Draft(int page, int category, int row) const
{
    return drafts_[page][category][row].value;
}

int Window::DraftToggle(int page, int category, int row) const
{
    return drafts_[page][category][row].toggle;
}

// ---------------------------------------------------------------------------------------------------------------
// Layout

bool Window::HasList() const
{
    const auto &cats = pages_[page_].categories;
    return cats.size() > 1 || (cats.size() == 1 && !cats[0].name.empty());
}

bool Window::Shown(const Row &r) const
{
    if (r.shownIfId.empty()) {
        return true;
    }
    const auto &cats = pages_[page_].categories;
    for (size_t c = 0; c < cats.size(); ++c) {
        for (size_t k = 0; k < cats[c].rows.size(); ++k) {
            if (cats[c].rows[k].id == r.shownIfId) {
                return drafts_[page_][c][k].value == r.shownIfValue;
            }
        }
    }
    return true;
}

int Window::Height(const Row &r)
{
    switch (r.kind) {
    case Kind::Header: return 24;
    case Kind::Check: return 28;
    case Kind::Choice: return 32;
    case Kind::Slider:
    case Kind::CheckSlider: return 54;
    }
    return 28;
}

int Window::ContentX() const
{
    return HasList() ? kListX : 16;
}

int Window::ContentW() const
{
    return kWidth - 16 - ContentX();
}

std::vector<int> Window::VisibleRows()
{
    std::vector<int> out;
    const auto &rows = Current().rows;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (Shown(rows[r])) {
            out.push_back(static_cast<int>(r));
        }
    }
    return out;
}

bool Window::Scrolls()
{
    int h = 0;
    for (int r : VisibleRows()) {
        h += Height(Current().rows[r]);
    }
    return h > RowsHeight();
}

int Window::MaxFirstRow()
{
    std::vector<int> v = VisibleRows();
    int h = 0, f = static_cast<int>(v.size()), room = RowsHeight();
    while (f > 0 && h + Height(Current().rows[v[f - 1]]) <= room) {
        h += Height(Current().rows[v[--f]]);
    }
    return f;
}

int Window::Left() const
{
    return ContentX() + 16;
}

int Window::Right()
{
    return ContentX() + ContentW() - (Scrolls() ? 52 : 16);
}

std::vector<Window::Placed> Window::Layout()
{
    first_ = std::clamp(first_, 0, MaxFirstRow());
    std::vector<int> v = VisibleRows();
    std::vector<Placed> out;
    int y = kContentY + 16, bottom = kContentY + 16 + RowsHeight();
    for (size_t k = first_; k < v.size(); ++k) {
        int h = Height(Current().rows[v[k]]);
        if (y + h > bottom + 6) { // the last row's spacing may run into the margin
            break;
        }
        out.push_back({v[k], y});
        y += h;
    }
    return out;
}

void Window::SliderGeometry(int rowY, int &arrowL, int &trackX, int &trackW, int &arrowR, int &y)
{
    int x = Left() + 48, w = Right() - Left() - 48;
    arrowL = x;
    trackX = x + kArrow + 2;
    trackW = (w - 2 * kArrow - 4) / kTile * kTile;
    arrowR = trackX + trackW + 2;
    y = rowY + 22;
}

int Window::SliderLow(const Row &r) const
{
    return r.hasAuto ? r.min - r.step : r.min;
}

int Window::SliderMax(const Row &r) const
{
    int hi = r.max;
    if (r.maxNow) {
        hi = std::clamp(r.maxNow(), r.min, r.max);
    }
    return hi;
}

void Window::SetSliderFromX(int row, int x)
{
    const Row &r = Current().rows[row];
    int al, tx, tw, ar, y;
    SliderGeometry(0, al, tx, tw, ar, y);
    int lo = SliderLow(r), hi = SliderMax(r);
    int span = std::max(1, tw - 4 - kKnobSize);
    double f = std::clamp((x - (tx + 2 + kKnobSize / 2)) / static_cast<double>(span), 0.0, 1.0);
    int steps = (hi - lo) / std::max(1, r.step);
    int v = lo + static_cast<int>(f * steps + 0.5) * r.step;
    DraftOf(row).value = std::clamp(v, lo, hi);
}

void Window::Step(int row, int dir)
{
    const Row &r = Current().rows[row];
    DraftValue &d = DraftOf(row);
    if (r.kind == Kind::Choice) {
        int n = static_cast<int>(r.choices.size());
        if (n > 0) {
            d.value = ((d.value + dir) % n + n) % n;
        }
        return;
    }
    int lo = SliderLow(r), hi = SliderMax(r);
    int v = d.value + dir * r.step;
    if (r.hasAuto && d.value < r.min && dir > 0) {
        v = r.min;
    }
    d.value = std::clamp(v, lo, hi);
}

std::string Window::ValueText(const Row &r, int v) const
{
    if (r.hasAuto && v < r.min) {
        return r.autoLabel;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", v);
    return std::string(buf) + r.unit;
}

// ---------------------------------------------------------------------------------------------------------------
// Input

Window::Target Window::HitTest(int x, int y)
{
    if (pages_.empty()) {
        return {};
    }
    // Tabs: the widths depend on the text, so they are measured when drawn (tabX_).
    for (size_t k = 0; k < tabs_.size(); ++k) {
        if (In(x, y, tabs_[k].x, kTabY, tabs_[k].w, 25)) {
            return {Hit::Tab, static_cast<int>(k)};
        }
    }
    if (HasList()) {
        for (size_t k = 0; k < pages_[page_].categories.size(); ++k) {
            if (In(x, y, kCategoryX + 8, kContentY + 12 + static_cast<int>(k) * 30, 160, 25)) {
                return {Hit::CategoryButton, static_cast<int>(k)};
            }
        }
    }
    if (In(x, y, 16, kButtonY, kHotkeysW, 25)) {
        return {Hit::Hotkeys, 0};
    }
    const Hit bottom[] = {Hit::Defaults, Hit::CancelButton, Hit::Ok, Hit::ApplyButton};
    for (int k = 0; k < 4; ++k) {
        if (In(x, y, BottomX(k), kButtonY, kBottomW[k], 25)) {
            return {bottom[k], 0};
        }
    }
    if (Scrolls()) {
        int sx = ContentX() + ContentW() - 44, sy = kContentY + 8, sh = kContentH - 16;
        if (In(x, y, sx + 4, sy + 4, kArrow, kArrow)) {
            return {Hit::ScrollUp, 0};
        }
        if (In(x, y, sx + 4, sy + sh - 28, kArrow, kArrow)) {
            return {Hit::ScrollDown, 0};
        }
        if (In(x, y, sx, sy + 30, 32, sh - 60)) {
            return {Hit::ScrollTrack, 0};
        }
    }
    // Names and values are hit over the whole height of their row (a tooltip stays while the cursor wanders a little
    // above or below the text), and a little past the name's end (the "*" of a restart row and a margin).
    int left = Left(), right = Right();
    for (const Placed &pl : Layout()) {
        const Row &r = Current().rows[pl.row];
        int tw = (labelWidths_.count(pl.row) ? labelWidths_[pl.row] : 0) + kLabelSlack;
        switch (r.kind) {
        case Kind::Header: break;
        case Kind::Check:
            if (In(x, y, left, pl.y - 5, 24 + tw, Height(r))) {
                return {Hit::RowCheck, pl.row};
            }
            break;
        case Kind::Choice:
            if (In(x, y, right - kChoiceW, pl.y - 2, kArrow, kArrow)) {
                return {Hit::ArrowLess, pl.row};
            }
            if (In(x, y, right - kArrow, pl.y - 2, kArrow, kArrow)) {
                return {Hit::ArrowMore, pl.row};
            }
            if (In(x, y, right - kChoiceW + kArrow, pl.y - 6, kChoiceW - 2 * kArrow, Height(r))) {
                return {Hit::ChoiceValue, pl.row};
            }
            if (In(x, y, left, pl.y - 6, tw, Height(r))) {
                return {Hit::RowLabel, pl.row};
            }
            break;
        case Kind::Slider:
        case Kind::CheckSlider: { // the name's line: from 5 above the text to the slider's line
            if (r.kind == Kind::CheckSlider && In(x, y, left, pl.y - 5, 24 + tw, 24)) {
                return {Hit::RowCheck, pl.row};
            }
            if (r.kind == Kind::Slider && In(x, y, left, pl.y - 5, tw, 24)) {
                return {Hit::RowLabel, pl.row};
            }
            int al, tx, tw2, ar, sy;
            SliderGeometry(pl.y, al, tx, tw2, ar, sy);
            if (In(x, y, al, sy - 2, kArrow, kArrow)) {
                return {Hit::ArrowLess, pl.row};
            }
            if (In(x, y, ar, sy - 2, kArrow, kArrow)) {
                return {Hit::ArrowMore, pl.row};
            }
            if (In(x, y, tx, sy - 2, tw2, 24)) {
                return {Hit::Track, pl.row};
            }
            break;
        }
        }
    }
    return {};
}

void Window::ScrollTo(int first)
{
    first_ = std::clamp(first, 0, MaxFirstRow());
}

void Window::MouseMove(int x, int y)
{
    mouseX_ = x;
    mouseY_ = y;
    if (dragRow_ >= 0) {
        SetSliderFromX(dragRow_, x);
        return;
    }
    if (dragThumb_) {
        int sy = kContentY + 8, sh = kContentH - 16, span = sh - 60 - 26;
        int maxFirst = MaxFirstRow();
        if (maxFirst > 0 && span > 0) {
            double f = std::clamp((y - (sy + 30 + 13)) / static_cast<double>(span), 0.0, 1.0);
            ScrollTo(static_cast<int>(f * maxFirst + 0.5));
        }
        return;
    }
    Target t = HitTest(x, y);
    if (!(t == hover_)) {
        hover_ = t;
        hoverSince_ = now_;
        tipShown_ = false;
    }
}

Action Window::MouseDown(int x, int y, unsigned nowMs)
{
    now_ = nowMs;
    MouseMove(x, y);
    pressed_ = HitTest(x, y);
    pressedAt_ = lastRepeat_ = nowMs;
    tipShown_ = false;
    switch (pressed_.hit) {
    case Hit::ArrowLess: Step(pressed_.index, -1); break;
    case Hit::ArrowMore: Step(pressed_.index, +1); break;
    case Hit::ScrollUp: ScrollTo(first_ - 1); break;
    case Hit::ScrollDown: ScrollTo(first_ + 1); break;
    case Hit::Track:
        dragRow_ = pressed_.index;
        SetSliderFromX(dragRow_, x);
        break;
    case Hit::ScrollTrack: {
        int sy = kContentY + 8, sh = kContentH - 16, span = sh - 60 - 26, maxFirst = MaxFirstRow();
        int thumbY = sy + 30 + (maxFirst > 0 ? first_ * span / maxFirst : 0);
        if (y >= thumbY && y < thumbY + 26) {
            dragThumb_ = true;
        } else {
            ScrollTo(first_ + (y < thumbY ? -4 : 4));
        }
        break;
    }
    default: break;
    }
    return Action::None;
}

Action Window::Activate(const Target &t, int)
{
    switch (t.hit) {
    case Hit::Tab:
        if (t.index != page_) {
            page_ = t.index;
            category_ = first_ = 0;
        }
        break;
    case Hit::CategoryButton:
        if (t.index != category_) {
            category_ = t.index;
            first_ = 0;
        }
        break;
    case Hit::RowCheck: {
        DraftValue &d = DraftOf(t.index);
        if (Current().rows[t.index].kind == Kind::CheckSlider) {
            d.toggle = !d.toggle;
        } else {
            d.value = !d.value;
        }
        break;
    }
    case Hit::Hotkeys: return Action::OpenHotkeys;
    case Hit::Defaults: RestoreDefaults(); break;
    case Hit::CancelButton:
        Cancel();
        return Action::Close;
    case Hit::Ok:
        Apply();
        return Action::Close;
    case Hit::ApplyButton:
        if (Dirty()) {
            Apply();
        }
        break;
    default: break;
    }
    return Action::None;
}

Action Window::MouseUp(int x, int y)
{
    Target was = pressed_;
    pressed_ = Target{};
    dragRow_ = -1;
    dragThumb_ = false;
    Target now = HitTest(x, y);
    MouseMove(x, y);
    if (was.hit == Hit::None || !(now == was)) {
        return Action::None;
    }
    return Activate(was, x);
}

void Window::Wheel(int notches)
{
    if (!pages_.empty() && Scrolls()) {
        ScrollTo(first_ - notches);
    }
}

Action Window::Key(int vk)
{
    if (vk == kVkEscape) {
        Cancel();
        return Action::Close;
    }
    if (vk == kVkReturn) {
        Apply();
        return Action::Close;
    }
    return Action::None;
}

void Window::Tick(unsigned nowMs)
{
    now_ = nowMs;
    bool repeats = pressed_.hit == Hit::ArrowLess || pressed_.hit == Hit::ArrowMore ||
                   pressed_.hit == Hit::ScrollUp || pressed_.hit == Hit::ScrollDown;
    if (repeats && nowMs - pressedAt_ >= kRepeatDelayMs && nowMs - lastRepeat_ >= kRepeatMs &&
        HitTest(mouseX_, mouseY_) == pressed_) {
        lastRepeat_ = nowMs;
        switch (pressed_.hit) {
        case Hit::ArrowLess: Step(pressed_.index, -1); break;
        case Hit::ArrowMore: Step(pressed_.index, +1); break;
        case Hit::ScrollUp: ScrollTo(first_ - 1); break;
        default: ScrollTo(first_ + 1); break;
        }
    }
    // The tooltip: the row's description on its name (or check), the value's on a choice's value (as it is now).
    if (!(hover_ == tip_)) {
        tipShown_ = false;
    }
    if (pressed_.hit == Hit::None && !tipShown_ && nowMs - hoverSince_ >= kTipDelayMs) {
        tip_ = hover_;
        tipShown_ = !TipText().empty();
    }
}

std::string Window::TipText()
{
    if (pages_.empty() || tip_.index < 0 || tip_.index >= static_cast<int>(Current().rows.size())) {
        return "";
    }
    const Row &r = Current().rows[tip_.index];
    switch (tip_.hit) {
    case Hit::RowLabel:
    case Hit::RowCheck: return r.help;
    case Hit::ChoiceValue: { // not on the arrows: they only change the value
        if (r.kind != Kind::Choice) {
            return "";
        }
        int v = DraftOf(tip_.index).value;
        return v >= 0 && v < static_cast<int>(r.choiceHelp.size()) ? r.choiceHelp[v] : "";
    }
    default: return "";
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing

void Window::TextButton(Painter &p, const char *label, int x, int y, int w, int style, Font f, bool hover)
{
    int bw = std::max(2, w / kTile) * kTile;
    p.ButtonBar(x, y, bw, style);
    p.Text(label, x + (bw - p.TextWidth(label, f)) / 2, y + 5, f);
    if (hover) {
        p.Frame(x, y, bw, 25);
    }
}

void Window::Tabs(Painter &p)
{
    tabs_.clear();
    int x = 24;
    for (size_t k = 0; k < pages_.size(); ++k) {
        const char *name = pages_[k].name.c_str();
        int bw = (p.TextWidth(name, Font::Normal) + 40) / kTile * kTile + kTile;
        bool sel = static_cast<int>(k) == page_;
        TextButton(p, name, x, kTabY, bw, 0, sel ? Font::Gold : Font::Normal,
                   hover_ == Target{Hit::Tab, static_cast<int>(k)} && !sel);
        tabs_.push_back({x, bw});
        x += bw + 6;
    }
}

void Window::Bottom(Painter &p)
{
    TextButton(p, "Hotkeys", 16, kButtonY, kHotkeysW, 0, Font::Normal, hover_.hit == Hit::Hotkeys);
    const char *labels[] = {"Restore defaults", "Cancel", "OK", "Apply"};
    const Hit hits[] = {Hit::Defaults, Hit::CancelButton, Hit::Ok, Hit::ApplyButton};
    bool dirty = Dirty();
    for (int k = 0; k < 4; ++k) {
        bool disabled = k == 3 && !dirty;
        TextButton(p, labels[k], BottomX(k), kButtonY, kBottomW[k], 0, disabled ? Font::Disabled : Font::Normal,
                   hover_.hit == hits[k] && !disabled);
    }
}

void Window::Rows(Painter &p)
{
    int left = Left(), right = Right();
    labelWidths_.clear();
    for (const Placed &pl : Layout()) {
        const Row &r = Current().rows[pl.row];
        const DraftValue &d = DraftOf(pl.row);
        const char *label = r.label.c_str();
        int tw = p.TextWidth(label, r.kind == Kind::Header ? Font::Gold : Font::Normal);
        labelWidths_[pl.row] = tw;
        int y = pl.y;
        if (r.restart) { // the red "*" after the label (where each kind draws its label)
            bool checkFirst = r.kind == Kind::Check || r.kind == Kind::CheckSlider;
            int ly = r.kind == Kind::Choice ? y + 3 : checkFirst ? y + 1 : y;
            p.Text("*", left + (checkFirst ? 24 : 0) + tw + 3, ly, Font::Disabled);
        }
        switch (r.kind) {
        case Kind::Header: {
            int mid = (left + right) / 2, a = mid - tw / 2 - 10, b = mid + tw / 2 + 10;
            p.Line(left, y + 7, a - left);
            p.Text(label, mid - tw / 2, y, Font::Gold);
            p.Line(b, y + 7, right - b);
            break;
        }
        case Kind::Check:
            p.Check(left, y, d.value != 0);
            p.Text(label, left + 24, y + 1, Font::Normal);
            break;
        case Kind::Choice: {
            p.Text(label, left, y + 3, Font::Normal);
            int x = right - kChoiceW, sw = (kChoiceW - 2 * kArrow - 4) / kTile * kTile;
            auto state = [&](Hit h) {
                return pressed_ == Target{h, pl.row}  ? ArrowState::Pressed
                       : hover_ == Target{h, pl.row} ? ArrowState::Hover
                                                      : ArrowState::Normal;
            };
            p.ArrowButton(x, y - 2, Arrow::Left, state(Hit::ArrowLess));
            p.Strip(x + kArrow + 2, y, sw);
            std::string v = d.value >= 0 && d.value < static_cast<int>(r.choices.size()) ? r.choices[d.value] : "?";
            p.Text(v.c_str(), x + kArrow + 2 + (sw - p.TextWidth(v.c_str(), Font::Gold)) / 2, y + 3, Font::Gold);
            p.ArrowButton(right - kArrow, y - 2, Arrow::Right, state(Hit::ArrowMore));
            break;
        }
        case Kind::Slider:
        case Kind::CheckSlider: {
            if (r.kind == Kind::CheckSlider) {
                p.Check(left, y, d.toggle != 0);
                p.Text(label, left + 24, y + 1, Font::Normal);
            } else {
                p.Text(label, left, y, Font::Normal);
            }
            std::string v = ValueText(r, d.value);
            p.Text(v.c_str(), left, y + 25, Font::Gold);
            int al, tx, tw2, ar, sy;
            SliderGeometry(y, al, tx, tw2, ar, sy);
            auto state = [&](Hit h) {
                return pressed_ == Target{h, pl.row}  ? ArrowState::Pressed
                       : hover_ == Target{h, pl.row} ? ArrowState::Hover
                                                      : ArrowState::Normal;
            };
            p.ArrowButton(al, sy - 2, Arrow::Left, state(Hit::ArrowLess));
            p.Strip(tx, sy, tw2);
            int lo = SliderLow(r), hi = SliderMax(r);
            double f = hi > lo ? std::clamp((d.value - lo) / static_cast<double>(hi - lo), 0.0, 1.0) : 0.0;
            p.Knob(tx + 2 + static_cast<int>(f * (tw2 - 4 - kKnobSize)), sy + 2);
            p.ArrowButton(ar, sy - 2, Arrow::Right, state(Hit::ArrowMore));
            break;
        }
        }
    }
}

void Window::Scrollbar(Painter &p)
{
    if (!Scrolls()) {
        return;
    }
    int sx = ContentX() + ContentW() - 44, sy = kContentY + 8, sh = kContentH - 16;
    p.Inset(sx, sy, 32, sh);
    auto state = [&](Hit h) {
        return pressed_.hit == h ? ArrowState::Pressed : hover_.hit == h ? ArrowState::Hover : ArrowState::Normal;
    };
    p.ArrowButton(sx + 4, sy + 4, Arrow::Up, state(Hit::ScrollUp));
    p.ArrowButton(sx + 4, sy + sh - 28, Arrow::Down, state(Hit::ScrollDown));
    int span = sh - 60 - 26, maxFirst = MaxFirstRow();
    p.Thumb(sx + 3, sy + 30 + (maxFirst > 0 ? first_ * span / maxFirst : 0));
}

std::vector<std::string> Window::Wrap(Painter &p, const std::string &text, int maxW, Font f)
{
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find(' ', i);
        std::string word = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? text.size() : j + 1;
        if (word.empty()) {
            continue;
        }
        std::string t = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && p.TextWidth(t.c_str(), f) > maxW) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = t;
        }
    }
    if (!cur.empty()) {
        lines.push_back(cur);
    }
    return lines;
}

void Window::Tooltip(Painter &p)
{
    if (!tipShown_) {
        return;
    }
    std::string text = TipText();
    if (text.empty()) {
        return;
    }
    // A restart row: the red line first, then the description.
    std::vector<std::string> red;
    if (Current().rows[tip_.index].restart && !restartTip_.empty()) {
        red = Wrap(p, restartTip_, kTipRedW, Font::Disabled); // one line where it fits (the larger font)
    }
    std::vector<std::string> lines = Wrap(p, text, kTipW, Font::Small);
    int w = 0;
    for (const auto &l : red) {
        w = std::max(w, p.TextWidth(l.c_str(), Font::Disabled));
    }
    for (const auto &l : lines) {
        w = std::max(w, p.TextWidth(l.c_str(), Font::Small));
    }
    w += 12;
    int redH = static_cast<int>(red.size()) * kTipRedLine + (red.empty() ? 0 : 2);
    int h = redH + static_cast<int>(lines.size()) * kTipLine + 8;
    int x = mouseX_ + kTipDx, y = mouseY_ + kTipDy;
    if (x + w > kWidth - 4) {
        x = mouseX_ - kTipDx - w;
    }
    if (y + h > kHeight - 4) {
        y = mouseY_ - kTipAbove - h;
    }
    x = std::clamp(x, 4, std::max(4, kWidth - 4 - w));
    y = std::clamp(y, 4, std::max(4, kHeight - 4 - h));
    p.TipBox(x, y, w, h);
    for (size_t k = 0; k < red.size(); ++k) {
        p.Text(red[k].c_str(), x + 6, y + 5 + static_cast<int>(k) * kTipRedLine, Font::Disabled);
    }
    for (size_t k = 0; k < lines.size(); ++k) {
        p.Text(lines[k].c_str(), x + 6, y + 5 + redH + static_cast<int>(k) * kTipLine, Font::Small);
    }
}

void Window::Draw(Painter &p)
{
    if (pages_.empty()) {
        return;
    }
    p.Panel(0, 0, kWidth, kHeight);
    const char *title = "Yongle options";
    p.Text(title, (kWidth - p.TextWidth(title, Font::Title)) / 2, 12, Font::Title);
    Tabs(p);
    if (HasList()) {
        p.Inset(kCategoryX, kContentY, kCategoryW, kContentH);
        const auto &cats = pages_[page_].categories;
        for (size_t k = 0; k < cats.size(); ++k) {
            bool sel = static_cast<int>(k) == category_;
            TextButton(p, cats[k].name.c_str(), kCategoryX + 8, kContentY + 12 + static_cast<int>(k) * 30, 160,
                       sel ? 1 : 0, sel ? Font::Gold : Font::Normal,
                       hover_ == Target{Hit::CategoryButton, static_cast<int>(k)} && !sel);
        }
    }
    p.Inset(ContentX(), kContentY, ContentW(), kContentH);
    Rows(p);
    Scrollbar(p);
    Bottom(p);
    Tooltip(p);
}

} // namespace core::options
