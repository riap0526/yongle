#include "core/diag/perf.h"
#include "core/diag/report.h"
#include "core/ui/perf_view.h"
#include "core/output/sdl_output.h"

#include "core/capture/screenshot.h"
#include "core/diag/environment.h"
#include "core/diag/guard.h"
#include "core/diag/stall.h"
#include "core/display/framing.h"
#include "core/log.h"
#include "core/ui/panel.h"

#include <SDL3/SDL.h>
#include <d3d9.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core::output {

namespace {

// ---------------------------------------------------------------------------------------------
// SDL state: owned by the game thread (original) or by the output thread (display/cap60), never both.

HWND g_hwnd = nullptr;
SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
int g_texW = 0, g_texH = 0;
core_scaling g_scaling = CORE_SCALING_SHARP_BILINEAR;
core_present_mode g_mode = CORE_PRESENT_ORIGINAL;
int g_vsync = 0;
std::atomic<int> g_wantedMode{-1}; // SetPresentMode: the mode the output thread switches to (-1: none)
std::atomic<int> g_refreshHz{0};
bool g_failed = false;
long g_presentErrors = 0;

std::atomic<long> g_presented{0}; // frames handed over by the game
std::atomic<long> g_shown{0};     // frames presented on screen

void Fail(const char *what)
{
    core_log("output: %s failed: %s -- no picture will be shown", what, SDL_GetError());
    g_failed = true;
}

void SetScaling(SDL_Texture *t)
{
    SDL_ScaleMode mode = g_scaling == CORE_SCALING_NEAREST ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_PIXELART;
    if (!SDL_SetTextureScaleMode(t, mode)) {
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
}

// ---------------------------------------------------------------------------------------------
// Framed full-screen pictures: background layers (set by the game layer on any thread), status line, screen capture

struct Background {
    int version = 0;
    int screenW = 0, screenH = 0, pictureW = 0, pictureH = 0, native = 0;
    int outerU = 0, outerV = 0, frameU = 0, frameV = 0;
    BYTE *outer = nullptr, *frame = nullptr; // RGB565, tightly packed
};
SRWLOCK g_bgLock = SRWLOCK_INIT;
Background g_bg;               // under g_bgLock
int g_bgUploaded = -1;         // the version in the textures (drawing thread)
SDL_Texture *g_bgOuter = nullptr, *g_bgFrame = nullptr;
int g_bgKey[5] = {0, 0, 0, 0, 0}; // screenW, screenH, pictureW, pictureH, native of the textures
int g_bgOuterU = 0, g_bgOuterV = 0; // the outer fill's size in units (drawn scaled by s from (0,0))

SRWLOCK g_captionLock = SRWLOCK_INIT;
char g_caption[512] = "";      // under g_captionLock
int g_captionY = 0;

SRWLOCK g_captureLock = SRWLOCK_INIT;
std::atomic<bool> g_captureWanted{false};
char g_captureStem[64] = "", g_captureTime[32] = ""; // under g_captureLock

void DropBackgroundTextures()
{
    if (g_bgOuter) {
        SDL_DestroyTexture(g_bgOuter);
        g_bgOuter = nullptr;
    }
    if (g_bgFrame) {
        SDL_DestroyTexture(g_bgFrame);
        g_bgFrame = nullptr;
    }
    g_bgUploaded = -1;
}

SDL_Texture *Upload(const BYTE *bits, int w, int h)
{
    SDL_Texture *t = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, nullptr, bits, w * 2);
        SetScaling(t);
    }
    return t;
}

// The background textures for this output and picture size, uploaded when the game layer composed new ones; false
// while they do not match (black meanwhile).
bool BackgroundFor(int ow, int oh, int pw, int ph, bool native)
{
    AcquireSRWLockShared(&g_bgLock);
    if (g_bg.version != g_bgUploaded && g_bg.outer && g_bg.frame) {
        DropBackgroundTextures();
        g_bgOuter = Upload(g_bg.outer, g_bg.outerU, g_bg.outerV);
        g_bgFrame = Upload(g_bg.frame, g_bg.frameU, g_bg.frameV);
        g_bgUploaded = g_bg.version;
        g_bgKey[0] = g_bg.screenW;
        g_bgKey[1] = g_bg.screenH;
        g_bgKey[2] = g_bg.pictureW;
        g_bgKey[3] = g_bg.pictureH;
        g_bgKey[4] = g_bg.native;
        g_bgOuterU = g_bg.outerU;
        g_bgOuterV = g_bg.outerV;
        if (!g_bgOuter || !g_bgFrame) {
            core_log("output: background textures failed: %s", SDL_GetError());
        }
    }
    ReleaseSRWLockShared(&g_bgLock);
    return g_bgOuter && g_bgFrame && g_bgKey[0] == ow && g_bgKey[1] == oh && g_bgKey[2] == pw && g_bgKey[3] == ph &&
           g_bgKey[4] == (native ? 1 : 0);
}

int g_loggedLayout[5] = {0, 0, 0, 0, -1};

void LogLayout(int ow, int oh, int pw, int ph, bool bg, const core::display::FrameLayout &l)
{
    if (g_loggedLayout[0] == ow && g_loggedLayout[1] == oh && g_loggedLayout[2] == pw && g_loggedLayout[3] == ph &&
        g_loggedLayout[4] == static_cast<int>(bg) + 2 * static_cast<int>(l.native)) {
        return;
    }
    g_loggedLayout[0] = ow;
    g_loggedLayout[1] = oh;
    g_loggedLayout[2] = pw;
    g_loggedLayout[3] = ph;
    g_loggedLayout[4] = static_cast<int>(bg) + 2 * static_cast<int>(l.native);
    core_log("output: framed picture %dx%d -> (%.2f,%.2f) %.2fx%.2f on %dx%d (one resample from its own pixels); "
             "frame %dx%d at %d,%d = %dx%d units, padding %d/%d units, border at the screen edge x %s y %s; s %.4f%s; "
             "background %s",
             pw, ph, l.picX, l.picY, l.picW, l.picH, ow, oh, l.frameW, l.frameH, l.frameX, l.frameY, l.frameU,
             l.frameV, l.padU, l.padV, l.snapX ? "yes" : "no", l.snapY ? "yes" : "no", l.s,
             l.native ? " (native size)" : "", bg ? "ready" : "not composed yet (black)");
}

// A screenshot asked for this frame: read the screen back before our panel is drawn.
void ReadBackForCapture(int ow, int oh)
{
    if (!g_captureWanted.exchange(false)) {
        return;
    }
    char stem[64], time[32];
    AcquireSRWLockShared(&g_captureLock);
    std::memcpy(stem, g_captureStem, sizeof(stem));
    std::memcpy(time, g_captureTime, sizeof(time));
    ReleaseSRWLockShared(&g_captureLock);
    char what[120];
    std::snprintf(what, sizeof(what), "%s_screen_%s", stem, time);
    if (!core::capture::HasRoom(what)) {
        return;
    }
    const DWORD t = GetTickCount();
    SDL_Surface *s = SDL_RenderReadPixels(g_renderer, nullptr);
    if (!s) {
        core_log("screenshot: reading back the %dx%d screen failed: %s", ow, oh, SDL_GetError());
        return;
    }
    core::capture::SaveScreen(s, stem, time, GetTickCount() - t);
}

