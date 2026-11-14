// Unattended smoke test: a short script of steps the game runs by itself -- wait for
// a screen or for a number of ticks, take a screenshot, write a report, end the game -- so a build can be checked for
// the faults that need no judgment (it starts, its hooks are in, nothing is switched off, the screens look as before)
// before anyone plays it.
//
// Only when the process was started with the environment variable IMPRESSIONS_AUTORUN set to the script's full path
// (a test runner sets it for the game it starts; a player's shortcut or launcher never has it). Without it nothing
// here runs. Every step goes to the log as a line starting "AUTORUN:". A failure of ours stops the steps and the game
// goes on as without them (core/diag/guard); only "quit" and the timeout end the game, through its own exit.
//
// Script: one step per line; ';' starts a comment; blank lines are skipped.
//   wait main_menu       the game's main menu screen, held for a few frames in a row
//   wait city            the city screen, held for a few frames in a row
//   wait screen <n>      that screen number (decimal or 0x...), held for a few frames in a row
//   wait frames <n>      n more game frames (what a frame is depends on the game layer: Emperor counts every
//                        screen_present call, which on its main menu runs thousands of times a second)
//   wait ms <n>          n milliseconds (checked on each frame)
//   wait ticks <n>       n more simulation ticks
//   shot                 a screenshot (as the screenshot key), waited for until it is saved
//   report               a problem report (as the report key), waited for until its picture is saved
//   game <action>        an action of the game layer (Game::actions lists them, e.g. "win lose"); one that cannot be
//                        done now (the game layer says why) ends the script with an error
//   click <x> <y>        a left click at that point of the display mode (mode pixels)
//   click dialog <x> <y> the same at a point of the game's 640 x 480 dialog area (Game::dialogOffset is added)
//   rclick               a right click where the mouse is
//                        A click is the button pressed, then released some frames and at least 100 ms later (the
//                        games see a click by comparing frames), then some more frames before the next step.
//   quit                 ask the game to end through its own exit
//   timeout <seconds>    the whole script's limit (default 120; anywhere in the script), counted from the game's first
//                        frame (an intro movie before it does not count): past it the game is ended
// While the script runs the game sees no real mouse: its cursor stays where the script last put it (the middle of the
// screen at first) and the real mouse messages do not reach it (core/display HoldMouse), so a player's mouse cannot
// disturb a run, nor the cursor at a screen edge scroll the city and stop its ticks.
// Nothing here knows a game: the game layer passes the screen numbers and the functions below.
#pragma once

#include <string>
#include <vector>

namespace core::diag::autorun {

struct Game {
    int (*screen)();     // the game's current screen number
    int mainMenuScreen;  // its main menu
    int cityScreen;      // its city screen
    long (*ticks)();     // simulation ticks run so far (any start value)
    void (*quit)();      // ask the game to end through its own exit (e.g. its quit flag)
    // Optional: a line of the game's state for the watchdog's log (flags that decide whether its loop runs). Reads
    // only; called from another thread.
    void (*describe)(char *out, int size) = nullptr;
    // Optional: the game layer's actions (step "game <action>"), their names separated by spaces, and the function
    // that does one, on the game thread after the game's frame: false with why (for the log) when it cannot be done.
    const char *actions = nullptr;
    bool (*action)(const char *name, char *why, int size) = nullptr;
    // Optional: where the game's 640 x 480 dialog area starts on the display mode (step "click dialog").
    void (*dialogOffset)(int *x, int *y) = nullptr;
};

enum class Op { WaitScreen, WaitFrames, WaitMs, WaitTicks, Shot, Report, Quit, Action, Click, RClick };

struct Step {
    Op op;
    long value; // the screen number, or how many frames or ticks; 0 otherwise
    std::string text; // the line as written, for the log
    std::string name = {}; // Action: its name
    long x = 0, y = 0;     // Click: the point
    bool dialog = false;   // Click: x, y are in the dialog area
};

struct Script {
    std::vector<Step> steps;
    long timeoutSeconds = 120;
};

// Reads the script text; the main menu and city names become the game's screen numbers; "game <action>" must name
// one of actions (space-separated; nullptr: none). false with a message (line number and why) when a line is not
// understood.
bool Parse(const char *text, int mainMenuScreen, int cityScreen, Script *out, std::string *error,
           const char *actions = nullptr);

// From the game layer's attach: reads IMPRESSIONS_AUTORUN and the script. Returns whether the autorun is on.
bool Start(const Game &game);

// From the game layer once per game frame, on the game thread, after the game's own frame. Does nothing when off.
void Frame();

bool Active();

// Whether the process was started for a smoke test (IMPRESSIONS_AUTORUN is set), whatever Start finds later: for
// what happens before the game's first frame (e.g. its intro movie), when Start has not run yet. A game layer skips
// its intro movie then, unless IntroRequested().
bool Requested();

// IMPRESSIONS_AUTORUN_INTRO=play: the test runner asks for the intro movie to be played as the settings say (the
// paths around the movie, e.g. its display mode switches).
bool IntroRequested();

// From another thread (core/output's thread, every pass): the watchdog. When the game's frames have stopped for a
// while and the script is past its timeout -- the game is in a loop of its own, e.g. a picture or a dialog waiting
// for a click -- a screenshot of that is asked for, then the game is ended through Game::quit. Does nothing
// otherwise, or when the autorun is off.
void Watch();

// For the offline test: the same as Start with a script given as text, and a clock in milliseconds (nullptr: the
// real one); shot and report requests go to the given functions, savedShots is a counter that grows when one is
// saved, and canShoot (asked when a shot or report step starts; nullptr: yes) says whether shots can be taken.
struct Hooks {
    unsigned long (*nowMs)();
    void (*requestShot)();
    void (*requestReport)();
    long (*savedShots)();
    bool (*canShoot)();
    // The mouse (nullptr: clicks fail): hold it at a mode point (on) or let it go; send a mouse message at a mode
    // point (false: no display mode yet); the display mode's size (false: none yet).
    void (*holdMouse)(bool on, long x, long y) = nullptr;
    bool (*sendMouse)(unsigned message, unsigned keys, long x, long y) = nullptr;
    bool (*modeSize)(long *w, long *h) = nullptr;
};
bool StartWith(const Game &game, const char *scriptText, const Hooks &hooks);
// The state after the script: "running", "done", "timeout" or "error".
const char *State();

} // namespace core::diag::autorun
