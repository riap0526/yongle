// The options window (docs/resolution_ui/original_settings.md §13, §14): one window in the game's own style that
// takes the place of the original Options menu. Pages (tabs) hold categories (the list on the left; a page with a
// single unnamed category has no list), categories hold rows. Changes are drafts until Apply or OK; Cancel drops
// them. Everything is laid out in window coordinates (640 x 480, the size of the game's dialogs) and drawn through
// a Painter, which the game layer implements with the game's own drawing functions and art, so nothing here knows a
// game (and the offline test draws with a fake one).
//
// Threads: one thread only (the game thread in the DLL): Open, input, Tick, Draw.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace core::options {

constexpr int kWidth = 640, kHeight = 480;

enum class Font {
    Normal,   // white
    Gold,     // the selected tab and category, values, section titles
    Title,    // the window's title
    Small,    // tooltips
    Disabled, // red, the same size as Normal (a button that does nothing now)
};

enum class Arrow { Left, Right, Up, Down };
enum class ArrowState { Normal, Hover, Pressed };

// What the window draws (window coordinates; the game layer adds the window's position). Sizes are the art's.
class Painter {
public:
    virtual ~Painter() = default;
    virtual void Panel(int x, int y, int w, int h) = 0;        // the dialog frame with parchment (16 px tiles)
    virtual void Inset(int x, int y, int w, int h) = 0;        // a sunken box (16 px tiles)
    virtual void ButtonBar(int x, int y, int w, int style) = 0; // a text button's bar, 25 high (style 0 / 1)
    virtual void Frame(int x, int y, int w, int h) = 0;        // the frame around the button under the mouse
    virtual void Strip(int x, int y, int w) = 0;               // a strip 20 high (choice values, slider tracks)
    virtual void Check(int x, int y, bool on) = 0;             // 16 x 16
    virtual void ArrowButton(int x, int y, Arrow a, ArrowState s) = 0; // 24 x 24 round arrow
    virtual void Knob(int x, int y) = 0;                       // the slider's knob, kKnobSize square
    virtual void Thumb(int x, int y) = 0;                      // the scroll bar's thumb, 26 x 26
    virtual void Line(int x, int y, int w) = 0;                // a thin line, 3 high
    virtual void Text(const char *utf8, int x, int y, Font f) = 0; // top of the letters at y
    virtual int TextWidth(const char *utf8, Font f) = 0;
    virtual void TipBox(int x, int y, int w, int h) = 0;       // the tooltip's box (edge and fill)
};

constexpr int kKnobSize = 16;

// How a row reads and changes its value. get: the value in effect; set: called by Apply (only when the draft
// differs). Check: 0 / 1; Choice: the index; Slider: the number (autoValue below min means "automatic").
struct Value {
    int (*get)(const void *ctx) = nullptr;
    void (*set)(const void *ctx, int value) = nullptr;
    const void *ctx = nullptr;
};

enum class Kind {
    Header,      // a section title between two lines
    Check,       // a checkbox before the label
    Choice,      // label left, the value between two arrows on the right
    Slider,      // label on the first line; the number left on the second, then the slider
    CheckSlider, // a checkbox before the label on the first line (toggle), the slider on the second (value)
    Key,         // label left, the key in a strip on the right; a click waits for a new key (value: KeyCode)
    Columns,     // column titles over Key rows: label left, rightLabel over the keys, a line under them
};

// A key with its modifiers as a row's value: vk | mods << 16 (core/ui/hotkey.h kMod*); 0: none.
inline int KeyCode(unsigned vk, unsigned mods)
{
    return static_cast<int>(vk | mods << 16);
}

struct Row {
    Kind kind = Kind::Header;
    std::string id;    // for shownIf (may be empty)
    std::string label; // UTF-8
    std::string help;  // the tooltip ("" none)
    Value value;
    Value toggle;      // CheckSlider: the checkbox
    int defaultValue = 0, defaultToggle = 0;
    int min = 0, max = 100, step = 1;      // Slider
    int (*maxNow)() = nullptr;             // Slider: the largest value that has an effect now (nullptr: max)
    bool hasAuto = false;                  // Slider: one step below min means automatic, shown as autoLabel
    std::string autoLabel = "Auto";
    std::string unit;                      // Slider: after the number ("%")
    std::vector<std::string> choices;      // Choice: shown values
    std::vector<std::string> choiceHelp;   // Choice: the tooltip of each value ("" or missing: none)
    std::string shownIfId;                 // shown only while the row with this id (same page) ...
    int shownIfValue = 0;                  // ... has this draft value
    bool restart = false;                  // takes effect after a restart: a red "*" after the label
    bool relayout = false;                 // the screen is laid out again: Apply asks to keep it (15 s, else reverts)
    std::string rightLabel;                // Columns: the title over the keys
};

