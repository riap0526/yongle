// Offline test of core/options (the options window; no DLL, no game): drafts and OK / Cancel / Apply / Restore
// defaults, checkboxes, choices, sliders (arrows, held repeat, the track, dragging, "automatic", the limit now),
// rows shown only for another row's value, categories and tabs, scrolling (wheel, arrows, thumb), the tooltip after
// a pause, keys, and that everything drawn stays inside the window. Drawing goes to a fake painter that records it.
#include "core/options/window.h"
#include "core/ui/hotkey.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace co = core::options;

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("%-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

struct Op {
    std::string op, text;
    int x, y, w, h, extra;
    co::Font font;
};

class FakePainter : public co::Painter {
public:
    std::vector<Op> ops;
    void Add(const char *op, int x, int y, int w, int h, int extra = 0, const char *text = "",
             co::Font f = co::Font::Normal)
    {
        ops.push_back({op, text, x, y, w, h, extra, f});
    }
    void Panel(int x, int y, int w, int h) override { Add("panel", x, y, w, h); }
    void Inset(int x, int y, int w, int h) override { Add("inset", x, y, w, h); }
    void ButtonBar(int x, int y, int w, int style) override { Add("bar", x, y, w, 25, style); }
    void Frame(int x, int y, int w, int h) override { Add("frame", x, y, w, h); }
    void Strip(int x, int y, int w) override { Add("strip", x, y, w, 20); }
    void Check(int x, int y, bool on) override { Add("check", x, y, 16, 16, on); }
    void ArrowButton(int x, int y, co::Arrow a, co::ArrowState s) override
    {
        Add("arrow", x, y, 24, 24, static_cast<int>(a) * 10 + static_cast<int>(s));
    }
    void Knob(int x, int y) override { Add("knob", x, y, co::kKnobSize, co::kKnobSize); }
    void Thumb(int x, int y) override { Add("thumb", x, y, 26, 26); }
    void Line(int x, int y, int w) override { Add("line", x, y, w, 3); }
    void Text(const char *t, int x, int y, co::Font f) override { Add("text", x, y, TextWidth(t, f), 14, 0, t, f); }
    int TextWidth(const char *t, co::Font) override { return 7 * static_cast<int>(std::strlen(t)); }
    void TipBox(int x, int y, int w, int h) override { Add("tip", x, y, w, h); }

    const Op *FindText(const char *t) const
    {
        for (const Op &o : ops) {
            if (o.op == "text" && o.text == t) return &o;
        }
        return nullptr;
    }
    const Op *Find(const char *op, int n = 0) const
    {
        for (const Op &o : ops) {
            if (o.op == op && n-- == 0) return &o;
        }
        return nullptr;
    }
    int Count(const char *op) const
    {
        int n = 0;
        for (const Op &o : ops) n += o.op == op;
        return n;
    }
};

// Values in effect, set only through Apply.
int g_values[16];
int g_sets = 0;
int Get(const void *ctx) { return g_values[reinterpret_cast<intptr_t>(ctx)]; }
void Set(const void *ctx, int v)
{
    g_values[reinterpret_cast<intptr_t>(ctx)] = v;
    ++g_sets;
}
co::Value V(int slot) { return {Get, Set, reinterpret_cast<const void *>(static_cast<intptr_t>(slot))}; }
int g_maxNow = 400;
int MaxNow() { return g_maxNow; }

co::Row CheckRow(const char *label, int slot, int def, const char *help = "")
{
    co::Row r;
    r.kind = co::Kind::Check;
    r.label = label;
    r.help = help;
    r.value = V(slot);
    r.defaultValue = def;
    return r;
}

co::Row SliderRow(const char *label, int slot, int def, int min, int max, int step)
{
    co::Row r;
    r.kind = co::Kind::Slider;
    r.label = label;
    r.value = V(slot);
    r.defaultValue = def;
    r.min = min;
    r.max = max;
    r.step = step;
    r.unit = "%";
    r.help = "How fast it goes.";
    return r;
}

co::Row Header(const char *t)
{
    co::Row r;
    r.kind = co::Kind::Header;
    r.label = t;
    return r;
}

std::vector<co::Page> Pages()
{
    // General: two categories. Display: a long category (scrolls). Sound: no category list.
    co::Category speed{"Speed", {SliderRow("Game speed", 0, 80, 10, 200, 10), Header("Scrolling"),
                                 CheckRow("Smooth scrolling", 1, 1, "Moves the map smoothly.")}};
    co::Row choice;
    choice.kind = co::Kind::Choice;
    choice.id = "res";
    choice.label = "Resolution";
    choice.value = V(2);
    choice.choices = {"Scaled", "1024 x 768", "1920 x 1080"};
    choice.help = "Pick a size.";
    choice.choiceHelp = {"Help for scaled.", "Help for 1024.", ""};
    choice.restart = true;
    co::Row scale = SliderRow("Display scale", 3, 100, 50, 400, 1);
    scale.hasAuto = true;
    scale.maxNow = MaxNow;
    scale.shownIfId = "res";
    scale.shownIfValue = 0;
    co::Category general{"General", {CheckRow("Autosave", 4, 1)}};
    co::Category screen{"Screen", {choice, scale}};
    co::Category many{"Many", {}};
    for (int k = 0; k < 14; ++k) many.rows.push_back(CheckRow("Row", 5, 0));
    co::Row cs;
    cs.kind = co::Kind::CheckSlider;
    cs.label = "Music";
    cs.value = V(6);
    cs.toggle = V(7);
    cs.min = 0;
    cs.max = 100;
    cs.step = 5;
    cs.defaultValue = 80;
    cs.defaultToggle = 1;
    return {{"General", {general, speed}}, {"Display", {screen, many}}, {"Sound", {{"", {cs}}}}};
}

bool AllInside(const FakePainter &p)
{
    for (const Op &o : p.ops) {
        if (o.x < 0 || o.y < 0 || o.x + o.w > co::kWidth || o.y + o.h > co::kHeight) {
            std::printf("  outside: %s %s at %d,%d %dx%d\n", o.op.c_str(), o.text.c_str(), o.x, o.y, o.w, o.h);
            return false;
        }
    }
    return true;
}

FakePainter Draw(co::Window &w)
{
    FakePainter p;
    w.Draw(p);
    return p;
}

void Click(co::Window &w, int x, int y, unsigned now = 0)
{
    w.MouseDown(x, y, now);
    w.MouseUp(x, y);
}

int Cx(const Op *o) { return o->x + o->w / 2; }
int Cy(const Op *o) { return o->y + o->h / 2; }

} // namespace

