// The unused nature sounds (0.2.0 item 10, player feedback; docs/archive/ambient_layer1.md): the game ships 24 recordings in
// Audio\Ambient\Layer1 (birds, wind, crickets, chimes ...) and reads their settings from Audio\sound_fxU.txt, but never
// plays them (they were meant for its 3D channels, which nothing starts). We play 12 of them (the buzzing insects,
// the two gongs and those the game already plays as layer 2 sounds left out) on one Miles voice of our own, in the
// city, now and then: each circles the listener as its line in sound_fxU.txt says (speed), heard as a slow change of
// left and right and of loudness. The game's sound effects volume and switch apply. Nothing is saved.
//
// Threads: the game thread only (Miles is called there, as the game does).
#pragma once

namespace emperor::nature_sounds {

// From emperor_register_settings (DllMain): [audio] nature_sounds, nature_sounds_gap.
void RegisterSettings();

// Once per main-loop pass (game thread, from emperor::sound::OnLoop): starts, moves and ends the sounds.
void OnLoop(unsigned long nowMs, bool city);

// Game thread: the game's sounds are paused (switched away): ours waits too. false: goes on. Our windows do not pause
// it: as the game's ambient sounds, the one playing goes on and no new one starts until they close (the game's passes,
// and so OnLoop, wait meanwhile).
void Pause(bool paused);

// The game stops every sound (sound_stop_all): ours ends too.
void Stop();

} // namespace emperor::nature_sounds
