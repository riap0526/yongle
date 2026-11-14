// The framed front-end screens both game layers share (Emperor's and Zeus's): while a front-end screen runs in its
// frame of the game's own size (core/display/resolution_switch: FrontEnd), the output frames it (core/display/framing):
// the picture -- the whole frame, or a loading picture's area -- goes straight from the game's pixels to its place on
// the screen, at its own size times the city's scale (CityScale), over a background composed here from the game's own
// dialog panel tiles (read from the player's own interface pack, in memory only): the game's panel pattern outside, the
// fret border and padding around the picture. Around the menus the player's city can be shown blurred (a picture of
// the city kept while playing, made into a background on a worker thread), or the player's own menu_background picture.
// debug.frame_labels draws the layout in label colours for a layout check.
//
// Settings: [video] loading_screen, menu_screens, background_blur, background_brightness. The menu_screens item is
// registered by each game layer (its description names the game's own screens), with the choices and labels here.
//
// Nothing here knows a game address: the game layer gives its pack, the tiles' first entries, its variables (as
// core::site::Var, read at each use), which screens show the city around them, and its loading screens' status block
// (or none) in a FramedGame, every field required. The game layer hooks its picture function and calls PictureShown
// after the picture was drawn, calls OnLoop once per loop, KeepCity after the city was drawn, OnGameEnd when the game
// ends.
//
// The setting of the resolution (panel, Shift+F7) is followed at once (resolution_switch's changed listener), also on
// a screen the game does not redraw; so is a change of the window's size (OnPresent, OnLoop).
//
// Failure: every entry point runs under one of two guards: "framed full-screen pictures" (on an exception: no framing
// any more, the game's own look -- the frame scaled with black bars, the game's own status line) and "city background
// of the front-end screens" (the panel pattern instead of the city).
#pragma once

#include "core/hook/site.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace core::display::framed {

constexpr WORD kTransparent = 0xF81F; // not drawn

// A w x h RGB565 picture the background is composed in. The game layer's status block draws into it.
struct Canvas {
    int w, h;
    std::vector<WORD> px;
    Canvas(int w_, int h_, WORD fill) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_, fill) {}
    // Pastes sw x sh pixels (kTransparent: not drawn) at (x, y), cut at the edges; labels: the label colours of
    // debug.frame_labels instead (dark lines and the rest of the border).
    void Paste(const WORD *src, int sw, int sh, int x, int y, bool labels);
};

// The loading screens' status block under the picture: a gap, the game's inset panel with the loader's status line
// in it, a gap (Emperor). A game without a status line on its loading screens gives zeros and nullptrs.
struct StatusBlock {
    using Draw_fn = bool (*)(Canvas &frame, const char *text, int x, int y, int w, int h); // false: not drawn
    using CaptionY_fn = int (*)();
    int gapAbove, insetHeight, gapBelow; // in units (picture pixels); all 0: no block
    Draw_fn draw;                        // the line (the game's bytes) in w x h at (x, y), in the game's own font
    CaptionY_fn captionY;                // where the output draws the line when it is not in the block (the frame's y)
    int Below() const { return gapAbove + insetHeight + gapBelow; }
};

// What the game gives. Every field is required (the constructor takes them all).
struct FramedGame {
    using CityAround_fn = bool (*)(unsigned char screen, bool playerPicture);

    FramedGame(const char *logPrefix, const wchar_t *interfacePack, int panelFirst, int insetFirst,
               core::site::Var<int> screenMode, core::site::Var<unsigned char> gameScreen, unsigned char videoScreen,
               CityAround_fn cityAround, StatusBlock status)
        : logPrefix(logPrefix), interfacePack(interfacePack), panelFirst(panelFirst), insetFirst(insetFirst),
          screenMode(screenMode), gameScreen(gameScreen), videoScreen(videoScreen), cityAround(cityAround),
          status(status)
    {
    }

