// Our own DirectDraw; see ddraw_emu.h. Logging follows ddraw_log (first calls of each method in detail, then
// counters). Built with -fno-exceptions, so no C++ exception can cross into the game.
#include "core/ddraw/ddraw_emu.h"

#include "core/config.h"
#include "core/ddraw/ddraw_log.h"
#include "core/ddraw/ddraw_wrap.h"
#include "core/ddraw/surface_memory.h"
#include "core/diag/guard.h"
#include "core/diag/perf.h"
#include "core/display/virtual_display.h"
#include "core/log.h"

#include <ddraw.h>

#include <atomic>
#include <cstring>
#include <unordered_set>

namespace {

using namespace core::ddlog;

std::atomic<unsigned> g_nextId{1};
std::atomic<long> g_liveDirectDraw{0};

DDPIXELFORMAT Pf565()
{
    DDPIXELFORMAT pf{};
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB;
    pf.dwRGBBitCount = 16;
    pf.dwRBitMask = 0xF800;
    pf.dwGBitMask = 0x07E0;
    pf.dwBBitMask = 0x001F;
    return pf;
}

void LogUnimplemented(const char *iface, unsigned id, const char *method, void *caller)
{
    char where[MAX_PATH + 40];
    core_log("UNIMPLEMENTED %s#%u.%s called -> DDERR_UNSUPPORTED  caller=%s", iface, id, method,
             core_describe_address(caller, where, sizeof(where)));
}

// Body of every method we do not implement.
#define UNIMPLEMENTED(IFACE, NAME)                                                                                   \
    do {                                                                                                             \
        TRACE_STAT(IFACE, NAME);                                                                                     \
        if (Hit(s_stat, DDERR_UNSUPPORTED)) {                                                                        \
            LogUnimplemented(IFACE, id_, NAME, CALLER());                                                            \
        }                                                                                                            \
        return DDERR_UNSUPPORTED;                                                                                    \
    } while (0)

void LogOnce(bool &done, const char *what)
{
    if (!done) {
        done = true;
        core_log("emulated ddraw: %s (logged once)", what);
    }
}

// ---------------------------------------------------------------------------------------------
// Surface

class EmuSurface;
SRWLOCK g_surfacesLock = SRWLOCK_INIT;
std::unordered_set<EmuSurface *> &Surfaces()
{
    static std::unordered_set<EmuSurface *> s;
    return s;
}

class EmuSurface final : public IDirectDrawSurface {
public:
    EmuSurface(bool primary, LONG w, LONG h, const DDPIXELFORMAT &pf, DWORD caps)
        : id_(g_nextId++), primary_(primary), w_(w), h_(h), pitch_((w * 2 + 3) & ~3), pf_(pf), caps_(caps)
    {
        // Plain committed memory: the game keeps drawing through the pointer it got from the first Lock after
        // Unlock (dd_create_surfaces stores it), so the bits must never move while the surface lives. Spare rows
        // with a canary below the last row and guard pages around (core/ddraw/surface_memory.h, R2b.1).
        if (core::ddraw::AllocSurfaceMemory(mem_, pitch_, h_)) {
            bits_ = mem_.bits;
            core_log("emulated ddraw: Surface#%u memory: %ldx%ld pitch %ld at %p, %d spare rows (%zu bytes, canary) "
                     "at %p, guard pages before and after",
                     id_, w_, h_, pitch_, static_cast<void *>(bits_), core::ddraw::kSpareRows, mem_.spareBytes,
                     static_cast<void *>(mem_.Spare()));
        }
        AcquireSRWLockExclusive(&g_surfacesLock);
        Surfaces().insert(this);
        ReleaseSRWLockExclusive(&g_surfacesLock);
    }
    ~EmuSurface()
    {
        AcquireSRWLockExclusive(&g_surfacesLock);
        Surfaces().erase(this);
        ReleaseSRWLockExclusive(&g_surfacesLock);
        core::display::FrameGone(bits_);
        core::ddraw::FreeSurfaceMemory(mem_);
        bits_ = nullptr;
    }
    bool ok() const { return bits_ != nullptr; }
    unsigned id() const { return id_; }

