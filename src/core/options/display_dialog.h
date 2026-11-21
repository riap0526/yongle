// The game's own Display options dialog (game_screen 0x0a in both games) with our display settings. Both games have
// the same dialog (the same panel, button rectangles and texts), so its buttons, layout, what they do and what is put
// back are here; the game layer hooks the dialog's drawing, hands the game our button table and draws with its own
// art (Game).
//
// The original dialog has four buttons: Full screen / Windowed screen, 800 x 600, 1024 x 768 and Cancel. Ours, in the
// same place:
//   Windowed screen / Full screen   our window mode (as F5), saved at once; the dialog stays open
//   1024 x 768 resolution           video.resolution = the game's own size
//   Resolution: W x H               the size the game draws now
//   Display scale: 125%  [v][^]     (or Auto (135%) / off) video.resolution = scaled with the scale one step (5 %)
//                                   smaller / larger
//   [ 100% ] [ Auto ]               scaled, scale 100 / auto
//   [ Scaling: ... ]                nearest -> linear -> sharp bilinear, saved at once
//   [ OK ] [ Cancel ]               OK keeps the changes (saved to our settings file); Cancel puts back the resolution
//                                   and scale the dialog opened with. Closing it any other way (Esc) is a Cancel.
// The changes apply while the dialog is open (the game is laid out again behind it); they are only saved with OK.
// 800 x 600 is gone (in play it was never wanted).
//
// Failure: drawing and the buttons run under one guard ("Display options dialog"); when it trips, Draw returns false
// and the game layer puts the game's own table and drawing back.
//
// Threads: Attach at start-up; everything else on the game thread.
#pragma once

namespace core::options::display_dialog {

// Our buttons, numbered from 1 as the game's button records number theirs (the hover variable, button_param).
enum Id { kWindowButton = 1, kNativeButton, kMinus, kPlus, k100, kAuto, kScalingButton, kOk, kCancelButton };
constexpr int kItemCount = kCancelButton;

enum class Arrow { None, Down, Up }; // Down makes it smaller (left), Up larger (right)

// A button in dialog coordinates (relative to the game's dialog_offset): text buttons are 20 high and w wide (a
// multiple of 16); arrows 24 x 24.
struct Item {
    int id;
    int x, y, w, h;
    Arrow arrow;
};
const Item (&Items())[kItemCount];
const Item &ItemOf(int id);

// The layout, relative to dialog_offset (the original: panel at (0x60, 0x50), 18 x 12 blocks of 16 px; buttons 14
// blocks wide at x 0x80, y 0x88 + 0x18 n; their text 4 px lower; title at y 0x5e). Ours is 18 blocks high. The scale
// line is laid out as a Sound options volume line: the text 6 px below the arrows' top.
constexpr int kPanelX = 0x60, kPanelY = 0x50, kPanelCols = 18, kPanelRows = 18;
constexpr int kLeft = 0x80, kWidth = 0xe0, kTitleY = 0x5e;
constexpr int kButtonH = 20, kArrowSize = 24, kTextDown = 4, kArrowTextDown = 6;
constexpr int kStatusY = 0xbe; // "Resolution: W x H"

// The game's own texts the dialog shows (both games: the dialog's text group, 0 title, 1 Full screen, 2 Windowed
// screen, 4 1024 x 768 resolution, 5 Cancel).
enum class GameText { Title, FullScreen, Windowed, NativeSize, Cancel };
constexpr int kTextIndex[] = {0, 1, 2, 4, 5}; // by GameText
constexpr int kTextGroup = 42;

enum class TextLook { Button, ButtonHover, Status };

// What the game layer gives (a constant of the game layer). Every field must be filled. Drawing is in dialog
// coordinates; the game adds its dialog_offset. Draw calls panel first, endDraw last.
struct Game {
    Game(void (*close)(), bool (*dialogShown)(), void (*screenSize)(int *w, int *h), int (*hover)(),
         bool (*drawBehind)(), void (*panel)(int x, int y, int cols, int rows),
         void (*button)(const Item &item, bool hovered), void (*arrow)(const Item &item, bool pressed),
         void (*gameText)(GameText t, int x, int y, int width, TextLook look),
         void (*text)(const char *text, int x, int y, int width, TextLook look), void (*endDraw)())
        : close(close), dialogShown(dialogShown), screenSize(screenSize), hover(hover), drawBehind(drawBehind),
          panel(panel), button(button), arrow(arrow), gameText(gameText), text(text), endDraw(endDraw)
    {
    }

    void (*close)();                  // the game's own way out of the dialog (back to the screen it came from)
    bool (*dialogShown)();            // game_screen is the dialog's
    void (*screenSize)(int *w, int *h); // the size the game draws now
    int (*hover)();                   // the button under the mouse (1-based, 0 none), as the game keeps it
    // The game was laid out again while the dialog is open (the draw buffer is new): the screen it came from drawn
    // again behind it. False: not now, asked again at the next drawing.
    bool (*drawBehind)();
    void (*panel)(int x, int y, int cols, int rows); // the game's dialog panel, in 16 px blocks
    void (*button)(const Item &item, bool hovered);  // a text button's frame or bar
    void (*arrow)(const Item &item, bool pressed);   // an arrow button (item.arrow)
    // Text centred in width; y as the game's dialog passes it (the button's top + kTextDown).
    void (*gameText)(GameText t, int x, int y, int width, TextLook look);
    void (*text)(const char *text, int x, int y, int width, TextLook look);
    void (*endDraw)(); // after the dialog: as the game's own drawing ends (the clip back to the screen, ...)
};

// Start-up, after the game layer checked and installed its part (g lives as long as the process).
void Attach(const Game &g);

// The dialog's drawing (from the game layer's hook of the game's own), under the guard. False: switched off or it
// failed (logged): the game layer puts the game's own dialog back and draws it.
bool Draw();

// Whether the guard is still on (the game layer checks it before drawing ours).
bool Working();

// The game's button handlers for our buttons (__cdecl, no arguments, as the game calls a button's onRelease): each
// presses its button under the guard. id 1..kItemCount.
using Handler = void(__cdecl *)();
Handler HandlerOf(int id);

// Once per main-loop iteration (game thread), before the game's pass: a dialog closed without OK puts the settings
// back.
void OnLoop();

// Pure parts, for the offline test.

// The largest scale (100..250, steps of 5) at which native / scale is still at least min (100 when none).
int MaxScale(int nativeW, int nativeH, int minW, int minH);
// The scale one step away from current (dir -1 or +1; current 0 = auto, which steps from the scale it chose, to the
// next 5 % step: 133 % -> 135 % / 130 %), kept within 100..MaxScale. Returns current's effective value when there is
// no step to take.
int StepScale(int current, int dir, int nativeW, int nativeH, int minW, int minH);
// The two status lines ("Resolution: 2048 x 1152", "Display scale: 125%" / "Display scale: Auto (135%)" /
// "Display scale: off"), in the games' character set (plain ASCII).
void StatusLines(int screenW, int screenH, bool scaled, int scale, int effectiveScale, char *line1, int size1,
                 char *line2, int size2);

// For the offline tests: the game layer's part again, the session closed and the guard on.
void DetachForTest();

} // namespace core::options::display_dialog