struct Category {
    std::string name; // "" on a page without a category list
    std::vector<Row> rows;
};

struct Page {
    std::string name;
    std::vector<Category> categories;
};

enum class Action {
    None,
    Close,       // OK or Cancel: the window wants to close
    OpenHotkeys, // the Hotkeys button
};

// What the window shows around its pages: the options window, or the Hotkeys window (no Hotkeys button, no Apply).
struct Look {
    std::string title = "Yongle options";
    bool hotkeysButton = true;
    bool applyButton = true;
};

// The texts of a wait for a key and of "keep this setting?" (the game layer gives them, so they sit with its other
// texts for players).
struct Texts {
    std::string pressKey = "Press a key";
    std::string notChanged = "Not changed.";
    std::string notUsable = "That key cannot be used here; try another one.";
    std::string inUse = "%s is already the key for \"%s\"; try another one."; // the key, the other row's label
    std::string keepTitle = "Keep this setting?";
    std::string keepCountdown = "The previous setting comes back in %d s."; // seconds left
    std::string keep = "Keep", revert = "Revert";
};

class Window {
public:
    void SetLook(Look look) { look_ = std::move(look); }
    void SetTexts(Texts texts) { texts_ = std::move(texts); }
    // A key the game itself uses: a sentence shown (not an error) when it is chosen; nullptr when none.
    void SetGameKeyNote(const char *(*note)(unsigned vk, unsigned mods)) { gameKeyNote_ = note; }
    // Called after "keep this setting?" is answered with Keep (the owner saves then).
    void SetOnKept(void (*kept)()) { onKept_ = kept; }
    void SetPages(std::vector<Page> pages);
    // The red first line of a "*" row's tooltips (its name's and its values'), above the description.
    void SetRestartTip(std::string text) { restartTip_ = std::move(text); }
    // Reads every row's value into its draft and shows page / category.
    void Open(int page, int category);

    void MouseMove(int x, int y);
    Action MouseDown(int x, int y, unsigned nowMs);
    Action MouseUp(int x, int y);
    void Wheel(int notches); // > 0: up
    // Windows virtual-key codes with the modifiers held (core/ui/hotkey.h kMod*): while a Key row waits, the new key
    // (Esc: not changed; a modifier alone: still waiting); otherwise Escape cancels, Enter is OK (or answers "keep
    // this setting?": Enter keeps, Escape reverts).
    Action Key(int vk, unsigned mods = 0);
    void Tick(unsigned nowMs); // held arrows repeat; the tooltip appears after a short pause, beside the cursor
    void Draw(Painter &p);

    bool WaitingForKey() const { return capture_ >= 0; }
    unsigned EndedWaitVk() const { return endedVk_; } // the key that ended the last wait (0 none)
    bool Confirming() const { return confirm_.pending; }
    // The window is being closed from outside (e.g. a right click): an unanswered "keep this setting?" reverts.
    void Closing();
    // After Tick: true once when the window wants to close (OK, then "keep this setting?" answered or timed out).
    bool TakeCloseRequest();

    bool Dirty() const;   // a draft differs from the value in effect (Apply is enabled)
    void Apply();         // sets the rows whose draft differs
    void Cancel();        // drafts back to the values in effect
    void RestoreDefaults(); // drafts of the current category to the defaults (Apply / OK still needed)

    // For tests and the game layer.
    size_t PageCount() const { return pages_.size(); }
    const std::vector<Category> &Categories(int page) const { return pages_[page].categories; }
    int PageIndex() const { return page_; }
    int CategoryIndex() const { return category_; }
    int FirstRow() const { return first_; }
    int MaxFirstRow();
    int Draft(int page, int category, int row) const;
    int DraftToggle(int page, int category, int row) const;
    bool TipShown() const { return tipShown_; }
    std::string TipText(); // what the tooltip shows now ("" none)

