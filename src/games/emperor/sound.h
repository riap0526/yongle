// The game's sound, through Miles (mss32.dll), on the game thread only. What is done is the core's
// (core/sound/sound.h: paused while switched away, ambient sounds per second, the pan limit, quiet under our windows,
// the 10 s counts; core/sound/layer1.h: the nature sounds; their settings are registered by
// emperor_register_settings); here are Emperor's hooks and where the game keeps its sound manager, its ambient
// sounds' switch and volume, and which nature sounds are left out.
#pragma once

namespace emperor::sound {

// Checks the sites and installs the hooks; each part is left out (logged) when its site does not match.
bool Attach();

// Once per main-loop pass (game thread), before the game's pass: the core's sound pass (with the nature sounds).
void OnLoop();

} // namespace emperor::sound