bool MakeTexture(int w, int h)
{
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = nullptr;
    }
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_texture) {
        core_log("output: SDL_CreateTexture(RGB565 %dx%d) failed: %s", w, h, SDL_GetError());
        return false;
    }
    g_texW = w;
    g_texH = h;
    // Sharp bilinear: SDL's "pixel art" mode samples nearest texels and blends only across texel edges (about one
    // output pixel wide), so non-integer scales stay sharp without uneven pixel sizes.
    SDL_ScaleMode mode = g_scaling == CORE_SCALING_NEAREST ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_PIXELART;
    const char *name = mode == SDL_SCALEMODE_NEAREST ? "nearest" : "sharp bilinear (SDL_SCALEMODE_PIXELART)";
    const bool d3d9 = std::strcmp(SDL_GetRendererName(g_renderer), "direct3d") == 0;
    if (mode == SDL_SCALEMODE_PIXELART && d3d9) {
        name = "nearest (direct3d samples SDL_SCALEMODE_PIXELART as nearest: no sharp bilinear)"; // SDL 3.4.18
    }
    if (!SDL_SetTextureScaleMode(g_texture, mode)) {
        core_log("output: scale mode %s not supported (%s); using linear", name, SDL_GetError());
        SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_LINEAR);
        name = "linear (fallback)";
    }
    // RGB565 straight into the GPU texture, or converted by SDL on this thread first (direct3d offers only ARGB8888):
    // the conversion is part of "upload to the texture" in the time log.
    const auto *formats = static_cast<const SDL_PixelFormat *>(SDL_GetPointerProperty(
        SDL_GetRendererProperties(g_renderer), SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, nullptr));
    bool native = false;
    for (const SDL_PixelFormat *f = formats; f && *f != SDL_PIXELFORMAT_UNKNOWN; ++f) {
        native = native || *f == SDL_PIXELFORMAT_RGB565;
    }
    core_log("output: texture %dx%d RGB565 (%s%s), scaling %s", w, h,
             native ? "used as it is" : "converted by SDL on the CPU to ",
             native || !formats ? "" : SDL_GetPixelFormatName(formats[0]), name);
    return true;
}

// Vsync interval for the mode: original never waits; display waits for every refresh; cap60 for every Nth refresh
// so that refresh/N <= 60 (60 Hz: every refresh, 120: every 2nd, 144: every 3rd = 48 per second).
int VsyncFor(int refresh)
{
    if (g_mode == CORE_PRESENT_ORIGINAL) {
        return 0;
    }
    if (g_mode == CORE_PRESENT_DISPLAY || refresh <= 61) {
        return 1;
    }
    int n = static_cast<int>(std::ceil(refresh / 60.0 - 0.02));
    return n < 1 ? 1 : n > 4 ? 4 : n;
}

// ---------------------------------------------------------------------------------------------
// O1g (docs/optimization.md): how Direct3D 9 is paced, an experiment ([debug] d3d9_pacing). With vsync its present
// took 7.5 ms on average at 200 Hz (5 ms per refresh) and the game handed over fewer frames while the output was
// still presenting (FrameWanted). timer: vsync off, and before each present we wait until one refresh (cap60: N) after
// the last one: a high-resolution waitable timer up to about 1 ms before, then a busy wait on the performance
// counter. dwmflush: vsync off, and after each present DwmFlush (the desktop's next composition; cap60: N times).
// Direct3D 9 in a window is composed by the desktop, so no vsync should not tear [to be checked in the test].

enum Pacing { kPaceVsync = 0, kPaceTimer = 1, kPaceDwmFlush = 2 };
int g_pacing = kPaceVsync; // in effect for the current renderer (output thread)
HANDLE g_paceTimer = nullptr;
std::int64_t g_paceTarget = 0; // performance counter of the next present (timer)
using DwmFlush_fn = HRESULT(WINAPI *)();
DwmFlush_fn g_dwmFlush = nullptr;

std::int64_t Qpc()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

std::int64_t QpcPerSecond()
{
    static const std::int64_t f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return q.QuadPart;
    }();
    return f;
}

// Refreshes per frame shown: 1 (display), or N so that refresh / N <= 60 (cap60).
int PaceRefreshes()
{
    const int refresh = g_refreshHz.load();
    if (g_mode != CORE_PRESENT_CAP60 || refresh <= 61) {
        return 1;
    }
    int n = static_cast<int>(std::ceil(refresh / 60.0 - 0.02));
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

std::int64_t PaceInterval()
{
    const int refresh = g_refreshHz.load();
    return QpcPerSecond() * PaceRefreshes() / (refresh > 0 ? refresh : 60);
}

// After the renderer is made (output thread): which pacing applies to it; logged when it changes.
void SetupPacing()
{
    const int want = core_config_get()->d3d9_pacing;
    int now = kPaceVsync;
    static int logged = -1;
    const char *note = "";
    if (want != kPaceVsync && g_mode != CORE_PRESENT_ORIGINAL) {
        if (std::strcmp(SDL_GetRendererName(g_renderer), "direct3d") != 0) {
            note = " (has no effect on this renderer: Direct3D 11 keeps vsync)";
        }
        else if (want == kPaceTimer) {
            if (!g_paceTimer) {
                g_paceTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                     TIMER_ALL_ACCESS);
                if (g_paceTimer) {
                    core_log("output: pacing timer: high-resolution waitable timer");
                }
                else {
                    g_paceTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                    core_log("output: pacing timer: no high-resolution waitable timer (%lu); %s", GetLastError(),
                             g_paceTimer ? "a normal one (about 1 ms late at most; the busy wait makes up for it)"
                                         : "none either: busy wait only");
                }
            }
            now = kPaceTimer;
        }
        else {
            if (!g_dwmFlush) {
                HMODULE m = LoadLibraryW(L"dwmapi.dll");
                g_dwmFlush = m ? reinterpret_cast<DwmFlush_fn>(GetProcAddress(m, "DwmFlush")) : nullptr;
            }
            if (g_dwmFlush) {
                now = kPaceDwmFlush;
            }
            else {
                note = " (DwmFlush not found: vsync kept)";
            }
        }
    }
    g_pacing = now;
    g_paceTarget = 0;
    if (want != kPaceVsync && logged != now) {
        logged = now;
        core_log("output: [debug] d3d9_pacing = %s: %s%s", core_d3d9_pacing_names[want],
                 now == kPaceTimer      ? "vsync off, frames spaced by our timer"
                 : now == kPaceDwmFlush ? "vsync off, DwmFlush after each present"
                                        : "vsync as usual",
                 note);
    }
}

