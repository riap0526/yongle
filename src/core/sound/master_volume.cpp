#include "core/sound/master_volume.h"

#include "core/diag/guard.h"
#include "core/hook/hook.h"
#include "core/log.h"
#include "core/settings/settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

// Evidence (Emperor.exe 1.0.1.0, SHA256 6373328b...53c0e; mss32.dll Miles 6.0i): the game sets volumes only through
// _AIL_set_sample_volume@8 (0x762a22), _AIL_set_stream_volume@8 (0x762c03 ... 0x76305a) and
// _AIL_set_3D_sample_volume@8 (0x763c30, 0x763fea), and reads them back through _AIL_stream_volume@4 (0x762ee0;
// the music's fade out 0x762f77 takes 5 off what it reads, the fade in 0x763008 adds 5 up to its target) and
// _AIL_3D_sample_volume@4 (0x763f9f) [confirmed: the calls to its imports, found with capstone]. All __stdcall.
// Zeus.exe 2.1.4.0 (SHA256 382bcd28...0a57; mss32.dll Miles 6.0m) imports the same Miles calls and its mss32.dll
// exports all eight hooked here [confirmed: the PE's import and export tables]; that the game sets its volumes through
// no other call is [inferred] from the same imports (checked by ear: music playing while the master volume moves).
namespace core::sound::master_volume {
namespace {

using SetVolume_fn = void(__stdcall *)(void *handle, int volume);
using GetVolume_fn = int(__stdcall *)(void *handle);

SetVolume_fn o_SetSample = nullptr, o_SetStream = nullptr, o_Set3D = nullptr;
GetVolume_fn o_GetStream = nullptr, o_Get3D = nullptr;

std::atomic<int> g_percent{100}; // [audio] master_volume
std::atomic<bool> g_on{true};     // [audio] master_sound: off is silence (the checkbox before the volume)
std::atomic<bool> g_active{false}; // every hook installed: scaling on (otherwise 100%)

int Percent()
{
    return !g_active.load() ? 100 : g_on.load() ? g_percent.load() : 0;
}
core::diag::Feature g_feature{"master volume (core/sound)"};
void (*g_reapplyGameVolumes)() = nullptr; // VolumeGame's, from Attach

// What the game asked for, by handle: the game reads its volumes back and steps them, so it gets back its own value
// (not the scaled one); and a change of the master volume sets every live handle again (a sound that loops, e.g. the
// frogs, keeps the volume it started with otherwise, as testing showed). A handle leaves the list when the game
// releases or closes it (_AIL_release_sample_handle@4, _AIL_release_3D_sample_handle@4, _AIL_close_stream@4), so
// only live handles are set again. Miles may call in from its timer thread too: a lock.
enum Kind { kStream, kSample, k3D };
struct Asked {
    void *handle;
    int volume;
    Kind kind;
};
constexpr int kAsked = 256;
Asked g_asked[kAsked];
int g_next = 0; // only when the list is full (should not happen): the slot overwritten
SRWLOCK g_lock = SRWLOCK_INIT;

using Release_fn = void(__stdcall *)(void *handle);
Release_fn o_ReleaseSample = nullptr, o_Release3D = nullptr, o_CloseStream = nullptr;

std::atomic<void *> g_solo{nullptr}; // SetSolo: only this handle is heard

int Scaled(void *h, int v)
{
    void *solo = g_solo.load();
    if (solo && h != solo) {
        return 0;
    }
    return (v * Percent() + 50) / 100;
}

void Remember(void *h, int v, Kind kind)
{
    AcquireSRWLockExclusive(&g_lock);
    int slot = -1, empty = -1;
    for (int k = 0; k < kAsked; ++k) {
        if (g_asked[k].handle == h) {
            slot = k;
            break;
        }
        if (empty < 0 && g_asked[k].handle == nullptr) {
            empty = k;
        }
    }
    if (slot < 0) {
        slot = empty;
    }
    if (slot < 0) {
        slot = g_next;
        g_next = (g_next + 1) % kAsked;
    }
    g_asked[slot] = {h, v, kind};
    ReleaseSRWLockExclusive(&g_lock);
}

void Forget(void *h)
{
    AcquireSRWLockExclusive(&g_lock);
    for (Asked &a : g_asked) {
        if (a.handle == h) {
            a = Asked{};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void __stdcall ReleaseSample_detour(void *h)
{
    Forget(h);
    o_ReleaseSample(h);
}
void __stdcall Release3D_detour(void *h)
{
    Forget(h);
    o_Release3D(h);
}
void __stdcall CloseStream_detour(void *h)
{
    Forget(h);
    o_CloseStream(h);
}

int Unscaled(void *h, int actual)
{
    AcquireSRWLockShared(&g_lock);
    int asked = -1;
    for (int k = 0; k < kAsked; ++k) {
        if (g_asked[k].handle == h) {
            asked = g_asked[k].volume;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (asked >= 0 && Scaled(h, asked) == actual) {
        return asked;
    }
    const int p = Percent();
    return p > 0 ? std::min(127, (actual * 100 + p / 2) / p) : actual;
}

void __stdcall SetSample_detour(void *h, int v)
{
    Remember(h, v, kSample);
    o_SetSample(h, Scaled(h, v));
}
void __stdcall SetStream_detour(void *h, int v)
{
    Remember(h, v, kStream);
    o_SetStream(h, Scaled(h, v));
}
void __stdcall Set3D_detour(void *h, int v)
{
    Remember(h, v, k3D);
    o_Set3D(h, Scaled(h, v));
}
int __stdcall GetStream_detour(void *h)
{
    return Unscaled(h, o_GetStream(h));
}
int __stdcall Get3D_detour(void *h)
{
    return Unscaled(h, o_Get3D(h));
}

bool g_attached = false;

void Reapply(void *)
{
    g_reapplyGameVolumes(); // e.g. Yongle: music, speech and ambient sounds, the game sets them again
    Asked live[kAsked];
    AcquireSRWLockShared(&g_lock);
    std::memcpy(live, g_asked, sizeof(live));
    ReleaseSRWLockShared(&g_lock);
    int count = 0;
    for (const Asked &a : live) { // every live handle with the volume the game last asked for, scaled anew
        if (!a.handle) {
            continue;
        }
        const SetVolume_fn set = a.kind == kSample ? o_SetSample : a.kind == k3D ? o_Set3D : o_SetStream;
        set(a.handle, Scaled(a.handle, a.volume));
        ++count;
    }
    core_log("master volume: %d%% (%s); %d sound(s) set again", g_percent.load(), g_on.load() ? "on" : "off",
             count);
}

// A change of the volume or the switch: every sound playing at once.
void Changed(bool initial)
{
    if (!initial && g_attached && core::diag::Enabled(g_feature)) {
        core::diag::Guarded(g_feature, &Reapply, nullptr);
    }
}

} // namespace

void NoGameVolumes() {}

void RegisterSettings()
{
    using core::settings::Effect;
    using core::settings::Item;
    using core::settings::Type;
    int i = core::settings::Register(Item{
        "audio", "master_volume", "Master volume",
        "The volume of all the game's music and sounds together; the game's own volumes (music, speech, general "
        "and ambient sounds) are set within it.",
        Type::Int, "100", Effect::Live, true, true, 0, 100, 5, nullptr, nullptr, nullptr, 0, "%"});
    core::settings::OnChange(i, [](int, const char *v, bool initial) {
        g_percent.store(std::max(0, std::min(100, std::atoi(v))));
        Changed(initial);
        return true;
    });
    i = core::settings::Register(Item{
        "audio", "master_sound", "Master volume: on",
        "off: no music and no sounds at all, whatever the volumes (the game's own switches and volumes keep their "
        "values).",
        Type::Bool, "on", Effect::Live, true, true, 0, 0, 0, nullptr, nullptr, nullptr, 0});
    core::settings::OnChange(i, [](int, const char *v, bool initial) {
        g_on.store(std::strcmp(v, "on") == 0);
        Changed(initial);
        return true;
    });
}

void SetSolo(void *handle)
{
    if (g_solo.exchange(handle) == handle) {
        return;
    }
    core_log("master volume: %s", handle ? "only the layer-1 sounds heard (debug)" : "all sounds heard again");
    Changed(false);
}

bool Attach(const VolumeGame &g)
{
    if (!g.reapplyGameVolumes) {
        core_log("master volume: the game layer gave no reapplyGameVolumes; not installed");
        return false;
    }
    g_reapplyGameVolumes = g.reapplyGameVolumes;
    HMODULE mss = GetModuleHandleA("mss32.dll");
    struct Hook {
        const char *name;
        void *detour;
        void **original;
    } hooks[] = {
        // The readers first: once a volume is scaled, the game must read back what it asked for (a fade in steps
        // up to its target by what it reads), so no setter is hooked unless both readers are.
        {"_AIL_stream_volume@4", reinterpret_cast<void *>(&GetStream_detour), reinterpret_cast<void **>(&o_GetStream)},
        {"_AIL_3D_sample_volume@4", reinterpret_cast<void *>(&Get3D_detour), reinterpret_cast<void **>(&o_Get3D)},
        // Then the ones that end a handle: it leaves our list, so a change of the master volume sets only live ones.
        {"_AIL_release_sample_handle@4", reinterpret_cast<void *>(&ReleaseSample_detour),
         reinterpret_cast<void **>(&o_ReleaseSample)},
        {"_AIL_release_3D_sample_handle@4", reinterpret_cast<void *>(&Release3D_detour),
         reinterpret_cast<void **>(&o_Release3D)},
        {"_AIL_close_stream@4", reinterpret_cast<void *>(&CloseStream_detour), reinterpret_cast<void **>(&o_CloseStream)},
        {"_AIL_set_sample_volume@8", reinterpret_cast<void *>(&SetSample_detour), reinterpret_cast<void **>(&o_SetSample)},
        {"_AIL_set_stream_volume@8", reinterpret_cast<void *>(&SetStream_detour), reinterpret_cast<void **>(&o_SetStream)},
        {"_AIL_set_3D_sample_volume@8", reinterpret_cast<void *>(&Set3D_detour), reinterpret_cast<void **>(&o_Set3D)},
    };
    constexpr int kHooks = sizeof(hooks) / sizeof(hooks[0]);
    void *where[kHooks] = {};
    for (int k = 0; k < kHooks; ++k) {
        where[k] = mss ? reinterpret_cast<void *>(GetProcAddress(mss, hooks[k].name)) : nullptr;
        if (!where[k]) {
            core_log("master volume: mss32.dll has no %s; not installed (the game's volumes as they are)",
                     hooks[k].name);
            return false;
        }
    }
    for (int k = 0; k < kHooks; ++k) {
        if (!core_hook_install(hooks[k].name, where[k], hooks[k].detour, hooks[k].original)) {
            core_log("master volume: %s could not be hooked; the volumes set from now on are not scaled",
                     hooks[k].name);
            return false; // the readers installed so far stay: with no setter scaled they give back what Miles has
        }
    }
    g_attached = true;
    g_active.store(true);
    core_log("master volume: installed (%d%%; the game's volumes set through Miles are scaled, read back as asked)",
             g_percent.load());
    return true;
}

void AttachForTest(const VolumeGame &g, const MilesVolume &fake, MilesVolume *detours)
{
    g_reapplyGameVolumes = g.reapplyGameVolumes;
    o_SetSample = fake.setSample;
    o_SetStream = fake.setStream;
    o_Set3D = fake.set3D;
    o_GetStream = fake.getStream;
    o_Get3D = fake.get3D;
    o_ReleaseSample = fake.releaseSample;
    o_Release3D = fake.release3D;
    o_CloseStream = fake.closeStream;
    *detours = MilesVolume{&SetSample_detour, &SetStream_detour, &Set3D_detour, &GetStream_detour,
                           &Get3D_detour,     &ReleaseSample_detour, &Release3D_detour, &CloseStream_detour};
    g_attached = true;
    g_active.store(true);
}

} // namespace core::sound::master_volume
