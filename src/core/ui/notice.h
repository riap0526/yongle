// Our messages in the game's own notice boxes: the boxes the game shows at the top of the city ("Game saved"), with
// their look, place and time on screen (15 s). The game's own "Warnings" switch does not hide ours: they answer
// something the player just did. This part is the game-independent one: the texts posted from any thread wait here
// and are handed to the game layer on the game thread, from the game layer's detour of the game's notice drawing
// (OnFrame, once per game frame). The game layer makes the boxes (Game below). Until it has installed this, or after
// a fault switched it off, Post shows the text on our own caption line (core/output SetNotice) instead.
#pragma once

namespace core::ui::notice {

// The longest text kept, with its terminating zero (the games' boxes hold 99 characters).
constexpr int kTextSize = 100;

// The zoom box's text (printf format with the zoom in percent); the same in every game.
extern const char *const kZoomFormat;
// The city and minimap pictures' texts (core/capture/city_picture); the same in every game.
extern const char *const kCityPictureSaved, *const kMinimapPictureSaved, *const kPictureNotSaved, *const kPictureBusy,
    *const kPictureOnlyCity, *const kPictureUnavailable, *const kPictureNoMemory, *const kPictureClosePanel;

// What the game layer does on the game thread. Every field is given; there are no defaults.
struct Game {
    // Whether the game has a red font for notices (Red::kNone: errors are shown in its plain notice font; the log says
    // so with the text).
    enum class Red { kGameFont, kNone };

    Game(const char *logPrefix_, Red red_, void (*newBox_)(const char *text, bool red), void (*zoomBox_)(int percent),
         bool (*showsBoxes_)())
        : logPrefix(logPrefix_), red(red_), newBox(newBox_), zoomBox(zoomBox_), showsBoxes(showsBoxes_)
    {
    }

    const char *logPrefix; // e.g. "notice" or "zeus: notice"
    Red red;
    // A new box with this text (at most kTextSize - 1 characters, UTF-8); red: an error or something to do first. No
    // box when there is no room or the game shows none now (the text is in the log already).
    void (*newBox)(const char *text, bool red);
    // The zoom box: made once, then rewritten in place while the zoom changes (kZoomFormat).
    void (*zoomBox)(int percent);
    // The game keeps its boxes on this frame's screen. Off it (a building's window, a message, a dialog...) the game
    // clears every box on every frame, so our texts wait until it is true again (at most kHoldMs).
    bool (*showsBoxes)();
};

// How long a text waits for a screen that shows the boxes; older ones are dropped (they are in the log).
constexpr unsigned long kHoldMs = 60000;

// From the game layer's attach, after its sites matched and its detour of the notice drawing is in. Logs that our
// messages go to the game's boxes. The Game is copied.
void Install(const Game &game);

bool Attached();

// Start-up, from the game layer's Attach before its checks: the prefix of Post's log line also when Install is never
// reached (the boxes not hooked: our messages on the caption line).
void SetLogPrefix(const char *prefix);

// From the game layer's detour of the game's notice drawing, before the game's own: the texts posted since the last
// frame, then the latest zoom -- or, while the game does not show its boxes (showsBoxes), nothing: they wait.
// Guarded: a fault switches this off (logged) and Post goes back to the caption line.
void OnFrame();

// Any thread: shows text in a notice box at the next game frame that shows the boxes (at most 4 waiting, longer texts
// cut to 99 characters). red: as the game shows errors and what needs attention, where it has such a font.
void Post(const char *text, bool red = false);

// Game thread, from the game layer's box: something of the game's faulted where the game layer guarded it (and put
// back what it had set); our notices are switched off as after a fault in OnFrame (logged with why).
void SwitchOff(const char *why);

// Any thread: the city's zoom in percent, for the zoom box at the next game frame (the latest one wins). Nothing when
// not installed.
void Zoom(int percent);

// For the offline test: back to the state before Install (the feature's switch is not reset; a test that switches it
// off runs in its own process).
void ResetForTest();

} // namespace core::ui::notice