// Before the present (timer): until one interval after the last present.
void PaceBeforePresent()
{
    if (g_pacing != kPaceTimer || g_paceTarget == 0) {
        return;
    }
    const std::int64_t rest = g_paceTarget - Qpc();
    const std::int64_t spin = QpcPerSecond() / 1000; // the last ms: busy
    if (rest > spin && g_paceTimer) {
        LARGE_INTEGER due;
        due.QuadPart = -((rest - spin) * 10000000 / QpcPerSecond()); // relative, in 100 ns
        if (SetWaitableTimer(g_paceTimer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(g_paceTimer, 1000);
        }
    }
    while (Qpc() < g_paceTarget) {
        YieldProcessor();
    }
}

// After a present: the next target (timer: one interval on, or from now when this one came late, so a late frame is
// not followed by a burst); dwmflush: wait for the composition(s).
void PaceAfterPresent()
{
    if (g_pacing == kPaceTimer) {
        const std::int64_t t = Qpc(), iv = PaceInterval();
        g_paceTarget = g_paceTarget != 0 && t - g_paceTarget < iv / 4 ? g_paceTarget + iv : t + iv;
    }
    else if (g_pacing == kPaceDwmFlush) {
        for (int i = PaceRefreshes(); i > 0; --i) {
            g_dwmFlush();
        }
    }
}

// Frame intervals (O1g, all renderers): the time between presents, kept for the rate log (TakeIntervals).
SRWLOCK g_ivLock = SRWLOCK_INIT;
constexpr int kIvMax = 8192;
float g_iv[kIvMax];
int g_ivCount = 0;
long g_ivTotal = 0;
double g_ivSum = 0;
float g_ivMax = 0;
std::int64_t g_ivLast = 0;

void RecordInterval()
{
    const std::int64_t t = Qpc();
    if (g_ivLast != 0) {
        const float ms = static_cast<float>((t - g_ivLast) * 1000.0 / QpcPerSecond());
        AcquireSRWLockExclusive(&g_ivLock);
        if (g_ivCount < kIvMax) {
            g_iv[g_ivCount++] = ms;
        }
        ++g_ivTotal;
        g_ivSum += ms;
        if (ms > g_ivMax) {
            g_ivMax = ms;
        }
        ReleaseSRWLockExclusive(&g_ivLock);
    }
    g_ivLast = t;
}

// Sets the vsync interval: logs what was asked, whether SDL took it (an interval the device does not offer falls back
// to 1), and then, once, the frames per second actually presented over the next seconds while frames keep coming (O1:
// Direct3D 9 in a window may accept 4 and still present every refresh, so cap60 would not cap).
std::atomic<long> g_measureFrames{-1}; // frames presented since the measurement started; -1: not measuring
DWORD g_measureStart = 0;
std::atomic<bool> g_measureArmed{false}; // MeasureFromNow was called: measurements are made

void ApplyVsync(int interval)
{
    if (g_pacing != kPaceVsync && interval > 0) {
        interval = 0; // O1g: paced by us
    }
    const bool ok = SDL_SetRenderVSync(g_renderer, interval);
    g_vsync = ok ? interval : 1;
    if (!ok) {
        const char *why = SDL_GetError();
        SDL_SetRenderVSync(g_renderer, 1);
        core_log("output: vsync interval %d asked, not accepted by %s (%s); using 1", interval,
                 SDL_GetRendererName(g_renderer), why);
    }
    else {
        core_log("output: vsync interval %d asked, accepted by %s%s", interval, SDL_GetRendererName(g_renderer),
                 g_measureArmed ? "; measuring the frames presented" : "");
    }
    g_measureStart = 0;
    g_measureFrames = g_measureArmed ? 0 : -1;
}

// Output thread, after each frame presented: the measurement's end (3 s of frames; pauses longer than 200 ms between
// frames restart it, so a paused game does not lower the figure).
void MeasureVsync()
{
    static DWORD lastFrame = 0;
    long n = g_measureFrames.load();
    if (n < 0) {
        return;
    }
    const DWORD now = GetTickCount();
    if (g_measureStart == 0 || now - lastFrame > 200) {
        g_measureStart = now;
        g_measureFrames = 0;
        lastFrame = now;
        return;
    }
    lastFrame = now;
    g_measureFrames = ++n;
    if (now - g_measureStart >= 3000) {
        const double fps = n * 1000.0 / (now - g_measureStart);
        const int refresh = g_refreshHz.load();
        core_log("output: measured %.1f frames/s presented with vsync interval %d (display %d Hz: expected about "
                 "%.0f when frames come faster)%s",
                 fps, g_vsync, refresh, g_vsync > 0 && refresh > 0 ? static_cast<double>(refresh) / g_vsync : 0.0,
                 g_vsync > 1 && refresh > 0 && fps > 1.3 * refresh / g_vsync ? "; the interval is not honoured" : "");
        g_measureFrames = -1;
    }
}

// O1 (docs/optimization.md §1.4, 2026-10-28): in a window Direct3D 9 showed the picture out of proportion after a
// resize, Direct3D 11 did not. Direct3D 9's back buffer has the size SDL gives it (SDL's idea of the window, which
// never hears of size changes: its window procedure is not in the chain); Direct3D 11's swap chain follows the
// window. Logged after each creation and device reset: the real back buffer, the window's client area and SDL's sizes;
// and, checked every second (CheckWindowState, when is null), whenever the two differ in a way not logged yet.
//
// The second test (2026-10-28) showed why: the rebuild runs on the output thread while the window is still being
// dragged on the game thread, so Direct3D 9 took the client size of that moment (say 1701 wide) and the drag went on
// (to 1024). SDL's render output size for Direct3D 9 is the window's live size, not the back buffer's, so it matched
// the client and no rebuild followed; Direct3D 11's output size is its swap chain's, so it rebuilt once more. The
// rebuild is now decided on the real back buffer (g_backW/H, read after each creation and device reset).
int g_backW = -1, g_backH = -1; // the renderer's back buffer; -1: not known (then its output size is used)

// 2026-10-29: and no rebuild while the window is being dragged. During a size-move (SetSizing, from the game
// thread) the frames are drawn at the old size and stretched; at its end the output is rebuilt once. Outside a
// size-move (maximise, restore, F5, a resize by another program) it is rebuilt when the client size has stayed the
// same for kSettleMs. A size-move longer than kSizingMaxMs counts as ended (should WM_EXITSIZEMOVE never come).
constexpr ULONGLONG kSettleMs = 100;
constexpr ULONGLONG kSizingMaxMs = 5000;
std::atomic<ULONGLONG> g_sizingSince{0}; // GetTickCount64 at WM_ENTERSIZEMOVE; 0: not sizing
std::atomic<bool> g_sizingEnded{false};  // WM_EXITSIZEMOVE came: rebuild at the next frame if the size differs

// The real back buffer of the renderer: Direct3D 9's present parameters, Direct3D 11's swap chain. False when neither.
bool QueryBackBuffer(int *bw, int *bh, const char **how)
{
    *bw = *bh = -1;
    *how = "not known";
    SDL_PropertiesID props = SDL_GetRendererProperties(g_renderer);
    if (auto *dev = static_cast<IDirect3DDevice9 *>(
            SDL_GetPointerProperty(props, SDL_PROP_RENDERER_D3D9_DEVICE_POINTER, nullptr))) {
        IDirect3DSwapChain9 *sc = nullptr;
        D3DPRESENT_PARAMETERS pp{};
        if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
            if (SUCCEEDED(sc->GetPresentParameters(&pp))) {
                *bw = static_cast<int>(pp.BackBufferWidth);
                *bh = static_cast<int>(pp.BackBufferHeight);
                *how = pp.Windowed ? "Direct3D 9, windowed" : "Direct3D 9, full screen";
            }
            sc->Release();
        }
    }
    else if (auto *sc = static_cast<IDXGISwapChain *>(
                 SDL_GetPointerProperty(props, SDL_PROP_RENDERER_D3D11_SWAPCHAIN_POINTER, nullptr))) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) {
            *bw = static_cast<int>(d.BufferDesc.Width);
            *bh = static_cast<int>(d.BufferDesc.Height);
            *how = "Direct3D 11 swap chain";
        }
    }
    return *bw >= 0;
}

