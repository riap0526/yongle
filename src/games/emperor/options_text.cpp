#include "games/emperor/options_text.h"

#include <cstring>

namespace emperor::options_text {
namespace {

#define CHOICES(name, ...) const ChoiceText name[] = {__VA_ARGS__}
#define N(a) static_cast<int>(sizeof(a) / sizeof((a)[0]))

CHOICES(kIntro,
        {"play", "The game's intro movie plays when the game starts."},
        {"skip", "The game starts at the main menu."});

CHOICES(kInfoWindow,
        {"original", "Above or below the building, moved with the middle of the city area, as the game does it."},
        {"centered", "In the middle of the city area."},
        {"cursor", "Beside the cursor, never past the edges of the screen."});

CHOICES(kFullSpeed,
        {"capped", "Always the same pace, a step above 90%: 90% runs 45 steps a second, 100% runs 62."},
        {"original", "The game's own 100%: as fast as your computer can draw. In a small city that is many times "
                     "faster than 90%; in a large city on a big screen only a little faster."});

CHOICES(kWindow,
        {"fullscreen", "The game covers its monitor, without a border."},
        {"windowed", "A window you can move and size. The game is laid out again for its size when you let go."});

CHOICES(kResolution,
        {"scaled", "The screen's own resolution divided by the display scale. At 100% every pixel of the game is one "
                   "pixel of the screen: the sharpest picture."},
        {"1024x768", "The game's own size, enlarged to the screen."},
        {"*", "The game is drawn at this size and enlarged to the screen. In a window, this is the window's size."});

CHOICES(kScaling,
        {"nearest", "Plain pixels. Some are a screen pixel wider than others."},
        {"linear", "Smooth, but softer."},
        {"sharp_bilinear", "Sharp: only the edge of each pixel is blended."});

CHOICES(kPresent,
        {"display", "A frame at every refresh of the screen."},
        {"cap60", "At most 60 frames a second."},
        {"original", "The game's own, about 20 frames a second. Takes effect after a restart, and this window is not "
                     "available then: the game's own options are used."});

CHOICES(kRenderer,
        {"auto", "Direct3D 11, or Direct3D 9 if Direct3D 11 cannot start."},
        {"direct3d", "Direct3D 9, or Direct3D 11 if Direct3D 9 cannot start."},
        {"direct3d11", "Direct3D 11."});

CHOICES(kShrink,
        {"average", "Each pixel on the screen is the average of the city's pixels it covers: smooth."},
        {"nearest", "Each pixel on the screen is one of the city's pixels: sharper, but it may shimmer while the view "
                    "moves."});

CHOICES(kLoading,
        {"native", "At their own size, centered, black around them."},
        {"scaled", "As large as fits, in a fret border on parchment."},
        {"scaled_black", "The whole 1024 x 768 picture enlarged to fit, with black bars."},
        {"original", "The whole 1024 x 768 picture at its own size, centered, black around it, as the game shows it on "
                     "a wide screen."});

CHOICES(kMenus,
        {"native_city", "At their own size in a fret border, with your city blurred around them. A picture of your "
                        "own named menu_background.png or .bmp in the game folder is used instead of the city."},
        {"native", "At their own size on parchment."},
        {"scaled", "As large as fits, on parchment."},
        {"scaled_black", "The whole 1024 x 768 picture enlarged to fit, with black bars."},
        {"original", "The whole 1024 x 768 picture at its own size, centered, black around it, as the game shows it on "
                     "a wide screen."});

CHOICES(kChinaMap,
        {"scaled", "Larger than its own size (see the scale below), in a fret border, over the background below."},
        {"native", "At its own size, centered, over the background below."},
        {"original", "As the game draws it: in the top left corner, the rest of the screen left as it was."});

CHOICES(kChinaMapBackground,
        {"map", "The same map, blurred and darkened."},
        {"city", "Your city as it was when you opened the map, standing still, as if the map were a window over it."});

CHOICES(kBackgroundSound,
        {"pause", "The music and the sounds wait, and go on where they were when you come back."},
        {"original", "The game stops them all, and another song starts when you come back."});

CHOICES(kAmbient,
        {"original", "Once in about 300 passes of the game's main loop, as the game does it: so it depends on the "
                     "resolution and the computer (about one a second at 1024 x 768 on a fast computer, far fewer at "
                     "a large resolution)."},
        {"0.25", "About one every 4 seconds."},
        {"0.5", "About one every 2 seconds: what the game gave on the computers of its time."},
        {"0.75", "About three every 4 seconds."},
        {"1.0", "About one every second."});

CHOICES(kMessageModes,
        {"full", "The whole message, written out in full."},
        {"condensed", "A shorter message, in fewer and plainer words."},
        {"off", "No message of this kind."});

const Text kTexts[] = {
    {"display", "intro_movie", "Whether the game plays its intro movie when it starts. If PlayIntroMovie=no is set in "
                               "the game's Emperor.ini, the movie is skipped either way.",
     kIntro, N(kIntro), true},
    {"ui", "info_window", "Where the window opens when you right-click a building. Applies to the next window you open.",
     kInfoWindow, N(kInfoWindow)},
    {"ui", "minimap_picture_scale", "The size of the minimap picture, in times the minimap in the game.", nullptr, 0},
    {"timing", "high_resolution_timer", "The game's timers tick every millisecond on every computer (otherwise every "
                                        "15.6 ms, unless another program asks for more). Keeps the game's pace even.",
     nullptr, 0},
    {"gameplay", "keep_speed", "When a frame takes long (a large city, a high resolution), the game's steps it missed "
                               "are made up in the next frame, so the game runs at its set speed. Off: the game's own "
                               "pace, slower when frames are slow.",
     nullptr, 0},
    {"gameplay", "full_speed", "How fast the game runs at game speed 100%.", kFullSpeed, N(kFullSpeed)},
    {"gameplay", "simulate_while_scrolling", "The city goes on while the view scrolls. Off: it stands still while "
                                             "scrolling, as in the original.",
     nullptr, 0},
    {"gameplay", "smooth_scroll", "The city scrolls pixel by pixel, at the game's own average speed, easing in and "
                                  "out. Off: the game's steps of whole tiles.",
     nullptr, 0},
    {"display", "window", "Full screen or a window. F5 switches between them.", kWindow, N(kWindow)},
    {"display", "resolution", "The size the game is drawn at. Menus and other full-screen pictures stay at "
                              "1024 x 768.",
     kResolution, N(kResolution)},
    {"display", "scale", "How large everything is. 100%: the sharpest picture, everything at its own size (small on a "
                         "large, high-resolution screen). Larger is bigger but softer. Auto: 1080 pixels high on a "
                         "taller screen. Never less than 1024 x 768.",
     nullptr, 0},
    {"display", "scaling", "How the picture is enlarged when the game's pixels are not the screen's: a display scale "
                           "above 100%, 1024 x 768, the menus, a window. In full screen at 100% all three look the "
                           "same.",
     kScaling, N(kScaling)},
    {"display", "present", "How often frames reach the screen. The game's own timing is the same with all three.",
     kPresent, N(kPresent)},
    {"display", "window_lock_cursor", "In a window: the cursor cannot leave the window while you play in it. Switch "
                                      "this off, or press F5, to move or size the window.",
     nullptr, 0},
    {"display", "renderer", "What puts the picture on the screen.", kRenderer, N(kRenderer)},
    {"display", "city_zoom", "The city's zoom when the game starts. Above 100 you see more of the city, below 100 "
                             "it is larger. While playing, zoom with the mouse wheel over the city; the middle mouse "
                             "button goes back to 100.",
     nullptr, 0},
    {"display", "city_zoom_shrink", "Above 100% zoom: how the city is made smaller on the screen.", kShrink,
     N(kShrink)},
    {"display", "loading_screen", "The pictures shown while a game loads.", kLoading, N(kLoading)},
    {"display", "menu_screens", "The main menu, the choice of emperor and campaign, briefings, high scores, victory "
                                "and defeat.",
     kMenus, N(kMenus)},
    {"display", "background_blur", "How much the picture of your city behind the menus is blurred. 0: not at all.",
     nullptr, 0},
    {"display", "background_brightness", "How bright the picture of your city behind the menus is. 100 with no "
                                         "blur: the city as it is.",
     nullptr, 0},
    {"display", "china_map", "How the map of China is shown on a screen larger than the map.", kChinaMap,
     N(kChinaMap)},
    {"display", "china_map_scale", "How large the map is, in percent of its own size. Auto: as large as fits.",
     nullptr, 0},
    {"display", "china_map_background", "What is around the map.", kChinaMapBackground, N(kChinaMapBackground)},
    {"sound", "background_sound", "What happens to the music and the sounds when you switch to another program.",
     kBackgroundSound, N(kBackgroundSound)},
    {"sound", "ambient_sounds", "How often the city plays a random sound of its life: birds, water, workshops.",
     kAmbient, N(kAmbient), false, "Ambient sounds: how often"},

    {"sound", "master_volume", "The volume of all the music and sounds together. The four volumes below are set "
                               "within it.",
     nullptr, 0},

    // The game's own options (kept in Emperor.inf, as the game's Options menu sets them).
    {"game", "game_speed", "How fast the game runs.", nullptr, 0, false, "Game speed"},
    {"game", "scroll_speed", "How fast the view moves when you scroll the city.", nullptr, 0, false, "Scroll speed"},
    {"game", "autosave", "The game saves your city by itself from time to time, as its own Autosave option does.",
     nullptr, 0, false, "Autosave"},
    {"game", "messages", nullptr, kMessageModes, N(kMessageModes)}, // no tooltip on the kinds' names
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

} // namespace

const char *const kRestartTip = "Takes effect after the game is restarted.";

const Text *Find(const char *section, const char *key)
{
    for (const Text &t : kTexts) {
        if (std::strcmp(t.section, section) == 0 && std::strcmp(t.key, key) == 0) {
            return &t;
        }
    }
    return nullptr;
}

const char *ChoiceHelp(const Text &t, const char *spell)
{
    const char *other = "";
    for (int k = 0; k < t.choiceCount; ++k) {
        if (std::strcmp(t.choices[k].spell, spell) == 0) {
            return t.choices[k].help;
        }
        if (std::strcmp(t.choices[k].spell, "*") == 0) {
            other = t.choices[k].help;
        }
    }
    return other;
}

} // namespace emperor::options_text