    const char *logPrefix;         // the log lines' prefix, e.g. "fullscreen" or "zeus: fullscreen"
    const wchar_t *interfacePack;  // the game's interface pack, relative to the exe's folder (DATA\...sg3)
    int panelFirst;                // the dialog panel's 144 tiles (12 per row) start at this entry of the pack ...
    int insetFirst;                // ... and the inset panel's 49 tiles (7 per row) at this one
    core::site::Var<int> screenMode;         // the game's mode number; framed in mode 3 only
    core::site::Var<unsigned char> gameScreen; // the screen shown
    unsigned char videoScreen;     // the screen that plays videos (never framed)
    CityAround_fn cityAround;      // menu_screens = city: whether this screen shows the city around it
    StatusBlock status;
};

// The menu_screens choices and their panel labels (the game layer registers the item; MenuScreensChanged applies it).
extern const char *const kMenuChoices[4];
extern const char *const kMenuLabels[4];
bool MenuScreensChanged(int, const char *value, bool initial);

// From the game layer's settings registration: loading_screen, then registerMenuScreens() (the game's item, see
// above), then background_blur and background_brightness (the order of the panel and the ini).
void RegisterSettings(void (*registerMenuScreens)());

// What the game gives (at attach, also when framing is not installed: BorderPicture reads the tiles with it).
void Configure(const FramedGame &game);
// Starts framing after Configure (the game layer has checked and hooked its sites; resolution switching is
// installed). Also follows debug.frame_labels and the resolution setting.
void Install();
bool Installed();

// Runs fn(arg) under the framing's guard: the game layer's own steps for a picture or a status line. False when
// framing is off or fn was abandoned (the game's own look is back then).
bool Guarded(void (*fn)(void *), void *arg);

// Inside Guarded: a full-screen picture was drawn. loading: the area of a loading picture on the frame (it gets the
// loading screens' look and the status block), or nullptr for any other picture (the status line is cleared). Leaving
// the city, the city seen last becomes the menus' background and is saved for the next run. Then Update.
void PictureShown(const RECT *loading);
// Inside Guarded: the loading picture's area found later (Emperor finds the real area on the back buffer).
void SetLoadingArea(const RECT &area);
// Inside Guarded: the loader's status line (the game's bytes for its font, UTF-8 for the output), then Update.
void SetStatus(const char *gameText, const char *utf8);
// Whether the last background has the status line in the status block (the game's font).
bool StatusInBlock();

// Once per loop (game thread), or whenever the screen may have changed: framing on or off, the background composed
// for the current layout. Guarded.
void OnLoop();

// Before each present of the game's (game thread), for games whose menus run outside the loop that calls OnLoop: a
// city background finished on its worker thread, another screen, another window size or scale -> OnLoop.
void OnPresent();

// After the city was drawn on the back buffer (game thread; only drawings that are shown): keeps the city on screen
// every few seconds (left, top, right, bottom: the city's area on the back buffer, right and bottom exclusive).
void KeepCity(const BYTE *back, int pitch, int left, int top, int right, int bottom);

// The game layer did not hand a city drawing on (game thread), and why (e.g. an editor's map). If no city is kept in
// the whole run, one log line at the next front-end picture or the game's end gives the count and the last reason.
// KeepCity notes its own refusals (an area outside the buffer).
void NoteCitySkipped(const char *why, int left, int top, int right, int bottom);

// The game is ending (game thread): the city seen last is saved now if it was not yet, written before returning.
void OnGameEnd();

// How many city frames KeepCity was given so far (game thread).
int CityFramesDrawn();

// w x h RGB565 pixels: the panel pattern inside the dialog panel's fret border (one tile wide, the last tiles flush
// with the far edges), from the same tiles. False when the tiles cannot be read.
bool BorderPicture(int w, int h, std::vector<WORD> *out);

// For the offline tests: forget everything Configure, Install and the settings set.
void ResetForTest();

} // namespace core::display::framed