void LogBackBuffer(const char *when)
{
    if (!g_renderer || !g_window) {
        return;
    }
    int bw, bh;
    const char *how;
    if (QueryBackBuffer(&bw, &bh, &how) && when) {
        g_backW = bw; // after a creation or device reset: what DrawFrame compares with the client area
        g_backH = bh;
    }
    RECT cr{};
    GetClientRect(g_hwnd, &cr);
    int sw = 0, sh = 0, pw = 0, ph = 0, ow = 0, oh = 0;
    SDL_GetWindowSize(g_window, &sw, &sh);
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
    const bool same = bw == cr.right && bh == cr.bottom;
    static long lastDiff[4] = {};
    if (!when) {
        const long diff[4] = {bw, bh, cr.right, cr.bottom};
        // a 0x0 client: the window is closing (2026-10-30), not worth a line
        if (bw < 0 || same || cr.right <= 0 || cr.bottom <= 0 || std::memcmp(diff, lastDiff, sizeof(diff)) == 0) {
            return;
        }
        std::memcpy(lastDiff, diff, sizeof(diff));
        when = "check";
    }
    core_log("output: %s: back buffer %dx%d (%s), window client %ldx%ld, SDL window %dx%d (pixels %dx%d, density "
             "%.3f), render output %dx%d%s",
             when, bw, bh, how, cr.right, cr.bottom, sw, sh, pw, ph, SDL_GetWindowPixelDensity(g_window), ow, oh,
             bw < 0 || same ? "" : " -- THE BACK BUFFER DIFFERS FROM THE CLIENT AREA (the picture is stretched)");
}

} // namespace

void TakeIntervals(char *out, std::size_t size)
{
    static float iv[kIvMax];
    AcquireSRWLockExclusive(&g_ivLock);
    const int n = g_ivCount;
    const long total = g_ivTotal;
    const double sum = g_ivSum;
    const float mx = g_ivMax;
    std::memcpy(iv, g_iv, sizeof(float) * n);
    g_ivCount = 0;
    g_ivTotal = 0;
    g_ivSum = 0;
    g_ivMax = 0;
    ReleaseSRWLockExclusive(&g_ivLock);
    out[0] = 0;
    if (n == 0) {
        return;
    }
    std::sort(iv, iv + n);
    const float median = iv[n / 2], p95 = iv[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
    const int refresh = g_refreshHz.load();
    const double period = 1000.0 / (refresh > 0 ? refresh : 60);
    const double limit = period * PaceRefreshes();
    const double steady = limit > median ? limit : median;
    int over = 0;
    int bins[6] = {}; // 1, 2, 3, 4, 5-9, 10+ refresh periods
    for (int i = 0; i < n; ++i) {
        over += iv[i] > 1.5 * steady;
        const int k = static_cast<int>(iv[i] / period + 0.5);
        bins[k <= 1 ? 0 : k <= 4 ? k - 1 : k <= 9 ? 4 : 5]++;
    }
    auto pc = [n](int c) { return 100.0 * c / n; };
    std::snprintf(out, size,
                  "%ld frames, mean %.1f, median %.1f, p95 %.1f, max %.1f; over 1.5x the steady interval (%.1f ms = "
                  "the larger of the limit %.1f and the median): %.1f%% (%d); in refresh periods of %.2f ms: 1: "
                  "%.1f%%, 2: %.1f%%, 3: %.1f%%, 4: %.1f%%, 5-9: %.1f%%, 10+: %.1f%%%s",
                  total, sum / total, median, p95, mx, steady, limit, pc(over), over, period, pc(bins[0]),
                  pc(bins[1]), pc(bins[2]), pc(bins[3]), pc(bins[4]), pc(bins[5]),
                  total > n ? " (the share figures from the first 8192)" : "");
}

void SetSizing(bool on)
{
    if (on) {
        const ULONGLONG t = GetTickCount64();
        g_sizingSince = t ? t : 1;
    }
    else {
        g_sizingSince = 0;
        g_sizingEnded = true;
    }
}

void MeasureFromNow()
{
    if (!g_measureArmed.exchange(true)) {
        g_measureStart = 0;
        g_measureFrames = 0;
    }
}

namespace {

// Wraps the window and creates the renderer. SDL sizes its swap chain from the window at this moment and, because
// its window procedure is not in the chain, never hears of later size changes; DrawFrame rebuilds when they happen.
bool Create(bool first)
{
    LONG_PTR proc = GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, g_hwnd);
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g_window) {
        Fail("SDL_CreateWindowWithProperties(existing HWND)");
        return false;
    }
    bool restored = GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC) != proc;
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, proc);
    // auto: Direct3D 11, but Direct3D 9 when the Steam overlay is in the process (O1, docs/optimization.md §1.4: with
    // it Direct3D 11 stopped for about 2.5 s every 10-30 s; Direct3D 9 did not). If the first cannot start, the other
    // one, then SDL's default.
    const int setting = core_config_get()->renderer;
    const bool steam = GetModuleHandleW(L"gameoverlayrenderer.dll") != nullptr;
    const char *wanted = setting == 1 || (setting == 0 && steam) ? "direct3d" : "direct3d11";
    if (first && setting == 0) {
        core_log("output: renderer auto -> %s (%s)", wanted,
                 steam ? "the Steam overlay (gameoverlayrenderer.dll) is loaded: Direct3D 11 stutters with it"
                       : "no Steam overlay");
    }
    if (std::strcmp(wanted, "direct3d") == 0 && core_config_get()->fail_direct3d9) {
        SDL_SetError("made to fail by [debug] fail_direct3d9 = on");
        g_renderer = nullptr;
    }
    else {
        g_renderer = SDL_CreateRenderer(g_window, wanted);
    }
    if (!g_renderer) {
        const char *other = std::strcmp(wanted, "direct3d") == 0 ? "direct3d11" : "direct3d";
        core_log("output: %s renderer failed (%s); trying %s", wanted, SDL_GetError(), other);
        if (std::strcmp(other, "direct3d") != 0 || !core_config_get()->fail_direct3d9) {
            g_renderer = SDL_CreateRenderer(g_window, other);
        }
    }
    if (!g_renderer) {
        core_log("output: renderer failed (%s); trying SDL's default", SDL_GetError());
        g_renderer = SDL_CreateRenderer(g_window, nullptr);
    }
    if (!g_renderer) {
        Fail("SDL_CreateRenderer");
        return false;
    }
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
    int refresh = dm ? static_cast<int>(dm->refresh_rate + 0.5f) : 0;
    g_refreshHz = refresh;
    SetupPacing();
    ApplyVsync(VsyncFor(refresh));
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
    if (first) {
        static const char *const kMode[] = {"original (game thread)", "display (output thread)",
                                            "cap60 (output thread)"};
        int v = SDL_GetVersion();
        core_log("output: SDL %d.%d.%d, video driver %s, renderer %s (setting %s), output %dx%d, display %d Hz, "
                 "present %s, vsync interval %d; window procedure %s",
                 SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v),
                 SDL_GetCurrentVideoDriver(), SDL_GetRendererName(g_renderer), core_renderer_names[setting], ow, oh,
                 refresh, kMode[g_mode], g_vsync,
                 restored ? "given back to the game after SDL's subclass" : "was not subclassed by SDL");
    }
    else {
        core_log("output: rebuilt for the new window size, output %dx%d", ow, oh);
    }
    LogBackBuffer(first ? "created" : "rebuilt");
    return true;
}

