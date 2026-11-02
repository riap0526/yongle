#include "games/emperor/intro.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/settings/settings.h"
#include "games/emperor/site.h"

#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e): docs/symbols.md config_ini_load 0x475380 and
// play_intro_movie 0x828398.
namespace emperor::intro {
namespace {

// config_ini_load: __cdecl, no arguments, plain ret (both ends, 0x4754d5 and 0x4754ed). Reads [Config] of Emperor.ini;
// PlayIntroMovie decides play_intro_movie (a byte, written at 0x4754c9 / 0x4754e2: 0 for "no", else 1).
const Site kConfigLoad{"config_ini_load", 0x475380, {0x81, 0xec, 0x14, 0x01, 0x00, 0x00, 0x8d, 0x44}};
const Var<BYTE> kPlayIntro{0x828398};

using Void_fn = void(__cdecl *)();
Void_fn o_ConfigLoad;

core::diag::Feature g_feature{"intro movie setting (games/emperor/intro)"};
bool g_skip = false; // [display] intro_movie = skip, as read at start-up

void Skip(void *)
{
    const BYTE was = *kPlayIntro;
    *kPlayIntro = 0;
    core_log("intro: intro_movie = skip: the intro movie is skipped (the game's Emperor.ini said %s; it is not "
             "changed)",
             was ? "play it" : "PlayIntroMovie=no");
}

void __cdecl ConfigLoad_detour()
{
    o_ConfigLoad();
    if (!g_skip) {
        core_log("intro: intro_movie = play: the game decides (its Emperor.ini says %s)",
                 *kPlayIntro ? "play it" : "PlayIntroMovie=no");
        return;
    }
    core::diag::Guarded(g_feature, &Skip, nullptr); // on a fault: the game plays the movie as its file says
}

const char *const kChoices[] = {"play", "skip"};
const char *const kLabels[] = {"play (the game's intro movie at start-up)", "skip (straight to the main menu)"};

} // namespace

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    const int i = core::settings::Register(Item{
        "display", "intro_movie", "Intro movie",
        "Whether the game plays its intro movie at start-up. Takes effect the next time the game starts. Yongle "
        "does not change the game's own Emperor.ini: if PlayIntroMovie=no is set there, the movie is skipped "
        "either way.",
        Type::Choice, "play", Effect::Restart, true, true, 0, 0, 0, nullptr, kChoices, kLabels, 2});
    core::settings::OnChange(i, [](int, const char *v, bool initial) {
        if (initial) {
            g_skip = std::strcmp(v, "skip") == 0;
        }
        return true;
    });
}

bool Attach()
{
    if (!Matches(kConfigLoad)) {
        core_log("intro: %s does not match: the intro movie plays as the game's Emperor.ini says", kConfigLoad.name);
        return false;
    }
    return Install(kConfigLoad, &ConfigLoad_detour, o_ConfigLoad);
}

} // namespace emperor::intro
