// [audio] master_volume and master_sound: one volume over all of the game's sounds and music, as Augustus has. Not a
// setting of the game: the game's own volumes (its .inf) stay as the player set them; ours scales what the game hands
// to Miles (mss32.dll) when it sets a sound's volume, and gives the game back the volume it asked for when it reads one
// (its music fades read the volume and step it). Both games use Miles 6.0 and set their volumes through the same
// exports, so all of it is the core's; the game layer only says whether the game must set its own volumes again when
// ours changes (VolumeGame).
#pragma once

namespace core::sound::master_volume {

// What the master volume needs from the game (a constant of the game layer).
struct VolumeGame {
    explicit VolumeGame(void (*reapplyGameVolumes)()) : reapplyGameVolumes(reapplyGameVolumes) {}

    // When the master volume changes, before every live handle is set again: lets the game set its own stream
    // volumes again (Yongle: the game's music, speech and ambient volumes). Not nullptr: a game that needs nothing
    // gives a function that does nothing (NoGameVolumes).
    void (*reapplyGameVolumes)();
};

// For a game whose every volume goes through the hooked Miles calls after Attach: nothing to do.
void NoGameVolumes();

void RegisterSettings(); // start-up, before the settings are loaded: [audio] master_volume, master_sound

// After the game's Miles is loaded (an import of the exe). Hooks the eight Miles exports; when one is missing nothing
// is installed (logged) and the game's volumes stay as they are. False: not installed.
bool Attach(const VolumeGame &g);

// For testing (the layer-1 sounds' [debug] setting): while handle is not nullptr, every other sound and stream is
// given volume 0 (the game reads back what it asked for; its own volumes are not touched); nullptr: all again. Every
// live handle is set again at once. Game thread.
void SetSolo(void *handle);

// The eight Miles functions (all __stdcall), so the offline test can give fake ones.
struct MilesVolume {
    void(__stdcall *setSample)(void *handle, int volume);
    void(__stdcall *setStream)(void *handle, int volume);
    void(__stdcall *set3D)(void *handle, int volume);
    int(__stdcall *getStream)(void *handle);
    int(__stdcall *get3D)(void *handle);
    void(__stdcall *releaseSample)(void *handle);
    void(__stdcall *release3D)(void *handle);
    void(__stdcall *closeStream)(void *handle);
};

// For the offline test (no mss32.dll, nothing hooked): fake becomes the originals and *detours gets ours, which the
// test calls as the game would.
void AttachForTest(const VolumeGame &g, const MilesVolume &fake, MilesVolume *detours);

} // namespace core::sound::master_volume