// The window's client size changed: drop the SDL window and renderer and wrap the window again. SDL_DestroyWindow
// puts back the window procedure SDL saved when wrapping, so the current one is reinstated explicitly.
bool Rebuild()
{
    LONG_PTR proc = GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
    core::ui::RendererLost();
    DropBackgroundTextures();
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = nullptr;
    }
    SDL_DestroyRenderer(g_renderer);
    g_renderer = nullptr;
    g_backW = g_backH = -1;
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, proc);
    return Create(false);
}

bool StartSdl()
{
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        Fail("SDL_Init(VIDEO)");
        return false;
    }
    return Create(true);
}

// Our features drawn here, each under core/diag's guard: on an exception the feature is switched off (logged) and the
// output goes on without it (unframed, no status line, no screen screenshot, panel closed).
core::diag::Feature g_framedFeature{"framed pictures (core/output)"};
core::diag::Feature g_captionFeature{"status line (core/output)"};
core::diag::Feature g_captureFeature{"screen screenshot (core/output)"};
core::diag::Feature g_panelFeature{"settings panel (core/ui)"};

struct FramedDraw {
    const core::display::FrameLayout *l;
    const Framing *fr;
    int ow, oh, pw, ph;
    bool ok;
};

void DrawFramed(FramedDraw &d)
{
    const core::display::FrameLayout &l = *d.l; // laid out for the block: the picture plus what lies below it
    const int bh = d.ph + d.fr->below;
    const bool bg = BackgroundFor(d.ow, d.oh, d.pw, bh, d.fr->native);
    LogLayout(d.ow, d.oh, d.pw, bh, bg, l);
    if (bg) {
        SDL_FRect outer{0, 0, static_cast<float>(g_bgOuterU * l.s), static_cast<float>(g_bgOuterV * l.s)};
        SDL_FRect frame{static_cast<float>(l.frameX), static_cast<float>(l.frameY), static_cast<float>(l.frameW),
                        static_cast<float>(l.frameH)};
        d.ok = SDL_RenderTexture(g_renderer, g_bgOuter, nullptr, &outer) && d.ok;
        d.ok = SDL_RenderTexture(g_renderer, g_bgFrame, nullptr, &frame) && d.ok;
    }
    SDL_FRect block{static_cast<float>(l.picX), static_cast<float>(l.picY), static_cast<float>(l.picW),
                    static_cast<float>(l.picH)};
    SDL_FRect pic = block; // the picture: the block's top part
    pic.h = static_cast<float>(l.picH * d.ph / bh);
    if (d.fr->labels) {
        d.ok = SDL_SetRenderDrawColor(g_renderer, 255, 255, 255, 255) && SDL_RenderFillRect(g_renderer, &block) && d.ok;
    }
    else {
        d.ok = SDL_RenderTexture(g_renderer, g_texture, nullptr, &pic) && d.ok;
    }
}

struct CaptionDraw {
    const char *text;
    float x, y, w, h, font;
    bool strip;
    int ow, oh;
};

void DrawCaption(const CaptionDraw &c)
{
    if (c.text[0] && core::diag::Enabled(g_captionFeature)) {
        core::diag::Guarded(g_captionFeature,
                            [](void *p) {
                                auto *c = static_cast<const CaptionDraw *>(p);
                                core::ui::RenderCaption(g_renderer, c->ow, c->oh, c->text, c->x, c->y, c->w, c->h,
                                                        c->font, c->strip);
                            },
                            const_cast<CaptionDraw *>(&c));
    }
}

// Draws one frame into the window and presents it (blocking for vsync when the interval is nonzero).
// Framed: the background layers, then the picture box straight from the frame into its place (one resample), then
// the status line. Otherwise the whole frame into dst. Then the screen capture, if asked for, and our panel.
namespace perf = core::diag::perf;
core::diag::Feature g_overlayFeature{"performance overlay (core/output)"};