    // The surface behind a pointer passed by the game (nullptr if it is not one of ours).
    static EmuSurface *From(IDirectDrawSurface *p)
    {
        if (!p) {
            return nullptr;
        }
        AcquireSRWLockShared(&g_surfacesLock);
        bool ours = Surfaces().count(static_cast<EmuSurface *>(p)) != 0;
        ReleaseSRWLockShared(&g_surfacesLock);
        return ours ? static_cast<EmuSurface *>(p) : nullptr;
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void **out) override
    {
        TRACE_STAT("Surface", "QueryInterface");
        HRESULT hr = E_NOINTERFACE;
        if (out) {
            *out = nullptr;
            if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectDrawSurface)) {
                AddRefQuiet();
                *out = static_cast<IDirectDrawSurface *>(this);
                hr = S_OK;
            }
        }
        if (Hit(s_stat, hr) || FAILED(hr)) {
            Buf a;
            AddGuid(a, riid);
            LogCall("Surface", id_, "QueryInterface", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD_(ULONG, AddRef)() override
    {
        TRACE_STAT("Surface", "AddRef");
        ULONG r = AddRefQuiet();
        if (Hit(s_stat, S_OK)) {
            char where[MAX_PATH + 40];
            core_log("Surface#%u.AddRef() -> %lu  caller=%s", id_, r, core_describe_address(CALLER(), where, sizeof(where)));
        }
        return r;
    }
    STDMETHOD_(ULONG, Release)() override
    {
        TRACE_STAT("Surface", "Release");
        ULONG r = --refs_;
        if (Hit(s_stat, S_OK)) {
            char where[MAX_PATH + 40];
            core_log("Surface#%u.Release() -> %lu%s  caller=%s", id_, r, r == 0 ? " (freed)" : "",
                     core_describe_address(CALLER(), where, sizeof(where)));
        }
        if (r == 0) {
            delete this;
        }
        return r;
    }

    // Implemented
    STDMETHOD(Blt)(LPRECT dst, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) override
    {
        TRACE_STAT("Surface", "Blt");
        EmuSurface *s = From(src);
        RECT d = dst ? *dst : Full();
        HRESULT hr = DD_OK;
        if (flags & DDBLT_COLORFILL) {
            Fill(d, fx ? static_cast<WORD>(fx->dwFillColor) : 0);
        }
        else if (s) {
            static bool keyNote = false, ropNote = false;
            if (flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE | DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE)) {
                LogOnce(keyNote, "Blt with a color key requested; copied without the key");
            }
            if ((flags & DDBLT_ROP) && fx && fx->dwROP != SRCCOPY) {
                LogOnce(ropNote, "Blt with a raster operation other than SRCCOPY; copied as SRCCOPY");
            }
            RECT sr = srcRect ? *srcRect : s->Full();
            if (sr.right - sr.left == d.right - d.left && sr.bottom - sr.top == d.bottom - d.top) {
                Copy(d, *s, sr);
            }
            else {
                Stretch(d, *s, sr);
            }
        }
        else {
            hr = DDERR_INVALIDPARAMS;
        }
        if (SUCCEEDED(hr) && primary_) {
            RECT sr = s ? (srcRect ? *srcRect : s->Full()) : d;
            PresentFrame(s ? "Blt" : "Blt(colorfill)", s ? s->id_ : id_, s ? s->Size() : Size(), sr, d, CALLER());
        }
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("dst=");
            AddRect(a, dst);
            a.add(" src=%s#%u srcRect=", src ? "Surface" : "NULL", s ? s->id_ : 0);
            AddRect(a, srcRect);
            a.add(" flags=0x%lx", flags);
            if (fx) a.add(" fill=0x%lx", fx->dwFillColor);
            LogCall("Surface", id_, "Blt", a, hr, CALLER());
        }
        MaybePeriodicSummary();
        return hr;
    }
    STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD trans) override
    {
        TRACE_STAT("Surface", "BltFast");
        EmuSurface *s = From(src);
        HRESULT hr = DD_OK;
        if (!s) {
            hr = DDERR_INVALIDPARAMS;
        }
        else {
            static bool keyNote = false;
            if (trans & (DDBLTFAST_SRCCOLORKEY | DDBLTFAST_DESTCOLORKEY)) {
                LogOnce(keyNote, "BltFast with a color key requested; copied without the key");
            }
            RECT sr = srcRect ? *srcRect : s->Full();
            RECT d{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x) + sr.right - sr.left,
                   static_cast<LONG>(y) + sr.bottom - sr.top};
            Copy(d, *s, sr);
            if (primary_) {
                PresentFrame("BltFast", s->id_, s->Size(), sr, d, CALLER());
            }
        }
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("x=%lu y=%lu src=Surface#%u srcRect=", x, y, s ? s->id_ : 0);
            AddRect(a, srcRect);
            a.add(" trans=0x%lx", trans);
            LogCall("Surface", id_, "BltFast", a, hr, CALLER());
        }
        MaybePeriodicSummary();
        return hr;
    }
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT pf) override
    {
        TRACE_STAT("Surface", "GetPixelFormat");
        HRESULT hr = pf ? DD_OK : DDERR_INVALIDPARAMS;
        if (pf) {
            *pf = pf_;
        }
        if (Hit(s_stat, hr)) {
            Buf a, o;
            AddPixelFormat(o, pf_);
            LogCall("Surface", id_, "GetPixelFormat", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC d) override
    {
        TRACE_STAT("Surface", "GetSurfaceDesc");
        HRESULT hr = d ? DD_OK : DDERR_INVALIDPARAMS;
        if (d) {
            Describe(d, nullptr);
        }
        if (Hit(s_stat, hr)) {
            Buf a, o;
            AddDesc(o, d);
            LogCall("Surface", id_, "GetSurfaceDesc", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(IsLost)() override
    {
        TRACE_STAT("Surface", "IsLost");
        CountIsLost();
        if (Hit(s_stat, DD_OK)) {
            Buf a;
            LogCall("Surface", id_, "IsLost", a, DD_OK, CALLER());
        }
        return DD_OK; // memory surfaces are never lost
    }
    STDMETHOD(Lock)(LPRECT r, LPDDSURFACEDESC d, DWORD flags, HANDLE ev) override
    {
        TRACE_STAT("Surface", "Lock");
        (void)ev;
        HRESULT hr = d ? DD_OK : DDERR_INVALIDPARAMS;
        if (d) {
            Describe(d, r);
            ++locks_;
        }
        if (Hit(s_stat, hr)) {
            Buf a, o;
            a.add("rect=");
            AddRect(a, r);
            a.add(" flags=0x%lx", flags);
            AddDesc(o, d);
            LogCall("Surface", id_, "Lock", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(Restore)() override
    {
        TRACE_STAT("Surface", "Restore");
        if (Hit(s_stat, DD_OK)) {
            Buf a;
            LogCall("Surface", id_, "Restore", a, DD_OK, CALLER());
        }
        return DD_OK;
    }
    STDMETHOD(Unlock)(LPVOID data) override
    {
        TRACE_STAT("Surface", "Unlock");
        if (locks_ > 0) {
            --locks_;
        }
        // Bink and the game write straight into the locked primary: show what they wrote.
        if (primary_) {
            RECT f = Full();
            PresentFrame("Unlock", id_, Size(), f, f, CALLER());
        }
        if (Hit(s_stat, DD_OK)) {
            Buf a;
            a.add("data=%p", data);
            LogCall("Surface", id_, "Unlock", a, DD_OK, CALLER());
        }
        return DD_OK;
    }

    // Not implemented (not used by the game)
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE) override { UNIMPLEMENTED("Surface", "AddAttachedSurface"); }
    STDMETHOD(AddOverlayDirtyRect)(LPRECT) override { UNIMPLEMENTED("Surface", "AddOverlayDirtyRect"); }
    STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD) override { UNIMPLEMENTED("Surface", "BltBatch"); }
    STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE) override
    {
        UNIMPLEMENTED("Surface", "DeleteAttachedSurface");
    }
    STDMETHOD(EnumAttachedSurfaces)(LPVOID, LPDDENUMSURFACESCALLBACK) override
    {
        UNIMPLEMENTED("Surface", "EnumAttachedSurfaces");
    }
    STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override
    {
        UNIMPLEMENTED("Surface", "EnumOverlayZOrders");
    }
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE, DWORD) override { UNIMPLEMENTED("Surface", "Flip"); }
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS, LPDIRECTDRAWSURFACE *) override
    {
        UNIMPLEMENTED("Surface", "GetAttachedSurface");
    }
    STDMETHOD(GetBltStatus)(DWORD) override { UNIMPLEMENTED("Surface", "GetBltStatus"); }
    STDMETHOD(GetCaps)(LPDDSCAPS) override { UNIMPLEMENTED("Surface", "GetCaps"); }
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER *) override { UNIMPLEMENTED("Surface", "GetClipper"); }
    STDMETHOD(GetColorKey)(DWORD, LPDDCOLORKEY) override { UNIMPLEMENTED("Surface", "GetColorKey"); }
    STDMETHOD(GetDC)(HDC *) override { UNIMPLEMENTED("Surface", "GetDC"); }
    STDMETHOD(GetFlipStatus)(DWORD) override { UNIMPLEMENTED("Surface", "GetFlipStatus"); }
    STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG) override { UNIMPLEMENTED("Surface", "GetOverlayPosition"); }
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE *) override { UNIMPLEMENTED("Surface", "GetPalette"); }
    STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC) override { UNIMPLEMENTED("Surface", "Initialize"); }
    STDMETHOD(ReleaseDC)(HDC) override { UNIMPLEMENTED("Surface", "ReleaseDC"); }
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER) override { UNIMPLEMENTED("Surface", "SetClipper"); }
    STDMETHOD(SetColorKey)(DWORD, LPDDCOLORKEY) override { UNIMPLEMENTED("Surface", "SetColorKey"); }
    STDMETHOD(SetOverlayPosition)(LONG, LONG) override { UNIMPLEMENTED("Surface", "SetOverlayPosition"); }
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE) override { UNIMPLEMENTED("Surface", "SetPalette"); }
    STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) override
    {
        UNIMPLEMENTED("Surface", "UpdateOverlay");
    }
    STDMETHOD(UpdateOverlayDisplay)(DWORD) override { UNIMPLEMENTED("Surface", "UpdateOverlayDisplay"); }
    STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE) override
    {
        UNIMPLEMENTED("Surface", "UpdateOverlayZOrder");
    }

