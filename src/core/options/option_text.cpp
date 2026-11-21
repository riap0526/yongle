// Texts for players (a text check scans every string literal here, for both games).
#include "core/options/option_text.h"

#include <cstring>

namespace core::options::option_text {
namespace {

#define CHOICES(name, ...) const ChoiceText name[] = {__VA_ARGS__}
#define N(a) static_cast<int>(sizeof(a) / sizeof((a)[0]))

// Choice names that fit the options window's value box (144 px in the value's font; Yongle's gold font measured with
// its own widths: "Scaled with black bars" is 151, so "Scaled + black bars", 130).

CHOICES(kInfoWindow,
        {"original", "Above or below the building, moved with the middle of the city area, as the game does it."},
        {"centered", "In the middle of the city area."},
        {"cursor", "Beside the cursor, never past the edges of the screen.", "Beside the cursor"});

CHOICES(kFullSpeed,
        {"capped", "Always the same pace, a step above 90%: 90% runs 45 steps a second, 100% runs 62.", "Even"},
        {"original", "The game's own 100%: as fast as your computer can draw. In a small city that is many times "
                     "faster than 90%; in a large city on a big screen only a little faster."});

CHOICES(kWindow,
        {"fullscreen", "The game covers its monitor, without a border.", "Full screen"},
        {"windowed", "A window you can move and size. The game is laid out again for its size when you let go.",
         "Window"});

CHOICES(kResolution,
        {"scaled", "The screen's own resolution divided by the display scale."},
        {"1024x768", "The game's own size, enlarged to the screen."},
        {"*", "The game is drawn at this size and enlarged to the screen. In a window, this is the window's size."});

CHOICES(kScaling,
        {"nearest", "Plain pixels. Some are a screen pixel wider than others."},
        {"linear", "Smooth, but softer."},
        {"sharp_bilinear", "Sharp: only the edge of each pixel is blended."});

CHOICES(kPresent,
        {"display", "A frame at every refresh of the screen."},
        {"cap60", "At most 60 frames a second.", "60 per second"},
        {"original", "The game's own, about 20 frames a second. This window is not available then: the game's own "
                     "options are used.",
         nullptr, true});

CHOICES(kRenderer,
        {"auto", "Direct3D 11, or Direct3D 9 if Direct3D 11 cannot start."},
        {"direct3d", "", "Direct3D 9"},
        {"direct3d11", "", "Direct3D 11"});

CHOICES(kShrink,
        {"average", "Each pixel on the screen is the average of the city's pixels it covers: smooth."},
        {"nearest", "Each pixel on the screen is one of the city's pixels: sharper, but it may shimmer while the view "
                    "moves."});

CHOICES(kLoading,
        {"framed", "In a fret border, black around it."},
        {"original", "The game's own picture, black around it, as the game shows it on a wide screen."});

CHOICES(kBackgroundSound,
        {"pause", "The music and the sounds wait, and go on where they were when you come back."},
        {"original", "As the game itself does it: it stops them all, and another song starts when you come back.",
         "Stop"});

CHOICES(kAmbient,
        {"original", "Once in about 300 passes of the game's main loop, as the game does it: so it depends on the "
                     "resolution and the computer (about one a second at 1024 x 768 on a fast computer, far fewer at "
                     "a large resolution)."},
        {"0.25", "About one every 4 seconds."},
        {"0.5", "About one every 2 seconds: what the game gave on the computers of its time."},
        {"0.75", "About three every 4 seconds."},
        {"1.0", "About one every second."});

CHOICES(kMinimapScale, {"1", "", "1x"}, {"2", "", "2x"}, {"4", "", "4x"});

CHOICES(kMessageModes,
        {"full", "The whole message, written out in full."},
        {"condensed", "A shorter message, in fewer and plainer words."},
        {"off", "No message of this kind."});

// The settings both games have, in both games' words.
const Text kShared[] = {
    {"ui", "info_window", "Where the window opens when you right-click a building. Applies to the next window you open.",
     kInfoWindow, N(kInfoWindow)},
    {"ui", "minimap_picture_scale", "How large the minimap screenshot saved with its key is (see Hotkeys), in times "
                                    "the minimap in the game. It does not change the minimap beside the city.",
     kMinimapScale, N(kMinimapScale), false, "Minimap screenshot: size"},
    {"gameplay", "keep_speed", "When a frame takes long (a large city, a high resolution), the game's steps it missed "
                               "are made up in the next frame, so the game runs at its set speed. Off: the game's own "
                               "pace, slower when frames are slow.",
     nullptr, 0},
    {"gameplay", "full_speed", "How fast the game runs at game speed 100%.", kFullSpeed, N(kFullSpeed)},
    {"gameplay", "simulate_while_scrolling", "The city goes on while the view scrolls. Off: it stands still while "
                                             "scrolling, as in the original.",
     nullptr, 0},
    {"gameplay", "smooth_scroll", "The city scrolls pixel by pixel, easing in and out, at a steady speed for each "
                                  "scroll speed setting. Off: the game's steps of whole tiles.",
     nullptr, 0},
    {"gameplay", "drag_scroll", "Hold the middle mouse button over the city and move the mouse: the city follows "
                                "it. A click without moving goes back to 100% zoom.",
     nullptr, 0},
    {"gameplay", "edge_scroll_width", "How close to the edge of the screen the cursor scrolls the city, in pixels. 1 "
                                      "is the original; 0 turns edge scrolling off (the arrow keys and dragging "
                                      "still scroll).",
     nullptr, 0},
    {"video", "window", "Full screen or a window. F5 switches between them.", kWindow, N(kWindow)},
    {"video", "resolution", "The size the game is drawn at. Menus and loading screens are shown as large as the "
                              "city.",
     kResolution, N(kResolution)},
    {"video", "scale", "How large everything is, menus and loading screens too. Larger is bigger but softer.\nAuto: "
                       "1080 pixels high on a taller screen. Never less than 1024 x 768.",
     nullptr, 0},
    {"video", "scaling", "How the picture is enlarged when the game's pixels are not the screen's: a display scale "
                           "above 100%, 1024 x 768, the menus, a window. In full screen at 100% all three look the "
                           "same.",
     kScaling, N(kScaling)},
    {"video", "present", "How often frames reach the screen. The game's own timing is the same with all three.",
     kPresent, N(kPresent)},
    {"video", "window_lock_cursor", "In a window: the cursor cannot leave the window while you play in it. Switch "
                                      "this off, or press F5, to move or size the window.",
     nullptr, 0},
    {"video", "renderer", "What puts the picture on the screen.", kRenderer, N(kRenderer)},
    {"video", "city_zoom", "The city's zoom when the game starts. Above 100 you see more of the city, below 100 "
                             "it is larger. While playing, zoom with the mouse wheel over the city; the middle mouse "
                             "button goes back to 100.",
     nullptr, 0},
    {"video", "city_zoom_shrink", "Above 100% zoom: how the city is made smaller on the screen.", kShrink,
     N(kShrink)},
    {"video", "loading_screen", "How the pictures shown while a game loads look.", kLoading, N(kLoading)},
    {"video", "background_blur", "How much the picture of your city behind the menus is blurred. 0: not at all.",
     nullptr, 0},
    {"video", "background_brightness", "How bright the picture of your city behind the menus is. 100 with no "
                                         "blur: the city as it is.",
     nullptr, 0},
    {"audio", "background_sound", "What happens to the music and the sounds when you switch to another program.",
     kBackgroundSound, N(kBackgroundSound)},
    {"audio", "ambient_sounds", "How often the city plays a random ambient sound of its buildings and land.",
     kAmbient, N(kAmbient), false, "Ambient sounds: how often"},
    {"audio", "nature_sounds", "The original game comes with these recordings but never played them. Played the way "
                               "the game designed them as 3D sounds, recreated in stereo: now and then in the city, "
                               "one comes down from above, moves around you for a while and rises away again, at the "
                               "ambient sounds volume.",
     nullptr, 0},
    {"audio", "nature_sounds_gap", "About how many seconds pass from the end of one to the start of the next.", nullptr,
     0, false, "Nature sounds: time between (s)"},
    {"audio", "master_volume", "The volume of all the music and sounds together. The four volumes below are set "
                               "within it. The checkbox switches them all off and on.",
     nullptr, 0},
    // {product}: the game's product (filled in by core/options/setting_rows).
    {"ui", "panel_key", "Opens and closes {product}'s panel for testing and finding problems. Most settings are in "
                        "Options.",
     nullptr, 0, false, "Debug panel"},
    {"debug", "screenshot_key", "Saves the game's picture as a PNG file in the shots folder in {product}'s folder.",
     nullptr, 0, false, "Save screenshot"},
    {"debug", "report_key", "When something odd is on the screen: marks the log and saves the picture shown, for a "
                            "problem report.",
     nullptr, 0, false, "Save report screenshot"},
    {"ui", "city_picture_key", "Saves the whole city as one picture in the shots folder; the game stops for a moment "
                               "while it is drawn.",
     nullptr, 0, false, "Save full city screenshot"},
    {"ui", "minimap_picture_key", "Saves the minimap of the whole map as a picture in the shots folder.", nullptr, 0,
     false, "Save minimap screenshot"},
    // The game's own options (core/options/game_options).
    {"game", "game_speed", "How fast the game runs.", nullptr, 0, false, "Game speed"},
    {"game", "scroll_speed", "How fast the view moves when you scroll the city.", nullptr, 0, false, "Scroll speed"},
    {"game", "autosave", "The game saves your city by itself from time to time, as its own Autosave option does.",
     nullptr, 0, false, "Autosave"},
    {"game", "messages", nullptr, kMessageModes, N(kMessageModes)}, // no tooltip on the kinds' names
    {"game", "difficulty", "How hard the game is. A change takes effect at once. The mission's result counts the "
                           "lowest difficulty you played it at: once you lower it, raising it again later does not "
                           "change that.",
     nullptr, 0, false, "Difficulty"},
    {"game", "auto_defend", "When enemies come, your soldiers gather and march out to meet them by themselves. They "
                            "stay out after the enemies have left: switch this off, then send them back yourself. It "
                            "can't be changed while enemies are attacking.",
     nullptr, 0, false, "Auto defend"},
    {"game", "music", "The music. The slider sets its volume.", nullptr, 0, false, "Music"},
    {"game", "speech", "The spoken voices. The slider sets their volume.", nullptr, 0, false, "Speech"},
    {"game", "sound_effects", "The sounds of the buildings, the people and the buttons. The slider sets their "
                              "volume.",
     nullptr, 0, false, "General sounds"},
    {"game", "ambient_volume", "The city's background sounds: birds, water, workshops. The slider sets their volume; "
                               "how often they play is set below.",
     nullptr, 0, false, "Ambient sounds"},
};

#undef CHOICES
#undef N

const Text *g_game = nullptr;
int g_gameCount = 0;

const Text *In(const Text *table, int count, const char *section, const char *key)
{
    for (int k = 0; k < count; ++k) {
        if (std::strcmp(table[k].section, section) == 0 && std::strcmp(table[k].key, key) == 0) {
            return &table[k];
        }
    }
    return nullptr;
}

} // namespace

void SetGameTexts(const Text *texts, int count)
{
    g_game = texts;
    g_gameCount = texts ? count : 0;
}

const Text *Find(const char *section, const char *key)
{
    const Text *t = In(g_game, g_gameCount, section, key);
    return t ? t : In(kShared, static_cast<int>(sizeof(kShared) / sizeof(kShared[0])), section, key);
}

const ChoiceText *Choice(const Text &t, const char *spell)
{
    const ChoiceText *other = nullptr;
    for (int k = 0; k < t.choiceCount; ++k) {
        if (std::strcmp(t.choices[k].spell, spell) == 0) {
            return &t.choices[k];
        }
        if (std::strcmp(t.choices[k].spell, "*") == 0) {
            other = &t.choices[k];
        }
    }
    return other;
}

} // namespace core::options::option_text