void DrawFrame(const void *pixels, int pitch, int w, int h, const RECT &dst, SIZE client, const Framing &fr)
{
    // Where the output's time goes (R2b.1-3): one sample per frame drawn.
    static const int kTotal = perf::Register("output frame", perf::Thread::Output, true);
    static const int kUpload = perf::Register("upload to the texture", perf::Thread::Output);
    static const int kDraw = perf::Register("draw (and overlays)", perf::Thread::Output);
    static const int kPresent = perf::Register("present (incl. vsync wait)", perf::Thread::Output);
    static const int kPace = perf::Register("pacing wait (ours)", perf::Thread::Output);
    if (!g_renderer || w <= 0 || h <= 0) {
        return;
    }
    struct Watched { // O1: a frame of a second or more is sampled (core/diag/stall)
        Watched() { core::diag::stall::Begin(core::diag::stall::Thread::Output); }
        ~Watched() { core::diag::stall::End(core::diag::stall::Thread::Output); }
    } watched;
    const std::int64_t t0 = perf::Now();
    core::diag::env::NoteCore(core::diag::env::Thread::Output);
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
    // The back buffer (see LogBackBuffer) decides too; but if rebuilding three times for the same client size does
    // not make it fit, it is left as it is (logged once) rather than rebuilt on every frame.
    static SIZE lastTry{};
    static int tries = 0;
    static SIZE seen{};
    static ULONGLONG seenAt = 0;
    static long deferred = 0; // frames drawn at the old size while waiting
    const ULONGLONG now = GetTickCount64();
    if (client.cx != seen.cx || client.cy != seen.cy) {
        seen = client;
        seenAt = now;
    }
    const ULONGLONG sizing = g_sizingSince.load();
    const bool ended = g_sizingEnded.exchange(false);
    const bool settled = sizing != 0 ? now - sizing >= kSizingMaxMs : ended || now - seenAt >= kSettleMs;
    const bool useBack = g_backW >= 0 && tries < 3;
    const int bw = useBack ? g_backW : ow, bh = useBack ? g_backH : oh;
    const bool differs = (ow != client.cx || oh != client.cy || bw != client.cx || bh != client.cy) &&
                         client.cx > 0 && client.cy > 0;
    if (differs && !settled) {
        ++deferred;
    }
    else if (differs) {
        if (deferred > 0) {
            core_log("output: rebuilding for %ldx%ld %s; %ld frames were drawn at the old size meanwhile", client.cx,
                     client.cy, ended ? "after the drag" : "once the size stayed the same", deferred);
            deferred = 0;
        }
        tries = lastTry.cx == client.cx && lastTry.cy == client.cy ? tries + 1 : 1;
        lastTry = client;
        if (!Rebuild()) {
            return;
        }
        SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
        if (tries == 3 && (g_backW != client.cx || g_backH != client.cy)) {
            core_log("output: the back buffer is %dx%d after 3 rebuilds for the client %ldx%ld; left as it is",
                     g_backW, g_backH, client.cx, client.cy);
        }
    }
    const RECT &b = fr.picture;
    const int pw = b.right - b.left, ph = b.bottom - b.top;
    core::display::FrameLayout l{};
    const bool framed = core::diag::Enabled(g_framedFeature) && fr.on && pw > 0 && ph > 0 && fr.below >= 0 &&
                        b.left >= 0 && b.top >= 0 && b.right <= w && b.bottom <= h &&
                        core::display::ComputeFrameLayout(ow, oh, pw, ph + fr.below, &l, fr.native);
    const BYTE *src = static_cast<const BYTE *>(pixels);
    int tw = w, th = h;
    if (framed) { // only the picture box goes into the texture: its edges are not blended with what lies outside
        src += static_cast<size_t>(pitch) * b.top + static_cast<size_t>(b.left) * 2;
        tw = pw;
        th = ph;
    }
    if ((!g_texture || tw != g_texW || th != g_texH) && !MakeTexture(tw, th)) {
        return;
    }
    std::int64_t t = perf::Now();
    bool ok = SDL_UpdateTexture(g_texture, nullptr, src, pitch);
    perf::Add(kUpload, perf::Now() - t);
    t = perf::Now();
    ok = SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255) && ok;
    ok = SDL_RenderClear(g_renderer) && ok;
    char caption[512];
    AcquireSRWLockShared(&g_captionLock);
    std::memcpy(caption, g_caption, sizeof(caption));
    const int captionY = g_captionY;
    ReleaseSRWLockShared(&g_captionLock);
    if (framed) {
        FramedDraw d{&l, &fr, ow, oh, pw, ph, ok};
        if (core::diag::Guarded(g_framedFeature, [](void *p) { DrawFramed(*static_cast<FramedDraw *>(p)); }, &d)) {
            ok = d.ok;
            const float k = static_cast<float>(l.picW / pw); // a dark strip over the picture's bottom: 22 units
                                                              // (only when the game layer did not draw the line)
            DrawCaption(CaptionDraw{caption, static_cast<float>(l.picX), static_cast<float>(l.picY + (ph - 30) * k),
                                    static_cast<float>(l.picW), 22 * k, 13 * k, true, ow, oh}); // high, 30 above
        }
    }
    else {
        SDL_FRect d{static_cast<float>(dst.left), static_cast<float>(dst.top), static_cast<float>(dst.right - dst.left),
                    static_cast<float>(dst.bottom - dst.top)};
        ok = SDL_RenderTexture(g_renderer, g_texture, nullptr, &d) && ok;
        const float k = d.h / h; // where the game's own GDI line went: centred, white on black
        DrawCaption(CaptionDraw{caption, d.x, d.y + captionY * k, d.w, 0, 13 * k, false, ow, oh});
    }
    if (g_captureWanted.load()) {
        int size[2] = {ow, oh};
        if (!core::diag::Guarded(g_captureFeature, [](void *p) { auto *s = static_cast<int *>(p); ReadBackForCapture(s[0], s[1]); }, size)) {
            g_captureWanted = false;
        }
    }
    if (core::ui::IsOpen()) { // our panel on top, at the window's real resolution
        int size[2] = {ow, oh};
        if (!core::diag::Guarded(g_panelFeature, [](void *p) { auto *s = static_cast<int *>(p); core::ui::Render(g_renderer, s[0], s[1]); }, size)) {
            core::ui::Abandon();
        }
    }
    else if (core_config_get()->perf_overlay && core::diag::Enabled(g_overlayFeature)) {
        int size[2] = {ow, oh};
        core::diag::Guarded(g_overlayFeature,
                            [](void *p) {
                                auto *s = static_cast<int *>(p);
                                core::ui::RenderOverlay(g_renderer, s[0], s[1], &core::ui::PerfWidget);
                            },
                            size);
    }
    perf::Add(kDraw, perf::Now() - t);
    t = perf::Now();
    PaceBeforePresent();
    perf::Add(kPace, perf::Now() - t);
    t = perf::Now();
    const bool presented = SDL_RenderPresent(g_renderer);
    ok = presented && ok;
    perf::Add(kPresent, perf::Now() - t);
    if (presented && g_pacing != kPaceVsync) {
        t = perf::Now();
        PaceAfterPresent();
        perf::Add(kPace, perf::Now() - t);
    }
    perf::Add(kTotal, perf::Now() - t0);
    perf::EndSample(perf::Thread::Output);
    // A present that fails returns at once (Direct3D 9 while its device is lost: Ctrl+Alt+Del, a sleep, another
    // program in exclusive full screen): wait about one refresh instead of spinning, and log only the start and the
    // end of such a spell (O1).
    static DWORD failingSince = 0;
    static long failures = 0;
    if (!presented) {
        if (failures++ == 0) {
            failingSince = GetTickCount();
            core_log("output: present failing (%s); waiting a refresh each time until it works again", SDL_GetError());
        }
        const int hz = g_refreshHz.load();
        Sleep(hz > 0 ? static_cast<DWORD>(1000 / hz) + 1 : 16);
    }
    else {
        if (failures > 0) {
            core_log("output: present works again after %ld failed frames (%lu ms)", failures,
                     GetTickCount() - failingSince);
            failures = 0;
        }
        MeasureVsync();
        RecordInterval();
    }
    if (ok) {
        ++g_shown;
    }
    else if (presented && ++g_presentErrors <= 16) {
        core_log("output: drawing the frame failed (%ld so far): %s", g_presentErrors, SDL_GetError());
    }
}

// ---------------------------------------------------------------------------------------------
// Triple buffer between the game thread (producer) and the output thread (consumer)

struct Frame {
    BYTE *bits = nullptr;
    size_t capacity = 0;
    int w = 0, h = 0, pitch = 0;
    RECT dst{};
    SIZE client{};
    Framing framing{};
    std::atomic<unsigned> seq{0}; // odd while the producer writes this copy (R2b.1 frame check, core/diag/report.h)
};

constexpr int kNew = 4; // flag in g_ready: the slot holds a frame the consumer has not taken yet
Frame g_slots[3];
int g_write = 0;                // producer's slot
int g_read = 1;                 // consumer's slot
std::atomic<int> g_ready{2};    // the slot in between, | kNew when fresh
std::atomic<bool> g_wanted{true};
std::atomic<bool> g_holdNext{false};
std::atomic<DWORD> g_holdMs{0};
DWORD g_holdUntil = 0;          // producer only
HANDLE g_frameEvent = nullptr;  // auto-reset: a frame was published
HANDLE g_readyEvent = nullptr;  // the output thread finished starting (successfully or not)
std::atomic<bool> g_threadOk{false};
bool g_threaded = false;