int main()
{
    std::memset(g_values, 0, sizeof(g_values));
    g_values[0] = 80;
    g_values[1] = 1;
    g_values[2] = 1; // Resolution: 1024 x 768 (Display scale hidden)
    g_values[3] = 120;
    g_values[4] = 1;
    g_values[6] = 80;
    g_values[7] = 1;

    co::Window w;
    w.SetPages(Pages());
    w.SetRestartTip("Restart first.");
    w.Open(0, 1); // General, Speed
    FakePainter p = Draw(w);
    Check(p.Find("panel") && p.Find("panel")->w == 640 && p.Find("panel")->h == 480, "the window frame 640 x 480");
    Check(p.FindText("Yongle options") && p.FindText("Yongle options")->font == co::Font::Title, "title");
    Check(p.FindText("General") && p.FindText("General")->font == co::Font::Gold, "selected tab in gold");
    Check(p.FindText("Display") && p.FindText("Display")->font == co::Font::Normal, "other tabs in white");
    Check(p.FindText("Speed") && p.FindText("Speed")->font == co::Font::Gold, "selected category in gold");
    Check(p.FindText("Apply") && p.FindText("Apply")->font == co::Font::Disabled, "Apply red while nothing changed");
    Check(p.FindText("80%") != nullptr, "slider number with unit");
    Check(p.Count("line") == 2 && p.FindText("Scrolling")->font == co::Font::Gold, "section title between two lines");
    Check(p.Find("inset", 1) && p.Find("inset", 1)->x == co::Window::kListX, "content right of the category list");
    Check(AllInside(p), "everything inside the window (General)");

    // Checkbox: the label toggles too; Apply sets it, once.
    const Op *smooth = p.FindText("Smooth scrolling");
    Click(w, smooth->x + 5, smooth->y + 3);
    Check(w.Draft(0, 1, 2) == 0 && g_values[1] == 1, "label click changes the draft only");
    Check(w.Dirty(), "dirty after a change");
    p = Draw(w);
    Check(p.FindText("Apply")->font == co::Font::Normal, "Apply white when something changed");
    g_sets = 0;
    Click(w, Cx(p.FindText("Apply")), Cy(p.FindText("Apply")));
    Check(g_values[1] == 0 && g_sets == 1 && !w.Dirty(), "Apply sets the changed value once");

    // Slider arrows, held repeat, limits.
    p = Draw(w);
    const Op *less = nullptr, *more = nullptr;
    for (const Op &o : p.ops) {
        if (o.op == "arrow" && o.extra / 10 == static_cast<int>(co::Arrow::Left) && !less) less = &o;
        if (o.op == "arrow" && o.extra / 10 == static_cast<int>(co::Arrow::Right) && !more) more = &o;
    }
    int mx = Cx(more), my = Cy(more);
    w.MouseDown(mx, my, 1000);
    Check(w.Draft(0, 1, 0) == 90, "right arrow: + step");
    w.Tick(1300);
    Check(w.Draft(0, 1, 0) == 90, "no repeat before the delay");
    for (unsigned t = 1400; t <= 3000; t += 60) w.Tick(t);
    Check(w.Draft(0, 1, 0) == 200, "held arrow repeats up to the maximum");
    w.MouseUp(mx, my);
    Click(w, Cx(less), Cy(less));
    Check(w.Draft(0, 1, 0) == 190, "left arrow: - step");

    // Track: the left end and the right end; dragging.
    const Op *strip = nullptr;
    for (const Op &o : p.ops) {
        if (o.op == "strip") { strip = &o; break; }
    }
    Click(w, strip->x + 1, Cy(strip));
    Check(w.Draft(0, 1, 0) == 10, "track left end = minimum");
    w.MouseDown(strip->x + 1, Cy(strip), 0);
    w.MouseMove(strip->x + strip->w / 2, Cy(strip));
    int mid = w.Draft(0, 1, 0);
    w.MouseMove(strip->x + strip->w + 50, 0);
    w.MouseUp(strip->x + strip->w + 50, 0);
    Check(mid > 80 && mid < 130 && w.Draft(0, 1, 0) == 200, "drag: the middle, then past the end = maximum");
    w.Cancel();
    Check(w.Draft(0, 1, 0) == 80 && !w.Dirty(), "Cancel drops the drafts");

    // Tooltip: only after a pause on the label.
    p = Draw(w);
    const Op *game = p.FindText("Game speed");
    w.Tick(5000);
    w.MouseMove(game->x + 3, game->y + 3);
    w.Tick(5000);
    w.Tick(5000 + co::Window::kTipDelayMs / 2);
    Check(!w.TipShown(), "no tooltip before the pause");
    w.Tick(5000 + co::Window::kTipDelayMs);
    p = Draw(w);
    Check(w.TipShown() && p.Find("tip") && p.FindText("How fast it goes."), "tooltip after the pause");
    Check(p.Find("tip")->x == game->x + 3 + 20 && p.Find("tip")->y == game->y + 3 + 40 && AllInside(p),
          "tooltip beside the cursor (20 right, 40 down), inside the window");
    w.MouseMove(game->x + 9, game->y + 4);
    w.Tick(5000 + co::Window::kTipDelayMs + 16);
    p = Draw(w);
    Check(w.TipShown() && p.Find("tip")->x == game->x + 9 + 20, "tooltip follows the cursor on the same label");
    w.MouseMove(5, 5);
    w.Tick(6000);
    Check(!w.TipShown(), "tooltip goes when the mouse leaves");

    // Categories and tabs.
    p = Draw(w);
    const Op *cat = p.FindText("General");
    for (const Op &o : p.ops) { // the category button, not the tab
        if (o.op == "text" && o.text == "General" && o.y > co::Window::kContentY) cat = &o;
    }
    Click(w, Cx(cat), Cy(cat));
    Check(w.CategoryIndex() == 0, "category button selects the category");
    p = Draw(w);
    Click(w, Cx(p.FindText("Display")), Cy(p.FindText("Display")));
    Check(w.PageIndex() == 1 && w.CategoryIndex() == 0, "tab selects the page, its first category");

    // Choice cycles; a row shown only for a value; "automatic" below the minimum; the limit now.
    p = Draw(w);
    Check(p.FindText("Display scale") == nullptr, "row hidden while the other row has another value");
    const Op *right = nullptr;
    for (const Op &o : p.ops) {
        if (o.op == "arrow" && o.extra / 10 == static_cast<int>(co::Arrow::Right)) right = &o;
    }
    Click(w, Cx(right), Cy(right));
    Click(w, Cx(right), Cy(right));
    Check(w.Draft(1, 0, 0) == 0, "choice wraps around (1 -> 2 -> 0)");
    g_values[2] = 1;
    w.Open(1, 0);
    Check(w.Draft(1, 0, 0) == 1, "Open reads the values in effect");
    g_values[2] = 0;
    w.Open(1, 0);
    p = Draw(w);
    Check(p.FindText("Display scale") && p.FindText("120%"), "row shown for that value");
    const Op *sl = nullptr;
    for (const Op &o : p.ops) {
        if (o.op == "arrow" && o.extra / 10 == static_cast<int>(co::Arrow::Left) && o.y > 140) sl = &o;
    }
    for (int k = 0; k < 80; ++k) Click(w, Cx(sl), Cy(sl));
    p = Draw(w);
    Check(w.Draft(1, 0, 1) == 49 && p.FindText("Auto"), "one step below the minimum = automatic");
    const Op *sr = nullptr;
    for (const Op &o : p.ops) {
        if (o.op == "arrow" && o.extra / 10 == static_cast<int>(co::Arrow::Right) && o.y > 140) sr = &o;
    }
    Click(w, Cx(sr), Cy(sr));
    Check(w.Draft(1, 0, 1) == 50, "from automatic up = the minimum");
    g_maxNow = 150;
    for (const Op &o : p.ops) {
        if (o.op == "strip" && o.y > 140) strip = &o;
    }
    Click(w, strip->x + strip->w - 1, Cy(strip));
    Check(w.Draft(1, 0, 1) == 150, "track right end = the limit now");
    g_maxNow = 400;
    w.Cancel();

    // Scrolling: a long category.
    p = Draw(w);
    cat = nullptr;
    for (const Op &o : p.ops) {
        if (o.op == "text" && o.text == "Many") cat = &o;
    }
    Click(w, Cx(cat), Cy(cat));
    p = Draw(w);
    Check(p.Find("thumb") && w.MaxFirstRow() > 0, "a long category scrolls");
    Check(AllInside(p), "everything inside the window (scrolling)");
    int maxFirst = w.MaxFirstRow();
    w.Wheel(-1);
    Check(w.FirstRow() == 1, "wheel down one row");
    w.Wheel(-100);
    Check(w.FirstRow() == maxFirst, "no further than the last row");
    w.Wheel(100);
    p = Draw(w);
    const Op *thumb = p.Find("thumb");
    w.MouseDown(Cx(thumb), Cy(thumb), 0);
    w.MouseMove(Cx(thumb), Cy(thumb) + 400);
    w.MouseUp(Cx(thumb), Cy(thumb) + 400);
    Check(w.FirstRow() == maxFirst, "dragging the thumb to the bottom");

    // A page without categories; the check of a check-slider.
    p = Draw(w);
    Click(w, Cx(p.FindText("Sound")), Cy(p.FindText("Sound")));
    p = Draw(w);
    Check(p.Find("inset")->x == 16 && p.Find("inset")->w == 608, "no category list: the content is full width");
    const Op *music = p.FindText("Music");
    Click(w, music->x + 2, music->y + 2);
    Check(w.DraftToggle(2, 0, 0) == 0 && w.Draft(2, 0, 0) == 80, "check-slider: the label toggles the checkbox");
    Check(AllInside(p), "everything inside the window (Sound)");

    // Restore defaults: the current category only; OK applies and closes; Esc cancels.
    g_values[0] = 150;
    g_values[4] = 0;
    w.Open(0, 1); // General, Speed
    p = Draw(w);
    Click(w, Cx(p.FindText("Restore defaults")), Cy(p.FindText("Restore defaults")));
    Check(w.Draft(0, 1, 0) == 80 && w.Draft(0, 0, 0) == 0, "Restore defaults: this category only");
    w.Cancel();
    g_values[4] = 1;
    g_values[6] = 30;
    w.Open(2, 0);
    p = Draw(w);
    Click(w, Cx(p.FindText("Restore defaults")), Cy(p.FindText("Restore defaults")));
    Check(w.Draft(2, 0, 0) == 80 && w.DraftToggle(2, 0, 0) == 1 && w.Draft(0, 1, 0) == 150,
          "Restore defaults: a page without categories; other pages kept");
    w.MouseDown(Cx(p.FindText("OK")), Cy(p.FindText("OK")), 0);
    co::Action a = w.MouseUp(Cx(p.FindText("OK")), Cy(p.FindText("OK")));
    Check(a == co::Action::Close && g_values[6] == 80, "OK applies and closes");
    w.Open(0, 0);
    p = Draw(w);
    Click(w, Cx(p.FindText("Autosave")), Cy(p.FindText("Autosave")));
    Check(w.Key(0x1b) == co::Action::Close && g_values[4] == 1, "Esc cancels and closes");
    Click(w, Cx(p.FindText("Autosave")), Cy(p.FindText("Autosave")));
    Check(w.Key(0x0d) == co::Action::Close && g_values[4] == 0, "Enter applies and closes");
    p = Draw(w);
    w.MouseDown(Cx(p.FindText("Hotkeys")), Cy(p.FindText("Hotkeys")), 0);
    Check(w.MouseUp(Cx(p.FindText("Hotkeys")), Cy(p.FindText("Hotkeys"))) == co::Action::OpenHotkeys,
          "the Hotkeys button");
    w.MouseDown(Cx(p.FindText("Hotkeys")), Cy(p.FindText("Hotkeys")), 0);
    Check(w.MouseUp(300, 300) == co::Action::None, "released elsewhere: nothing");

    // A choice's tooltip explains the value shown; "*" rows: the red mark and the red line under the rows.
    g_values[2] = 0;
    w.Open(1, 0);
    p = Draw(w);
    const Op *res = p.FindText("Resolution");
    const Op *star = p.FindText("*");
    Check(star && star->font == co::Font::Disabled && star->x > res->x + res->w && star->y == res->y,
          "red * after a restart row's label");
    const Op *value = p.FindText("Scaled");
    w.MouseMove(Cx(value), Cy(value));
    w.Tick(20000);
    w.Tick(20000 + co::Window::kTipDelayMs);
    p = Draw(w);
    const Op *red = p.FindText("Restart first."), *help = p.FindText("Help for scaled.");
    Check(w.TipShown() && help && red && red->font == co::Font::Disabled && red->y < help->y,
          "on the value: the red restart line, then the value's description");
    const Op *more2 = nullptr;
    for (const Op &o : p.ops) {
        if (o.op == "arrow" && o.extra / 10 == static_cast<int>(co::Arrow::Right) && !more2) more2 = &o;
    }
    w.MouseMove(Cx(more2), Cy(more2));
    w.Tick(20200 + co::Window::kTipDelayMs);
    Check(!w.TipShown(), "no tooltip on an arrow");
    w.MouseDown(Cx(more2), Cy(more2), 20500);
    w.MouseUp(Cx(more2), Cy(more2));
    w.Tick(20500 + co::Window::kTipDelayMs);
    Check(!w.TipShown() && w.Draft(1, 0, 0) == 1, "an arrow changes the value, still no tooltip");
    w.MouseMove(Cx(value), value->y - 4); // above the text, within the row
    w.Tick(21000);
    w.Tick(21000 + co::Window::kTipDelayMs);
    p = Draw(w);
    Check(w.TipShown() && p.FindText("Help for 1024."), "a little above the value: the new value's tooltip");
    w.MouseMove(res->x + 4, res->y + 14); // below the name's text, within the row
    w.Tick(22000);
    w.Tick(22000 + co::Window::kTipDelayMs);
    p = Draw(w);
    Check(w.TipShown() && p.FindText("Pick a size.") && p.FindText("Restart first."),
          "a little below the name: its tooltip, with the restart line");
    Click(w, Cx(more2), Cy(more2));
    w.MouseMove(Cx(value), Cy(value));
    w.Tick(23000);
    w.Tick(23000 + co::Window::kTipDelayMs);
    Check(!w.TipShown(), "a value without help: no tooltip");
    w.Cancel();
    w.Open(0, 0);
    p = Draw(w);
    Check(!p.FindText("*"), "no * rows: no mark");

    // "Keep this setting?": a relayout row applied asks; Keep calls the owner; no answer in 15 s reverts.
    {
        static int kept = 0;
        g_values[8] = 1;
        co::Row res;
        res.kind = co::Kind::Choice;
        res.label = "Size";
        res.value = V(8);
        res.choices = {"A", "B", "C"};
        res.relayout = true;
        co::Window r;
        r.SetOnKept([] { ++kept; });
        r.SetPages({{"Video", {{"", {res}}}}});
        r.Open(0, 0);
        r.Tick(1000);
        FakePainter q = Draw(r);
        const Op *more3 = nullptr;
        for (const Op &o : q.ops) {
            if (o.op == "arrow" && o.extra / 10 == static_cast<int>(co::Arrow::Right)) more3 = &o;
        }
        Click(r, Cx(more3), Cy(more3), 1000);
        q = Draw(r);
        Click(r, Cx(q.FindText("Apply")), Cy(q.FindText("Apply")), 1000);
        q = Draw(r);
        Check(g_values[8] == 2 && r.Confirming() && q.FindText("Keep this setting?") && q.FindText("Keep"),
              "relayout: applied, then asks to keep it");
        Click(r, Cx(q.FindText("Keep")), Cy(q.FindText("Keep")), 2000);
        Check(!r.Confirming() && kept == 1 && g_values[8] == 2, "Keep: kept, the owner told");
        Click(r, Cx(more3), Cy(more3), 3000);
        q = Draw(r);
        Click(r, Cx(q.FindText("Apply")), Cy(q.FindText("Apply")), 3000);
        r.Tick(3000 + 14000);
        Check(r.Confirming() && g_values[8] == 0, "still asking after 14 s");
        r.Tick(3000 + 15001);
        Check(!r.Confirming() && g_values[8] == 2 && kept == 1, "no answer in 15 s: the previous value back");
        Click(r, Cx(more3), Cy(more3), 20000);
        q = Draw(r);
        r.MouseDown(Cx(q.FindText("OK")), Cy(q.FindText("OK")), 20000);
        co::Action ra = r.MouseUp(Cx(q.FindText("OK")), Cy(q.FindText("OK")));
        Check(ra == co::Action::None && r.Confirming(), "OK with a relayout change: waits for the answer");
        Check(r.Key(0x0d) == co::Action::None && !r.Confirming() && r.TakeCloseRequest() && kept == 2,
              "Enter keeps, then the window asks to close");
    }

    // The Hotkeys window: Key rows, no Apply, no Hotkeys button.
    {
        g_values[9] = co::KeyCode(0x2d, 0);           // Insert
        g_values[10] = co::KeyCode(0x7a, 0);          // F11
        co::Row cols;
        cols.kind = co::Kind::Columns;
        cols.label = "Action";
        cols.rightLabel = "Key";
        co::Row k1;
        k1.kind = co::Kind::Key;
        k1.label = "Panel";
        k1.value = V(9);
        k1.defaultValue = co::KeyCode(0x2d, 0);
        co::Row k2 = k1;
        k2.label = "Screenshot";
        k2.value = V(10);
        k2.defaultValue = co::KeyCode(0x7a, 0);
        co::Window kw;
        co::Look look;
        look.title = "Hotkeys";
        look.hotkeysButton = look.applyButton = false;
        kw.SetLook(look);
        kw.SetGameKeyNote([](unsigned vk, unsigned) -> const char * { return vk == 0x50 ? "The game uses P." : nullptr; });
        kw.SetPages({{"Hotkeys", {{"", {cols, k1, k2}}}}});
        kw.Open(0, 0);
        FakePainter q = Draw(kw);
        Check(q.FindText("Hotkeys") && !q.FindText("Apply") && q.Count("text") > 0 && q.FindText("Insert") &&
                  q.FindText("F11") && q.FindText("Action") && q.FindText("Key"),
              "Hotkeys window: title, columns, keys; no Apply");
        int hotkeysButtons = 0;
        for (const Op &o : q.ops) hotkeysButtons += o.op == "text" && o.text == "Hotkeys" && o.y > 400;
        Check(hotkeysButtons == 0 && AllInside(q), "no Hotkeys button; everything inside");
        const Op *ins = q.FindText("Insert");
        Click(kw, Cx(ins), Cy(ins));
        q = Draw(kw);
        Check(kw.WaitingForKey() && q.FindText("Press a key") && q.Count("frame") >= 1, "a click on a key waits");
        kw.Key(0x10, core::ui::kModShift); // Shift alone
        Check(kw.WaitingForKey(), "a modifier alone: still waiting");
        kw.Key(0x7a, 0); // F11: the other row's
        q = Draw(kw);
        Check(kw.WaitingForKey() && q.FindText("F11 is already the key for \"Screenshot\"; try another one."),
              "a key in use: refused, still waiting");
        kw.Key(0x1b, 0);
        Check(!kw.WaitingForKey() && kw.Draft(0, 0, 1) == co::KeyCode(0x2d, 0) && kw.EndedWaitVk() == 0x1b,
              "Esc: not changed");
        Click(kw, Cx(ins), Cy(ins));
        kw.Key(0x50, core::ui::kModCtrl); // Ctrl+P
        q = Draw(kw);
        Check(!kw.WaitingForKey() && kw.Draft(0, 0, 1) == co::KeyCode(0x50, core::ui::kModCtrl) && q.FindText("Ctrl+P") &&
                  q.FindText("The game uses P."),
              "a new key: in the draft, the game's note shown");
        Check(g_values[9] == co::KeyCode(0x2d, 0), "not set before OK");
        q = Draw(kw);
        kw.MouseDown(Cx(q.FindText("OK")), Cy(q.FindText("OK")), 0);
        Check(kw.MouseUp(Cx(q.FindText("OK")), Cy(q.FindText("OK"))) == co::Action::Close &&
                  g_values[9] == co::KeyCode(0x50, core::ui::kModCtrl),
              "OK sets the new key and closes");
    }

    // Hover: the frame around the button under the mouse.
    w.MouseMove(Cx(p.FindText("Cancel")), Cy(p.FindText("Cancel")));
    p = Draw(w);
    Check(p.Count("frame") == 1, "frame around the button under the mouse");

    // One value that takes effect after a restart (the row itself does not): the red line only on that value's
    // tooltip, no "*" after the name; '\n' in a tooltip starts a new line.
    {
        g_values[2] = 0;
        co::Row present;
        present.kind = co::Kind::Choice;
        present.label = "Frame output";
        present.value = V(2);
        present.choices = {"Display", "Original"};
        present.help = "First line.\nSecond line.";
        present.choiceHelp = {"Every refresh.", "The game's own."};
        present.choiceRestart = {false, true};
        co::Window v;
        v.SetPages({{"Video", {{"Screen", {present}}}}});
        v.SetRestartTip("Restart first.");
        v.Open(0, 0);
        FakePainter q = Draw(v);
        Check(!q.FindText("*"), "a value that needs a restart: no * after the name");
        const Op *name = q.FindText("Frame output"), *val = q.FindText("Display");
        v.MouseMove(Cx(val), Cy(val));
        v.Tick(30000);
        v.Tick(30000 + co::Window::kTipDelayMs);
        q = Draw(v);
        Check(v.TipShown() && q.FindText("Every refresh.") && !q.FindText("Restart first."),
              "the other value: its tooltip without the red line");
        const Op *more = nullptr;
        for (const Op &o : q.ops) {
            if (o.op == "arrow" && o.extra / 10 == static_cast<int>(co::Arrow::Right) && !more) more = &o;
        }
        Click(v, Cx(more), Cy(more));
        v.MouseMove(Cx(val), Cy(val));
        v.Tick(31000);
        v.Tick(31000 + co::Window::kTipDelayMs);
        q = Draw(v);
        const Op *red = q.FindText("Restart first."), *own = q.FindText("The game's own.");
        Check(v.TipShown() && red && own && red->y < own->y, "that value: the red line first, then its description");
        v.MouseMove(Cx(name), Cy(name));
        v.Tick(32000);
        v.Tick(32000 + co::Window::kTipDelayMs);
        q = Draw(v);
        const Op *l1 = q.FindText("First line."), *l2 = q.FindText("Second line.");
        Check(v.TipShown() && l1 && l2 && l2->y > l1->y && !q.FindText("Restart first."),
              "the name: no red line; a line break in the tooltip");
    }

    std::printf("%s\n", g_failures ? "SOME TESTS FAILED" : "all tests passed");
    return g_failures ? 1 : 0;
}
