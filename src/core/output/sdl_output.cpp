#include "core/diag/perf.h"
#include "core/diag/report.h"
#include "core/ui/perf_view.h"
#include "core/output/sdl_output.h"

#include "core/capture/screenshot.h"
#include "core/diag/guard.h"
#include "core/display/framing.h"
#include "core/log.h"
#include "core/ui/panel.h"

#include <SDL3/SDL.h>

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
    SDL_Surface *s = SDL_RenderReadPixels(g_renderer, nullptr);
    if (!s) {
        core_log("screenshot: reading back the %dx%d screen failed: %s", ow, oh, SDL_GetError());
        return;
    }
    core::capture::SaveScreen(s, stem, time);
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
    if (!SDL_SetTextureScaleMode(g_texture, mode)) {
        core_log("output: scale mode %s not supported (%s); using linear", name, SDL_GetError());
        SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_LINEAR);
        name = "linear (fallback)";
    }
    core_log("output: texture %dx%d RGB565, scaling %s", w, h, name);
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
    const char *wanted = core_renderer_names[core_config_get()->renderer];
    g_renderer = SDL_CreateRenderer(g_window, wanted);
    if (!g_renderer) {
        core_log("output: %s renderer failed (%s); trying direct3d11, then SDL's default", wanted, SDL_GetError());
        g_renderer = SDL_CreateRenderer(g_window, "direct3d11");
        if (!g_renderer) {
            g_renderer = SDL_CreateRenderer(g_window, nullptr);
        }
    }
    if (!g_renderer) {
        Fail("SDL_CreateRenderer");
        return false;
    }
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
    int refresh = dm ? static_cast<int>(dm->refresh_rate + 0.5f) : 0;
    g_refreshHz = refresh;
    g_vsync = VsyncFor(refresh);
    if (!SDL_SetRenderVSync(g_renderer, g_vsync)) {
        core_log("output: vsync %d not supported (%s); using 1", g_vsync, SDL_GetError());
        g_vsync = 1;
        SDL_SetRenderVSync(g_renderer, 1);
    }
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
    if (first) {
        static const char *const kMode[] = {"original (game thread)", "display (output thread)",
                                            "cap60 (output thread)"};
        int v = SDL_GetVersion();
        core_log("output: SDL %d.%d.%d, video driver %s, renderer %s, output %dx%d, display %d Hz, present %s, "
                 "vsync interval %d; window procedure %s",
                 SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v),
                 SDL_GetCurrentVideoDriver(), SDL_GetRendererName(g_renderer), ow, oh, refresh, kMode[g_mode],
                 g_vsync, restored ? "given back to the game after SDL's subclass" : "was not subclassed by SDL");
    }
    else {
        core_log("output: rebuilt for the new window size, output %dx%d", ow, oh);
    }
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
    if (!g_renderer || w <= 0 || h <= 0) {
        return;
    }
    const std::int64_t t0 = perf::Now();
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
    if ((ow != client.cx || oh != client.cy) && client.cx > 0 && client.cy > 0) {
        if (!Rebuild()) {
            return;
        }
        SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
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
    ok = SDL_RenderPresent(g_renderer) && ok;
    perf::Add(kPresent, perf::Now() - t);
    perf::Add(kTotal, perf::Now() - t0);
    perf::EndSample(perf::Thread::Output);
    if (ok) {
        ++g_shown;
    }
    else if (++g_presentErrors <= 16) {
        core_log("output: present failed (%ld so far): %s", g_presentErrors, SDL_GetError());
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
    struct Shot {
        const Frame *f;
        const char *label;
    } shot{&f, label};
    if (core::diag::Guarded(g_shotFeature,
                            [](void *p) {
                                auto *s = static_cast<Shot *>(p);
                                core::capture::Save(s->f->bits, s->f->pitch, s->f->w, s->f->h, s->label);
                            },
                            &shot)) {
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

DWORD WINAPI OutputThread(void *)
{
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
            g_vsync = VsyncFor(g_refreshHz.load());
            bool ok = SDL_SetRenderVSync(g_renderer, g_vsync);
            core_log("output: present %s, vsync interval %d%s", want == CORE_PRESENT_CAP60 ? "cap60" : "display",
                     g_vsync, ok ? "" : " (not supported by the renderer)");
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