private:
    ULONG AddRefQuiet() { return ++refs_; }
    RECT Full() const { return RECT{0, 0, w_, h_}; }
    SIZE Size() const { return SIZE{w_, h_}; }
    WORD *Row(LONG y) const { return reinterpret_cast<WORD *>(bits_ + static_cast<SIZE_T>(y) * pitch_); }

    void Describe(LPDDSURFACEDESC d, const RECT *r) const
    {
        DWORD size = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
        std::memset(d, 0, sizeof(DDSURFACEDESC));
        d->dwSize = size;
        d->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
        d->dwWidth = w_;
        d->dwHeight = h_;
        d->lPitch = pitch_;
        d->ddpfPixelFormat = pf_;
        d->ddsCaps.dwCaps = caps_;
        if (bits_) {
            LONG x = r ? r->left : 0, y = r ? r->top : 0;
            d->lpSurface = bits_ + static_cast<SIZE_T>(y) * pitch_ + x * 2;
        }
    }

    void Fill(RECT d, WORD c)
    {
        RECT bounds = Full();
        IntersectRect(&d, &d, &bounds);
        for (LONG y = d.top; y < d.bottom; ++y) {
            WORD *row = Row(y);
            for (LONG x = d.left; x < d.right; ++x) {
                row[x] = c;
            }
        }
    }

    // Same-size copy, clipped against both surfaces; handles overlap within one surface.
    void Copy(RECT d, const EmuSurface &s, RECT sr)
    {
        LONG shift;
        if ((shift = -sr.left) > 0) { sr.left += shift; d.left += shift; }
        if ((shift = -sr.top) > 0) { sr.top += shift; d.top += shift; }
        if ((shift = sr.right - s.w_) > 0) { sr.right -= shift; d.right -= shift; }
        if ((shift = sr.bottom - s.h_) > 0) { sr.bottom -= shift; d.bottom -= shift; }
        if ((shift = -d.left) > 0) { d.left += shift; sr.left += shift; }
        if ((shift = -d.top) > 0) { d.top += shift; sr.top += shift; }
        if ((shift = d.right - w_) > 0) { d.right -= shift; sr.right -= shift; }
        if ((shift = d.bottom - h_) > 0) { d.bottom -= shift; sr.bottom -= shift; }
        LONG w = d.right - d.left, h = d.bottom - d.top;
        if (w <= 0 || h <= 0) {
            return;
        }
        bool upward = &s == this && d.top > sr.top;
        for (LONG i = 0; i < h; ++i) {
            LONG k = upward ? h - 1 - i : i;
            std::memmove(Row(d.top + k) + d.left, s.Row(sr.top + k) + sr.left, static_cast<size_t>(w) * 2);
        }
    }

    // Nearest-neighbour stretch (never seen from the game; kept simple).
    void Stretch(RECT d, const EmuSurface &s, RECT sr)
    {
        LONG dw = d.right - d.left, dh = d.bottom - d.top, sw = sr.right - sr.left, sh = sr.bottom - sr.top;
        if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) {
            return;
        }
        for (LONG y = d.top < 0 ? 0 : d.top; y < d.bottom && y < h_; ++y) {
            LONG syy = sr.top + (y - d.top) * sh / dh;
            if (syy < 0 || syy >= s.h_) continue;
            WORD *row = Row(y);
            const WORD *srow = s.Row(syy);
            for (LONG x = d.left < 0 ? 0 : d.left; x < d.right && x < w_; ++x) {
                LONG sx = sr.left + (x - d.left) * sw / dw;
                if (sx >= 0 && sx < s.w_) row[x] = srow[sx];
            }
        }
    }

    void PresentFrame(const char *how, unsigned srcId, SIZE srcSize, const RECT &sr, const RECT &dr, void *caller)
    {
        NotePresent(how, srcId, srcSize, sr, id_, Size(), dr, caller);
        CheckCanaries();
        core::display::Present(bits_, pitch_, w_, h_, how);
    }

    // Every frame: has anything been written below the last row of a surface (its spare rows)?
    static void CheckCanaries();
    void CheckOwnCanary(DWORD now);

    std::atomic<ULONG> refs_{1};
    unsigned id_;
    bool primary_;
    LONG w_, h_, pitch_;
    DDPIXELFORMAT pf_;
    DWORD caps_;
    BYTE *bits_ = nullptr;
    core::ddraw::SurfaceMemory mem_;
    long locks_ = 0;
};