// The screenshot key and the report key (R2b.1), seen by the output thread itself (display/cap60): the game thread
// does not handle messages while the start-up loader runs or holds a stage, so the keys are polled here, once per
// press (not its auto-repeat), only while the game window is in front and with exactly the configured modifiers.
// The window subclass keeps the keys' messages from the game (core/display/virtual_display).
bool KeyPressed(unsigned vk, unsigned keyMods, bool &wasDown)
{
    const core_config *cfg = core_config_get();
    if (vk == 0 || (vk == cfg->panel_key_vk && keyMods == cfg->panel_key_mods)) {
        return false;
    }
    auto held = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
    const unsigned mods = (held(VK_CONTROL) ? 1u : 0u) | (held(VK_SHIFT) ? 2u : 0u) | (held(VK_MENU) ? 4u : 0u);
    const bool down = held(static_cast<int>(vk)) && mods == keyMods && GetForegroundWindow() == g_hwnd;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

bool ScreenshotKeyPressed()
{
    static bool wasDown = false;
    const core_config *cfg = core_config_get();
    return KeyPressed(cfg->screenshot_key_vk, cfg->screenshot_key_mods, wasDown);
}

bool ReportKeyPressed()
{
    static bool wasDown = false;
    const core_config *cfg = core_config_get();
    if (cfg->report_key_vk == cfg->screenshot_key_vk && cfg->report_key_mods == cfg->screenshot_key_mods) {
        return false;
    }
    return KeyPressed(cfg->report_key_vk, cfg->report_key_mods, wasDown);
}

core::diag::Feature g_shotFeature{"screenshot (core/output)"};

// Both pictures of a screenshot from the frame this thread shows: the game frame from its own copy, the screen by
// drawing that frame again and reading it back (so they always match, also while the game is paused or loading).
// label: nullptr for a screenshot, "report<n>" for a report.
void ScreenshotHere(const char *label = nullptr)
{
    const Frame &f = g_slots[g_read];
    if (!f.bits || IsIconic(g_hwnd)) {
        core_log("screenshot: no picture to save yet");
        return;
    }
    // The screen picture is the game picture when the frame fills the window 1:1 with nothing drawn around or over it
    // (e.g. the city at Display scale 100 %): then only one is saved (O1b).
    bool caption;
    AcquireSRWLockShared(&g_captionLock);
    caption = g_caption[0] != 0;
    ReleaseSRWLockShared(&g_captionLock);
    const bool same = !f.framing.on && !caption && f.dst.left == 0 && f.dst.top == 0 && f.dst.right == f.w &&
                      f.dst.bottom == f.h && f.client.cx == f.w && f.client.cy == f.h;
    struct Shot {
        const Frame *f;
        const char *label;
        bool screen;
    } shot{&f, label, !same};
    if (core::diag::Guarded(g_shotFeature,
                            [](void *p) {
                                auto *s = static_cast<Shot *>(p);
                                core::capture::Save(s->f->bits, s->f->pitch, s->f->w, s->f->h, s->label, s->screen);
                            },
                            &shot) &&
        !same) {
        DrawFrame(f.bits, f.pitch, f.w, f.h, f.dst, f.client, f.framing); // reads the screen back
    }
}

// Draws the frame just taken and checks that its copy did not change meanwhile (core/diag/report.h).
void DrawChecked(const Frame &f)
{
    const unsigned before = f.seq.load(std::memory_order_acquire);
    DrawFrame(f.bits, f.pitch, f.w, f.h, f.dst, f.client, f.framing);
    const unsigned after = f.seq.load(std::memory_order_acquire);
    if ((before & 1) || after != before) {
        core::diag::NoteTornFrame(before, after, f.w, f.h);
    }
}

// SDL's render events (O1, docs/resolution_ui/sdl_output_comparison.md §2). Direct3D 9 loses its device on Ctrl+Alt+Del,
// a sleep, a display change or a full-screen switch: SDL resets it in the next present and keeps our textures (it
// re-uploads them from its own copies), then sends RENDER_TARGETS_RESET; we draw into no target textures, so that
// is only logged. Direct3D 11 after a driver reset sends RENDER_DEVICE_RESET with every texture gone: ours are made
// again (the frame texture on the next frame, the backgrounds and the panel's on their next use). RENDER_DEVICE_LOST
// (the device could not be made again): the renderer is built anew. Nothing else reads SDL's event queue, so the
// other events are dropped here (this thread pumps no game window).
void HandleRenderEvents()
{
    SDL_Event ev[16];
    int n;
    while ((n = SDL_PeepEvents(ev, 16, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) > 0) {
        for (int i = 0; i < n; ++i) {
            switch (ev[i].type) {
            case SDL_EVENT_RENDER_TARGETS_RESET:
                core_log("output: the renderer's device was reset (render targets reset); SDL keeps our textures");
                LogBackBuffer("after the reset");
                break;
            case SDL_EVENT_RENDER_DEVICE_RESET:
                core_log("output: the renderer's device was made again (device reset): our textures are made again");
                core::ui::RendererLost();
                DropBackgroundTextures();
                if (g_texture) {
                    SDL_DestroyTexture(g_texture);
                    g_texture = nullptr;
                }
                break;
            case SDL_EVENT_RENDER_DEVICE_LOST:
                core_log("output: the renderer's device is lost: building the renderer anew");
                if (!Rebuild()) {
                    core_log("output: building the renderer anew failed: %s", SDL_GetError());
                }
                break;
            default:
                break;
            }
        }
    }
}

// O1a (docs/optimization.md): whether Windows treats the game window as hidden -- minimized, not in front, cloaked by
// the desktop window manager, or occluded for Direct3D 11 (a test present) -- logged when it changes, checked once a
// second on this thread (the swap chain is this thread's).
void CheckWindowState()
{
    static DWORD last = 0;
    static char logged[160] = "";
    const DWORD now = GetTickCount();
    if (now - last < 1000 || !g_renderer) {
        return;
    }
    last = now;
    LogBackBuffer(nullptr); // O1: a back buffer that no longer fits the window (Direct3D 9 after a resize)
    using DwmAttr_fn = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
    static DwmAttr_fn dwmAttr = [] {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        return m ? reinterpret_cast<DwmAttr_fn>(GetProcAddress(m, "DwmGetWindowAttribute")) : nullptr;
    }();
    DWORD cloaked = 0;
    constexpr DWORD kDwmCloaked = 14; // DWMWA_CLOAKED
    if (dwmAttr && FAILED(dwmAttr(g_hwnd, kDwmCloaked, &cloaked, sizeof(cloaked)))) {
        cloaked = 0;
    }
    const char *occluded = "not tested (not direct3d11)";
    auto *swap = static_cast<IDXGISwapChain *>(
        SDL_GetPointerProperty(SDL_GetRendererProperties(g_renderer), SDL_PROP_RENDERER_D3D11_SWAPCHAIN_POINTER, nullptr));
    if (swap) {
        HRESULT hr = swap->Present(0, DXGI_PRESENT_TEST);
        occluded = hr == DXGI_STATUS_OCCLUDED ? "yes" : SUCCEEDED(hr) ? "no" : "test failed";
    }
    char state[160];
    std::snprintf(state, sizeof(state), "in front %s, minimized %s, cloaked %s, occluded %s",
                  GetForegroundWindow() == g_hwnd ? "yes" : "no", IsIconic(g_hwnd) ? "yes" : "no",
                  cloaked ? "yes" : "no", occluded);
    if (std::strcmp(state, logged) != 0) {
        core_log("output: window %s", state);
        std::memcpy(logged, state, sizeof(logged));
    }
}

DWORD WINAPI OutputThread(void *)
{
    core::diag::env::RegisterThread(core::diag::env::Thread::Output);
    core::diag::stall::RegisterThread(core::diag::stall::Thread::Output);
    g_threadOk = StartSdl();
    SetEvent(g_readyEvent);
    if (!g_threadOk) {
        return 1;
    }
    for (;;) {
        // While our panel is open the game is paused and sends no frames: the last frame is drawn again with the
        // panel on top every refresh (vsync paces the loop), so the panel reacts to input.
        int want = g_wantedMode.exchange(-1);
        if (want >= 0 && want != g_mode && g_renderer) {
            g_mode = static_cast<core_present_mode>(want);
            core_log("output: present %s", want == CORE_PRESENT_CAP60 ? "cap60" : "display");
            ApplyVsync(VsyncFor(g_refreshHz.load()));
        }
        bool panel = core::ui::IsOpen();
        DWORD w = WaitForSingleObject(g_frameEvent, panel ? 0 : 100);
        // The thread owns no game windows, but SDL may own helper windows: keep their messages flowing.
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&m);
        }
        if (ScreenshotKeyPressed()) {
            ScreenshotHere();
        }
        if (ReportKeyPressed()) {
            char label[24];
            std::snprintf(label, sizeof(label), "report%d", core::diag::BeginReport("report key"));
            ScreenshotHere(label);
        }
        HandleRenderEvents();
        CheckWindowState();
        int r = g_ready.load();
        if (!(r & kNew) && panel) {
            const Frame &f = g_slots[g_read];
            if (IsIconic(g_hwnd) || !f.bits) {
                Sleep(16);
            }
            else {
                DrawFrame(f.bits, f.pitch, f.w, f.h, f.dst, f.client, f.framing);
            }
            continue;
        }
        if (!(r & kNew)) {
            if (w == WAIT_TIMEOUT) {
                g_wanted = true; // a requested frame never came (e.g. the game skipped presenting): ask again
            }
            continue;
        }
        g_read = g_ready.exchange(g_read) & 3;
        const Frame &f = g_slots[g_read];
        if (!IsIconic(g_hwnd)) {
            DrawChecked(f);
        }
        g_wanted = true;
    }
}

} // namespace