    // Layout (window coordinates), from the third round of mockups.
    static constexpr int kTabY = 50, kContentY = 82, kContentH = 344, kButtonY = kHeight - 42;
    static constexpr int kCategoryX = 16, kCategoryW = 176, kListX = 196;
    // The tooltip almost at once (the game's own show at once; a short pause keeps them from flashing while the mouse
    // crosses the rows).
    static constexpr int kTipDelayMs = 100, kRepeatDelayMs = 400, kRepeatMs = 60;

private:
    struct DraftValue {
        int value = 0, toggle = 0;
    };
    enum class Hit {
        None, Tab, CategoryButton, RowLabel, RowCheck, ArrowLess, ArrowMore, ChoiceValue, Track, ScrollUp, ScrollDown,
        ScrollTrack, Hotkeys, Defaults, CancelButton, Ok, ApplyButton, KeyStrip, Keep, Revert,
    };
    struct Button {
        Hit hit;
        const char *label;
        int x, w;
    };
    struct Confirm {
        bool pending = false;
        unsigned until = 0;
        bool closeAfter = false; // OK was pressed: close once answered
        struct Was {
            int page, category, row, value;
        };
        std::vector<Was> before;
    };
    struct Target {
        Hit hit = Hit::None;
        int index = 0; // tab, category or row (in the current category)
        bool operator==(const Target &o) const { return hit == o.hit && index == o.index; }
    };
    struct Placed {
        int row; // index in the category
        int y;   // top, window coordinates
    };

    Category &Current() { return pages_[page_].categories[category_]; }
    bool HasList() const;
    bool Shown(const Row &r) const;
    static int Height(const Row &r);
    int ContentX() const;
    int ContentW() const;
    bool Scrolls();
    int RowsHeight();   // the height the rows may take (less the note line under Key rows)
    bool HasKeyRows();  // the current category has Key rows (the note line under them)
    int Left() const;
    int Right();
    std::vector<int> VisibleRows();
    std::vector<Placed> Layout();
    void SliderGeometry(int rowY, int &arrowL, int &trackX, int &trackW, int &arrowR, int &y);
    int SliderMax(const Row &r) const;
    int SliderLow(const Row &r) const;
    void SetSliderFromX(int row, int x);
    void Step(int row, int dir);
    std::string ValueText(const Row &r, int v) const;
    Target HitTest(int x, int y);
    Action Activate(const Target &t, int x);
    void ScrollTo(int first);
    void Tabs(Painter &p);
    void Bottom(Painter &p);
    void Rows(Painter &p);
    void Scrollbar(Painter &p);
    void Tooltip(Painter &p);
    void TextButton(Painter &p, const char *label, int x, int y, int w, int style, Font f, bool hover);
    std::vector<Button> BottomButtons() const;
    void ConfirmBox(Painter &p);
    void Answer(bool keep);
    void KeyPressed(int vk, unsigned mods);
    std::string KeyText(int code) const;
    std::vector<std::string> Wrap(Painter &p, const std::string &text, int maxW, Font f);
    DraftValue &DraftOf(int row) { return drafts_[page_][category_][row]; }

    std::vector<Page> pages_;
    std::vector<std::vector<std::vector<DraftValue>>> drafts_;
    struct TabPos {
        int x, w;
    };
    std::vector<TabPos> tabs_;          // measured when drawn
    std::map<int, int> labelWidths_;    // row -> label width, measured when drawn
    int page_ = 0, category_ = 0, first_ = 0;
    int mouseX_ = -1, mouseY_ = -1;
    Target hover_, pressed_;
    unsigned pressedAt_ = 0, lastRepeat_ = 0, hoverSince_ = 0, now_ = 0;
    int dragRow_ = -1;
    bool dragThumb_ = false;
    Target tip_;           // what the tooltip is for (a row's label, check or choice)
    bool tipShown_ = false;
    std::string restartTip_;
    Look look_;
    Texts texts_;
    const char *(*gameKeyNote_)(unsigned, unsigned) = nullptr;
    void (*onKept_)() = nullptr;
    int capture_ = -1;          // the Key row (current category) waiting for a key
    unsigned endedVk_ = 0;
    std::string note_;          // under the rows: the last wait's result
    bool noteError_ = false;
    Confirm confirm_;
    bool closeRequest_ = false;
};

} // namespace core::options