// Canary reports: the first kCanaryDetailed hits in full; after that one summary line at most every 10 s.
constexpr long kCanaryDetailed = 10;
long g_canaryHits = 0, g_canaryUnreported = 0;
DWORD g_canarySummaryAt = 0;
core::diag::Feature g_canaryFeature{"surface canary check (core/ddraw)"};

void EmuSurface::CheckOwnCanary(DWORD now)
{
    core::ddraw::CanaryDamage d;
    if (core::ddraw::CheckCanary(mem_, &d)) {
        return;
    }
    if (++g_canaryHits <= kCanaryDetailed) {
        char state[256];
        core::ddraw::DescribeState(state, sizeof(state));
        core_log("CANARY: Surface#%u (%ldx%ld) written below its last row: rows %d..%d (%d..%d past the end), %zu "
                 "byte(s) changed; %s%s",
                 id_, w_, h_, d.firstRow, d.lastRow, d.firstRow - static_cast<int>(h_) + 1, d.lastRow - static_cast<int>(h_) + 1, d.bytes,
                 state[0] ? state : "no game state", g_canaryHits == kCanaryDetailed ? " (further hits: summary only)" : "");
        return;
    }
    ++g_canaryUnreported;
    if (now - g_canarySummaryAt >= 10000) {
        g_canarySummaryAt = now;
        core_log("CANARY: %ld more hit(s) since the last report (%ld in all)", g_canaryUnreported, g_canaryHits);
        g_canaryUnreported = 0;
    }
}