bool Init(HWND hwnd, core_scaling scaling, core_present_mode mode)
{
    if (g_renderer || g_threadOk) {
        return true;
    }
    if (g_failed) {
        return false;
    }
    g_hwnd = hwnd;
    g_scaling = scaling;
    g_mode = mode;
    if (mode == CORE_PRESENT_ORIGINAL) {
        return StartSdl();
    }
    g_threaded = true;
    g_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, &OutputThread, nullptr, 0, nullptr);
    if (!t) {
        core_log("output: CreateThread failed (err %lu)", GetLastError());
        g_failed = true;
        return false;
    }
    SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(t);
    // Wait for the thread, but keep handling messages sent to this thread's windows: SDL sends a few to the game
    // window while wrapping it, and SendMessage across threads only completes when this thread lets them through.
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &g_readyEvent, FALSE, 10000, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0) {
            break;
        }
        if (r == WAIT_OBJECT_0 + 1) {
            MSG m;
            PeekMessageA(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            continue;
        }
        core_log("output: output thread did not start within 10 s");
        g_failed = true;
        return false;
    }
    return g_threadOk;
}

bool Threaded()
{
    return g_threaded;
}

void Present(const void *pixels, int pitch, int w, int h, const RECT &dst, SIZE client, const Framing &framing)
{
    ++g_presented;
    DrawFrame(pixels, pitch, w, h, dst, client, framing);
}

void Submit(const void *pixels, int pitch, int w, int h, const RECT &dst, SIZE client, const Framing &framing)
{
    if (!g_threadOk || w <= 0 || h <= 0) {
        return;
    }
    ++g_presented;
    DWORD now = GetTickCount();
    if (g_holdNext.exchange(false)) {
        g_holdUntil = now + g_holdMs.load();
    }
    else if (static_cast<LONG>(g_holdUntil - now) > 0) {
        return; // keep the held frame on screen (FrameWanted stays false until the hold ends)
    }
    Frame &f = g_slots[g_write];
    size_t rowBytes = static_cast<size_t>(w) * 2;
    size_t need = rowBytes * h;
    if (f.capacity < need) {
        if (f.bits) {
            VirtualFree(f.bits, 0, MEM_RELEASE);
        }
        f.bits = static_cast<BYTE *>(VirtualAlloc(nullptr, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        f.capacity = f.bits ? need : 0;
        if (!f.bits) {
            return;
        }
    }
    static const int kSubmit = core::diag::perf::Register("copy to the output", core::diag::perf::Thread::Game);
    core::diag::perf::Scope timed(kSubmit);
    f.seq.fetch_add(1, std::memory_order_acq_rel); // odd: being written
    const BYTE *src = static_cast<const BYTE *>(pixels);
    if (pitch == static_cast<int>(rowBytes)) {
        std::memcpy(f.bits, src, need);
    }
    else {
        for (int y = 0; y < h; ++y) {
            std::memcpy(f.bits + rowBytes * y, src + static_cast<size_t>(pitch) * y, rowBytes);
        }
    }
    f.w = w;
    f.h = h;
    f.pitch = static_cast<int>(rowBytes);
    f.dst = dst;
    f.client = client;
    f.framing = framing;
    f.seq.fetch_add(1, std::memory_order_release); // even: complete
    g_write = g_ready.exchange(g_write | kNew) & 3;
    SetEvent(g_frameEvent);
}

bool SetPresentMode(core_present_mode mode)
{
    if (!g_threaded || mode == CORE_PRESENT_ORIGINAL) {
        return false;
    }
    g_wantedMode = mode;
    SetEvent(g_frameEvent); // switch before the next frame
    return true;
}

bool FrameWanted()
{
    // Called on the game thread, like Submit, so g_holdUntil needs no synchronisation.
    if (!g_threaded || static_cast<LONG>(g_holdUntil - GetTickCount()) > 0) {
        return false;
    }
    return g_wanted.exchange(false);
}

void HoldNextFrame(DWORD holdMs)
{
    g_holdMs = holdMs;
    g_holdNext = true;
}

void TakeCounts(long *presented, long *shown)
{
    *presented = g_presented.exchange(0);
    *shown = g_shown.exchange(0);
}

int RefreshHz()
{
    return g_refreshHz;
}

void SetBackground(int screenW, int screenH, int pictureW, int pictureH, bool native, const void *outer, int outerPitch,
                   const void *frame, int framePitch)
{
    core::display::FrameLayout l{};
    if (!core::display::ComputeFrameLayout(screenW, screenH, pictureW, pictureH, &l, native)) {
        return;
    }
    const size_t outerRow = static_cast<size_t>(l.outerU) * 2, frameRow = static_cast<size_t>(l.frameU) * 2;
    auto *o = static_cast<BYTE *>(std::malloc(outerRow * l.outerV));
    auto *f = static_cast<BYTE *>(std::malloc(frameRow * l.frameV));
    if (!o || !f) {
        std::free(o);
        std::free(f);
        core_log("output: no memory for the background");
        return;
    }
    for (int y = 0; y < l.outerV; ++y) {
        std::memcpy(o + outerRow * y, static_cast<const BYTE *>(outer) + static_cast<size_t>(outerPitch) * y, outerRow);
    }
    for (int y = 0; y < l.frameV; ++y) {
        std::memcpy(f + frameRow * y, static_cast<const BYTE *>(frame) + static_cast<size_t>(framePitch) * y, frameRow);
    }
    AcquireSRWLockExclusive(&g_bgLock);
    std::free(g_bg.outer);
    std::free(g_bg.frame);
    g_bg.outer = o;
    g_bg.frame = f;
    g_bg.screenW = screenW;
    g_bg.screenH = screenH;
    g_bg.pictureW = pictureW;
    g_bg.pictureH = pictureH;
    g_bg.native = native ? 1 : 0;
    g_bg.outerU = l.outerU;
    g_bg.outerV = l.outerV;
    g_bg.frameU = l.frameU;
    g_bg.frameV = l.frameV;
    ++g_bg.version;
    ReleaseSRWLockExclusive(&g_bgLock);
}

void SetCaption(const char *utf8, int frameY)
{
    AcquireSRWLockExclusive(&g_captionLock);
    std::snprintf(g_caption, sizeof(g_caption), "%s", utf8 ? utf8 : "");
    g_captionY = frameY;
    ReleaseSRWLockExclusive(&g_captionLock);
}

void RequestScreenCapture(const char *stem, const char *time)
{
    AcquireSRWLockExclusive(&g_captureLock);
    std::snprintf(g_captureStem, sizeof(g_captureStem), "%s", stem);
    std::snprintf(g_captureTime, sizeof(g_captureTime), "%s", time);
    ReleaseSRWLockExclusive(&g_captureLock);
    g_captureWanted = true;
}

} // namespace core::output