void EmuSurface::CheckCanaries()
{
    static const int kPhase = core::diag::perf::Register("canary check", core::diag::perf::Thread::Game);
    core::diag::perf::Scope timed(kPhase);
    core::diag::Guarded(
        g_canaryFeature,
        [](void *) {
            // A copy of the list, checked outside the lock (an abandoned check must not leave it held). Surfaces
            // are created, released and presented on the game thread, so none goes away meanwhile.
            EmuSurface *list[16];
            int n = 0;
            AcquireSRWLockShared(&g_surfacesLock);
            for (EmuSurface *s : Surfaces()) {
                if (n < 16) {
                    list[n++] = s;
                }
            }
            ReleaseSRWLockShared(&g_surfacesLock);
            const DWORD now = GetTickCount();
            for (int i = 0; i < n; ++i) {
                list[i]->CheckOwnCanary(now);
            }
        },
        nullptr);
}

// ---------------------------------------------------------------------------------------------
// DirectDraw

class EmuDirectDraw final : public IDirectDraw {
public:
    EmuDirectDraw() : id_(g_nextId++) { ++g_liveDirectDraw; }
    unsigned id() const { return id_; }

    STDMETHOD(QueryInterface)(REFIID riid, void **out) override
    {
        TRACE_STAT("DirectDraw", "QueryInterface");
        HRESULT hr = E_NOINTERFACE;
        if (out) {
            *out = nullptr;
            if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectDraw)) {
                ++refs_;
                *out = static_cast<IDirectDraw *>(this);
                hr = S_OK;
            }
        }
        if (Hit(s_stat, hr) || FAILED(hr)) {
            Buf a;
            AddGuid(a, riid);
            LogCall("DirectDraw", id_, "QueryInterface", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD_(ULONG, AddRef)() override
    {
        TRACE_STAT("DirectDraw", "AddRef");
        ULONG r = ++refs_;
        if (Hit(s_stat, S_OK)) {
            char where[MAX_PATH + 40];
            core_log("DirectDraw#%u.AddRef() -> %lu  caller=%s", id_, r,
                     core_describe_address(CALLER(), where, sizeof(where)));
        }
        return r;
    }
    STDMETHOD_(ULONG, Release)() override
    {
        TRACE_STAT("DirectDraw", "Release");
        ULONG r = --refs_;
        if (Hit(s_stat, S_OK)) {
            char where[MAX_PATH + 40];
            core_log("DirectDraw#%u.Release() -> %lu%s  caller=%s", id_, r, r == 0 ? " (freed)" : "",
                     core_describe_address(CALLER(), where, sizeof(where)));
        }
        if (r == 0) {
            // Like the real ddraw: releasing the object that set the mode restores the display.
            if (modeSet_) {
                core::display::ClearMode();
            }
            if (--g_liveDirectDraw == 0) {
                core_ddraw_write_summary("last IDirectDraw released");
            }
            delete this;
        }
        return r;
    }

    STDMETHOD(CreateSurface)(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE *out, IUnknown *outer) override
    {
        TRACE_STAT("DirectDraw", "CreateSurface");
        HRESULT hr = DD_OK;
        EmuSurface *s = nullptr;
        if (!d || !out || outer || !(d->dwFlags & DDSD_CAPS)) {
            hr = DDERR_INVALIDPARAMS;
        }
        else if ((d->dwFlags & DDSD_BACKBUFFERCOUNT) ||
                 (d->ddsCaps.dwCaps & (DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_OVERLAY | DDSCAPS_TEXTURE))) {
            hr = DDERR_UNSUPPORTED; // flipping chains, overlays, textures: not used by the game
        }
        else if (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
            SIZE m = core::display::ModeSize();
            s = new EmuSurface(true, m.cx, m.cy, Pf565(),
                               DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
        }
        else if ((d->dwFlags & (DDSD_WIDTH | DDSD_HEIGHT)) != (DDSD_WIDTH | DDSD_HEIGHT) || d->dwWidth == 0 ||
                 d->dwHeight == 0 || d->dwWidth > 16384 || d->dwHeight > 16384) {
            hr = DDERR_INVALIDPARAMS;
        }
        else {
            DDPIXELFORMAT pf = (d->dwFlags & DDSD_PIXELFORMAT) ? d->ddpfPixelFormat : Pf565();
            if (!(pf.dwFlags & DDPF_RGB) || pf.dwRGBBitCount != 16) {
                hr = DDERR_INVALIDPIXELFORMAT;
            }
            else {
                static bool note555 = false;
                if (pf.dwGBitMask != 0x07E0) {
                    LogOnce(note555, "16-bit surface with masks other than 565 requested; shown as 565");
                }
                DWORD caps = d->ddsCaps.dwCaps;
                if (!(caps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY))) {
                    caps |= DDSCAPS_SYSTEMMEMORY;
                }
                s = new EmuSurface(false, static_cast<LONG>(d->dwWidth), static_cast<LONG>(d->dwHeight), pf, caps);
            }
        }
        if (s && !s->ok()) {
            delete s;
            s = nullptr;
            hr = DDERR_OUTOFMEMORY;
        }
        if (out) {
            *out = s;
        }
        if (Hit(s_stat, hr) || FAILED(hr)) {
            Buf a, o;
            AddDesc(a, d);
            if (s) o.add("-> Surface#%u", s->id());
            LogCall("DirectDraw", id_, "CreateSurface", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC d) override
    {
        TRACE_STAT("DirectDraw", "GetDisplayMode");
        HRESULT hr = d ? DD_OK : DDERR_INVALIDPARAMS;
        if (d) {
            DWORD size = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
            std::memset(d, 0, sizeof(DDSURFACEDESC));
            d->dwSize = size;
            d->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            SIZE m = core::display::ModeSize();
            d->dwWidth = m.cx;
            d->dwHeight = m.cy;
            d->dwRefreshRate = 60;
            if (core::display::ModeActive()) {
                d->ddpfPixelFormat = Pf565();
                d->lPitch = m.cx * 2;
            }
            else {
                // No emulated mode: describe the real desktop (32-bit on any current system).
                d->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
                d->ddpfPixelFormat.dwFlags = DDPF_RGB;
                d->ddpfPixelFormat.dwRGBBitCount = 32;
                d->ddpfPixelFormat.dwRBitMask = 0xFF0000;
                d->ddpfPixelFormat.dwGBitMask = 0x00FF00;
                d->ddpfPixelFormat.dwBBitMask = 0x0000FF;
                d->lPitch = m.cx * 4;
            }
        }
        if (Hit(s_stat, hr)) {
            Buf a, o;
            AddDesc(o, d);
            LogCall("DirectDraw", id_, "GetDisplayMode", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(RestoreDisplayMode)() override
    {
        TRACE_STAT("DirectDraw", "RestoreDisplayMode");
        if (modeSet_) {
            core::display::ClearMode();
            modeSet_ = false;
        }
        if (Hit(s_stat, DD_OK)) {
            Buf a;
            LogCall("DirectDraw", id_, "RestoreDisplayMode", a, DD_OK, CALLER());
        }
        return DD_OK;
    }
    STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD flags) override
    {
        TRACE_STAT("DirectDraw", "SetCooperativeLevel");
        exclusive_ = (flags & DDSCL_EXCLUSIVE) && (flags & DDSCL_FULLSCREEN) && hwnd;
        if (exclusive_) {
            core::display::Attach(hwnd, core_config_get()->scaling, core_config_get()->present);
        }
        if (Hit(s_stat, DD_OK)) {
            Buf a;
            a.add("hwnd=%p flags=0x%lx%s", static_cast<void *>(hwnd), flags,
                  exclusive_ ? " (emulated fullscreen)" : "");
            LogCall("DirectDraw", id_, "SetCooperativeLevel", a, DD_OK, CALLER());
        }
        return DD_OK;
    }
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp) override
    {
        TRACE_STAT("DirectDraw", "SetDisplayMode");
        HRESULT hr = DD_OK;
        if (!exclusive_) {
            hr = DDERR_NOEXCLUSIVEMODE;
        }
        else if (bpp != 16 || w < 320 || h < 200 || w > 8192 || h > 8192) {
            hr = DDERR_INVALIDMODE; // only the 16-bit modes the game uses
        }
        else {
            core::display::SetMode(static_cast<int>(w), static_cast<int>(h));
            modeSet_ = true;
        }
        if (Hit(s_stat, hr) || FAILED(hr)) {
            Buf a;
            a.add("%lux%lux%lu", w, h, bpp);
            LogCall("DirectDraw", id_, "SetDisplayMode", a, hr, CALLER());
        }
        return hr;
    }

    STDMETHOD(Compact)() override { UNIMPLEMENTED("DirectDraw", "Compact"); }
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER *, IUnknown *) override
    {
        static bool note = false;
        LogOnce(note, "CreateClipper: the game asked for windowed mode, which the emulated back end does not support "
                      "(set display.backend=system in emperor_engine.ini to use the real ddraw)");
        UNIMPLEMENTED("DirectDraw", "CreateClipper");
    }
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE *, IUnknown *) override
    {
        UNIMPLEMENTED("DirectDraw", "CreatePalette");
    }
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE *) override
    {
        UNIMPLEMENTED("DirectDraw", "DuplicateSurface");
    }
    STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMMODESCALLBACK) override
    {
        UNIMPLEMENTED("DirectDraw", "EnumDisplayModes");
    }
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override
    {
        UNIMPLEMENTED("DirectDraw", "EnumSurfaces");
    }
    STDMETHOD(FlipToGDISurface)() override { UNIMPLEMENTED("DirectDraw", "FlipToGDISurface"); }
    STDMETHOD(GetCaps)(LPDDCAPS, LPDDCAPS) override { UNIMPLEMENTED("DirectDraw", "GetCaps"); }
    STDMETHOD(GetFourCCCodes)(LPDWORD, LPDWORD) override { UNIMPLEMENTED("DirectDraw", "GetFourCCCodes"); }
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE *) override { UNIMPLEMENTED("DirectDraw", "GetGDISurface"); }
    STDMETHOD(GetMonitorFrequency)(LPDWORD) override { UNIMPLEMENTED("DirectDraw", "GetMonitorFrequency"); }
    STDMETHOD(GetScanLine)(LPDWORD) override { UNIMPLEMENTED("DirectDraw", "GetScanLine"); }
    STDMETHOD(GetVerticalBlankStatus)(WINBOOL *) override { UNIMPLEMENTED("DirectDraw", "GetVerticalBlankStatus"); }
    STDMETHOD(Initialize)(GUID *) override { UNIMPLEMENTED("DirectDraw", "Initialize"); }
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) override { UNIMPLEMENTED("DirectDraw", "WaitForVerticalBlank"); }

private:
    std::atomic<ULONG> refs_{1};
    unsigned id_;
    bool exclusive_ = false;
    bool modeSet_ = false;
};

} // namespace

extern "C" HRESULT core_ddraw_emu_create(GUID *guid, void **dd, IUnknown *outer, void *caller)
{
    char where[MAX_PATH + 40];
    core_describe_address(caller, where, sizeof(where));
    if (!dd || outer) {
        core_log("DirectDrawCreate(emulated, guid=%p, outer=%p) -> DDERR_INVALIDPARAMS  caller=%s",
                 static_cast<void *>(guid), static_cast<void *>(outer), where);
        return DDERR_INVALIDPARAMS;
    }
    EmuDirectDraw *d = new EmuDirectDraw();
    *dd = static_cast<IDirectDraw *>(d);
    core_log("DirectDrawCreate(emulated, guid=%p) -> DD_OK, DirectDraw#%u  caller=%s", static_cast<void *>(guid),
             d->id(), where);
    return DD_OK;
}
