// DirectDraw wrapper: forwards everything to the real ddraw, logs every vtable method. v2 adds the output
// monitor (geometry and rate of every blit onto the primary surface) and window/DPI logs around
// SetCooperativeLevel and SetDisplayMode.
//
// Logging policy and the output monitor live in ddraw_log (shared with our own implementation, ddraw_emu).
//
// Object identity: each real interface gets at most one wrapper at a time. Pointers passed in by the
// game are unwrapped before forwarding; interfaces returned by the real ddraw are wrapped. Interfaces
// obtained through QueryInterface for other IIDs (IDirectDraw2, IDirectDrawSurface3, ...) are returned
// unwrapped and logged as escaped: calls through them are not seen.
//
// Built with -fno-exceptions, so no C++ exception can cross into the game.
#include "core/ddraw/ddraw_wrap.h"

#include "core/ddraw/ddraw_log.h"
#include "core/log.h"
#include "core/platform/dpi.h"

#include <ddraw.h>

#include <atomic>
#include <unordered_map>

namespace {

using namespace core::ddlog;

// ---------------------------------------------------------------------------------------------
// Wrapper registry

SRWLOCK g_registryLock = SRWLOCK_INIT;
std::unordered_map<void *, void *> &RealToWrapper()
{
    static std::unordered_map<void *, void *> m;
    return m;
}
std::unordered_map<void *, void *> &WrapperToReal()
{
    static std::unordered_map<void *, void *> m;
    return m;
}

void *FindWrapper(void *real)
{
    AcquireSRWLockShared(&g_registryLock);
    auto &m = RealToWrapper();
    auto it = m.find(real);
    void *w = it == m.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_registryLock);
    return w;
}

// Returns the real interface for a wrapper, or the pointer itself if it is not one of ours.
template <typename T> T *Unwrap(T *p)
{
    if (!p) {
        return nullptr;
    }
    AcquireSRWLockShared(&g_registryLock);
    auto &m = WrapperToReal();
    auto it = m.find(p);
    T *r = it == m.end() ? p : static_cast<T *>(it->second);
    ReleaseSRWLockShared(&g_registryLock);
    return r;
}

void Register(void *wrapper, void *real)
{
    AcquireSRWLockExclusive(&g_registryLock);
    RealToWrapper()[real] = wrapper;
    WrapperToReal()[wrapper] = real;
    ReleaseSRWLockExclusive(&g_registryLock);
}

void Unregister(void *wrapper, void *real)
{
    AcquireSRWLockExclusive(&g_registryLock);
    RealToWrapper().erase(real);
    WrapperToReal().erase(wrapper);
    ReleaseSRWLockExclusive(&g_registryLock);
}

std::atomic<unsigned> g_nextId{1};
std::atomic<long> g_liveDirectDraw{0};

// ---------------------------------------------------------------------------------------------
// Common base: reference counting mirrors the real object.

template <typename Iface> class WrapBase : public Iface {
public:
    WrapBase(Iface *real, const char *kind) : real_(real), kind_(kind), id_(g_nextId++) { Register(this, real); }
    virtual ~WrapBase() { Unregister(this, real_); }
    Iface *real() const { return real_; }
    unsigned id() const { return id_; }

    // Called when the real ddraw hands out another reference to the same real object.
    void AddWrapperRef() { ++wrapperRefs_; }

protected:
    ULONG DoAddRef(Stat &s, void *caller)
    {
        ULONG r = real_->AddRef();
        ++wrapperRefs_;
        if (Hit(s, S_OK)) {
            char where[MAX_PATH + 40];
            core_log("%s#%u.AddRef() -> %lu  caller=%s", kind_, id_, r,
                     core_describe_address(caller, where, sizeof(where)));
        }
        return r;
    }

    ULONG DoRelease(Stat &s, void *caller)
    {
        ULONG r = real_->Release();
        long left = --wrapperRefs_;
        if (Hit(s, S_OK)) {
            char where[MAX_PATH + 40];
            core_log("%s#%u.Release() -> %lu%s  caller=%s", kind_, id_, r, left == 0 ? " (wrapper freed)" : "",
                     core_describe_address(caller, where, sizeof(where)));
        }
        if (left == 0) {
            delete this;
        }
        return r;
    }

    // QueryInterface: our own IID (or IUnknown) returns this wrapper; anything else escapes unwrapped.
    HRESULT DoQueryInterface(Stat &s, REFIID riid, void **out, REFIID own, void *caller)
    {
        HRESULT hr;
        bool escaped = false;
        if (IsEqualGUID(riid, own) || IsEqualGUID(riid, IID_IUnknown)) {
            hr = real_->QueryInterface(riid, out);
            if (SUCCEEDED(hr) && out) {
                // The real QI added a reference to the real object; account for it on this wrapper.
                ++wrapperRefs_;
                *out = static_cast<Iface *>(this);
            }
        }
        else {
            hr = real_->QueryInterface(riid, out);
            escaped = SUCCEEDED(hr);
        }
        if (Hit(s, hr) || escaped) {
            Buf a;
            AddGuid(a, riid);
            Buf o;
            if (escaped) {
                o.add("ESCAPED unwrapped interface %p (calls through it are not logged)", out ? *out : nullptr);
            }
            LogCall(kind_, id_, "QueryInterface", a, hr, caller, escaped ? &o : nullptr);
        }
        return hr;
    }

    Iface *real_;
    const char *kind_;
    unsigned id_;
    std::atomic<long> wrapperRefs_{1};
};

class SurfaceWrap;
class ClipperWrap;
class PaletteWrap;
class DirectDrawWrap;

IDirectDrawSurface *WrapSurface(IDirectDrawSurface *real, const char *origin);
IDirectDrawClipper *WrapClipper(IDirectDrawClipper *real);
IDirectDrawPalette *WrapPalette(IDirectDrawPalette *real);


// Enumeration callbacks receive real surfaces; hand wrappers to the game's callback instead.
struct EnumCtx {
    LPDDENUMSURFACESCALLBACK cb;
    void *ctx;
};
HRESULT WINAPI EnumSurfacesThunk(LPDIRECTDRAWSURFACE s, LPDDSURFACEDESC d, LPVOID p)
{
    EnumCtx *e = static_cast<EnumCtx *>(p);
    return e->cb(WrapSurface(s, "enum"), d, e->ctx);
}

// ---------------------------------------------------------------------------------------------
// IDirectDrawPalette

class PaletteWrap final : public WrapBase<IDirectDrawPalette> {
public:
    explicit PaletteWrap(IDirectDrawPalette *r) : WrapBase(r, "Palette") {}

    STDMETHOD(QueryInterface)(REFIID riid, void **out) override
    {
        TRACE_STAT("Palette", "QueryInterface");
        return DoQueryInterface(s_stat, riid, out, IID_IDirectDrawPalette, CALLER());
    }
    STDMETHOD_(ULONG, AddRef)() override
    {
        TRACE_STAT("Palette", "AddRef");
        return DoAddRef(s_stat, CALLER());
    }
    STDMETHOD_(ULONG, Release)() override
    {
        TRACE_STAT("Palette", "Release");
        return DoRelease(s_stat, CALLER());
    }
    STDMETHOD(GetCaps)(LPDWORD caps) override
    {
        TRACE_STAT("Palette", "GetCaps");
        HRESULT hr = real_->GetCaps(caps);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            if (SUCCEEDED(hr) && caps) o.add("caps=0x%lx", *caps);
            LogCall(kind_, id_, "GetCaps", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetEntries)(DWORD flags, DWORD base, DWORD count, LPPALETTEENTRY e) override
    {
        TRACE_STAT("Palette", "GetEntries");
        HRESULT hr = real_->GetEntries(flags, base, count, e);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx base=%lu count=%lu", flags, base, count);
            LogCall(kind_, id_, "GetEntries", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(Initialize)(LPDIRECTDRAW dd, DWORD flags, LPPALETTEENTRY e) override
    {
        TRACE_STAT("Palette", "Initialize");
        HRESULT hr = real_->Initialize(Unwrap(dd), flags, e);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            LogCall(kind_, id_, "Initialize", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(SetEntries)(DWORD flags, DWORD start, DWORD count, LPPALETTEENTRY e) override
    {
        TRACE_STAT("Palette", "SetEntries");
        HRESULT hr = real_->SetEntries(flags, start, count, e);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx start=%lu count=%lu", flags, start, count);
            LogCall(kind_, id_, "SetEntries", a, hr, CALLER());
        }
        return hr;
    }
};

// ---------------------------------------------------------------------------------------------
// IDirectDrawClipper

class ClipperWrap final : public WrapBase<IDirectDrawClipper> {
public:
    explicit ClipperWrap(IDirectDrawClipper *r) : WrapBase(r, "Clipper") {}

    STDMETHOD(QueryInterface)(REFIID riid, void **out) override
    {
        TRACE_STAT("Clipper", "QueryInterface");
        return DoQueryInterface(s_stat, riid, out, IID_IDirectDrawClipper, CALLER());
    }
    STDMETHOD_(ULONG, AddRef)() override
    {
        TRACE_STAT("Clipper", "AddRef");
        return DoAddRef(s_stat, CALLER());
    }
    STDMETHOD_(ULONG, Release)() override
    {
        TRACE_STAT("Clipper", "Release");
        return DoRelease(s_stat, CALLER());
    }
    STDMETHOD(GetClipList)(LPRECT r, LPRGNDATA list, LPDWORD size) override
    {
        TRACE_STAT("Clipper", "GetClipList");
        HRESULT hr = real_->GetClipList(r, list, size);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            AddRect(a, r);
            if (size) o.add("size=%lu", *size);
            LogCall(kind_, id_, "GetClipList", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetHWnd)(HWND *hwnd) override
    {
        TRACE_STAT("Clipper", "GetHWnd");
        HRESULT hr = real_->GetHWnd(hwnd);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            if (SUCCEEDED(hr) && hwnd) o.add("hwnd=%p", static_cast<void *>(*hwnd));
            LogCall(kind_, id_, "GetHWnd", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(Initialize)(LPDIRECTDRAW dd, DWORD flags) override
    {
        TRACE_STAT("Clipper", "Initialize");
        HRESULT hr = real_->Initialize(Unwrap(dd), flags);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            LogCall(kind_, id_, "Initialize", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(IsClipListChanged)(WINBOOL *changed) override
    {
        TRACE_STAT("Clipper", "IsClipListChanged");
        HRESULT hr = real_->IsClipListChanged(changed);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            if (SUCCEEDED(hr) && changed) o.add("changed=%d", *changed);
            LogCall(kind_, id_, "IsClipListChanged", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(SetClipList)(LPRGNDATA list, DWORD flags) override
    {
        TRACE_STAT("Clipper", "SetClipList");
        HRESULT hr = real_->SetClipList(list, flags);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("list=%p flags=0x%lx", static_cast<void *>(list), flags);
            LogCall(kind_, id_, "SetClipList", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(SetHWnd)(DWORD flags, HWND hwnd) override
    {
        TRACE_STAT("Clipper", "SetHWnd");
        HRESULT hr = real_->SetHWnd(flags, hwnd);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx hwnd=%p", flags, static_cast<void *>(hwnd));
            LogCall(kind_, id_, "SetHWnd", a, hr, CALLER());
        }
        return hr;
    }
};

// ---------------------------------------------------------------------------------------------
// IDirectDrawSurface (v1)

class SurfaceWrap final : public WrapBase<IDirectDrawSurface> {
public:
    explicit SurfaceWrap(IDirectDrawSurface *r) : WrapBase(r, "Surface") {}

    STDMETHOD(QueryInterface)(REFIID riid, void **out) override
    {
        TRACE_STAT("Surface", "QueryInterface");
        return DoQueryInterface(s_stat, riid, out, IID_IDirectDrawSurface, CALLER());
    }
    STDMETHOD_(ULONG, AddRef)() override
    {
        TRACE_STAT("Surface", "AddRef");
        return DoAddRef(s_stat, CALLER());
    }
    STDMETHOD_(ULONG, Release)() override
    {
        TRACE_STAT("Surface", "Release");
        return DoRelease(s_stat, CALLER());
    }
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE s) override
    {
        TRACE_STAT("Surface", "AddAttachedSurface");
        HRESULT hr = real_->AddAttachedSurface(Unwrap(s));
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("surface=%p", static_cast<void *>(s));
            LogCall(kind_, id_, "AddAttachedSurface", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(AddOverlayDirtyRect)(LPRECT r) override
    {
        TRACE_STAT("Surface", "AddOverlayDirtyRect");
        HRESULT hr = real_->AddOverlayDirtyRect(r);
        if (Hit(s_stat, hr)) {
            Buf a;
            AddRect(a, r);
            LogCall(kind_, id_, "AddOverlayDirtyRect", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(Blt)(LPRECT dst, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) override
    {
        TRACE_STAT("Surface", "Blt");
        HRESULT hr = real_->Blt(dst, Unwrap(src), srcRect, flags, fx);
        if (primary_ && src && SUCCEEDED(hr)) {
            NotePresent(false, src, srcRect, dst, 0, 0, CALLER());
        }
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("dst=");
            AddRect(a, dst);
            a.add(" src=%s#%u srcRect=", src ? "Surface" : "NULL", src ? IdOf(src) : 0);
            AddRect(a, srcRect);
            a.add(" flags=0x%lx fx=%p", flags, static_cast<void *>(fx));
            if (fx) a.add(" fx.flags=0x%lx fill=0x%lx", fx->dwDDFX, fx->dwFillColor);
            LogCall(kind_, id_, "Blt", a, hr, CALLER());
        }
        MaybePeriodicSummary();
        return hr;
    }
    STDMETHOD(BltBatch)(LPDDBLTBATCH batch, DWORD count, DWORD flags) override
    {
        TRACE_STAT("Surface", "BltBatch");
        // The batch entries carry source surface pointers; forward a copy with them unwrapped.
        DDBLTBATCH local[16];
        LPDDBLTBATCH use = batch;
        if (batch && count <= 16) {
            for (DWORD i = 0; i < count; ++i) {
                local[i] = batch[i];
                local[i].lpDDSSrc = Unwrap(batch[i].lpDDSSrc);
            }
            use = local;
        }
        HRESULT hr = real_->BltBatch(use, count, flags);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("count=%lu flags=0x%lx%s", count, flags, count > 16 ? " (>16: not unwrapped)" : "");
            LogCall(kind_, id_, "BltBatch", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD trans) override
    {
        TRACE_STAT("Surface", "BltFast");
        HRESULT hr = real_->BltFast(x, y, Unwrap(src), srcRect, trans);
        if (primary_ && src && SUCCEEDED(hr)) {
            NotePresent(true, src, srcRect, nullptr, static_cast<LONG>(x), static_cast<LONG>(y), CALLER());
        }
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("x=%lu y=%lu src=%s#%u srcRect=", x, y, src ? "Surface" : "NULL", src ? IdOf(src) : 0);
            AddRect(a, srcRect);
            a.add(" trans=0x%lx", trans);
            LogCall(kind_, id_, "BltFast", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(DeleteAttachedSurface)(DWORD flags, LPDIRECTDRAWSURFACE s) override
    {
        TRACE_STAT("Surface", "DeleteAttachedSurface");
        HRESULT hr = real_->DeleteAttachedSurface(flags, Unwrap(s));
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx surface=%p", flags, static_cast<void *>(s));
            LogCall(kind_, id_, "DeleteAttachedSurface", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(EnumAttachedSurfaces)(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) override
    {
        TRACE_STAT("Surface", "EnumAttachedSurfaces");
        EnumCtx e{cb, ctx};
        HRESULT hr = real_->EnumAttachedSurfaces(&e, EnumSurfacesThunk);
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "EnumAttachedSurfaces", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(EnumOverlayZOrders)(DWORD flags, LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) override
    {
        TRACE_STAT("Surface", "EnumOverlayZOrders");
        EnumCtx e{cb, ctx};
        HRESULT hr = real_->EnumOverlayZOrders(flags, &e, EnumSurfacesThunk);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            LogCall(kind_, id_, "EnumOverlayZOrders", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE target, DWORD flags) override
    {
        TRACE_STAT("Surface", "Flip");
        HRESULT hr = real_->Flip(Unwrap(target), flags);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("target=%p flags=0x%lx", static_cast<void *>(target), flags);
            LogCall(kind_, id_, "Flip", a, hr, CALLER());
        }
        MaybePeriodicSummary();
        return hr;
    }
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS caps, LPDIRECTDRAWSURFACE *out) override
    {
        TRACE_STAT("Surface", "GetAttachedSurface");
        HRESULT hr = real_->GetAttachedSurface(caps, out);
        if (SUCCEEDED(hr) && out && *out) {
            *out = WrapSurface(*out, "attached");
        }
        if (Hit(s_stat, hr)) {
            Buf a, o;
            a.add("caps=0x%lx", caps ? caps->dwCaps : 0);
            if (SUCCEEDED(hr) && out) o.add("-> Surface#%u", IdOf(*out));
            LogCall(kind_, id_, "GetAttachedSurface", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetBltStatus)(DWORD flags) override
    {
        TRACE_STAT("Surface", "GetBltStatus");
        HRESULT hr = real_->GetBltStatus(flags);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            LogCall(kind_, id_, "GetBltStatus", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(GetCaps)(LPDDSCAPS caps) override
    {
        TRACE_STAT("Surface", "GetCaps");
        HRESULT hr = real_->GetCaps(caps);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            if (SUCCEEDED(hr) && caps) o.add("caps=0x%lx", caps->dwCaps);
            LogCall(kind_, id_, "GetCaps", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER *out) override
    {
        TRACE_STAT("Surface", "GetClipper");
        HRESULT hr = real_->GetClipper(out);
        if (SUCCEEDED(hr) && out && *out) {
            *out = WrapClipper(*out);
        }
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "GetClipper", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(GetColorKey)(DWORD flags, LPDDCOLORKEY key) override
    {
        TRACE_STAT("Surface", "GetColorKey");
        HRESULT hr = real_->GetColorKey(flags, key);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            a.add("flags=0x%lx", flags);
            if (SUCCEEDED(hr) && key) o.add("key=[0x%lx,0x%lx]", key->dwColorSpaceLowValue, key->dwColorSpaceHighValue);
            LogCall(kind_, id_, "GetColorKey", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetDC)(HDC *dc) override
    {
        TRACE_STAT("Surface", "GetDC");
        HRESULT hr = real_->GetDC(dc);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            if (SUCCEEDED(hr) && dc) o.add("hdc=%p", static_cast<void *>(*dc));
            LogCall(kind_, id_, "GetDC", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetFlipStatus)(DWORD flags) override
    {
        TRACE_STAT("Surface", "GetFlipStatus");
        HRESULT hr = real_->GetFlipStatus(flags);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            LogCall(kind_, id_, "GetFlipStatus", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(GetOverlayPosition)(LPLONG x, LPLONG y) override
    {
        TRACE_STAT("Surface", "GetOverlayPosition");
        HRESULT hr = real_->GetOverlayPosition(x, y);
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "GetOverlayPosition", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE *out) override
    {
        TRACE_STAT("Surface", "GetPalette");
        HRESULT hr = real_->GetPalette(out);
        if (SUCCEEDED(hr) && out && *out) {
            *out = WrapPalette(*out);
        }
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "GetPalette", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT pf) override
    {
        TRACE_STAT("Surface", "GetPixelFormat");
        HRESULT hr = real_->GetPixelFormat(pf);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            if (SUCCEEDED(hr) && pf) AddPixelFormat(o, *pf);
            LogCall(kind_, id_, "GetPixelFormat", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC d) override
    {
        TRACE_STAT("Surface", "GetSurfaceDesc");
        HRESULT hr = real_->GetSurfaceDesc(d);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            if (SUCCEEDED(hr)) AddDesc(o, d);
            LogCall(kind_, id_, "GetSurfaceDesc", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(Initialize)(LPDIRECTDRAW dd, LPDDSURFACEDESC d) override
    {
        TRACE_STAT("Surface", "Initialize");
        HRESULT hr = real_->Initialize(Unwrap(dd), d);
        if (Hit(s_stat, hr)) {
            Buf a;
            AddDesc(a, d);
            LogCall(kind_, id_, "Initialize", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(IsLost)() override
    {
        TRACE_STAT("Surface", "IsLost");
        CountIsLost();
        HRESULT hr = real_->IsLost();
        if (Hit(s_stat, hr) || FAILED(hr)) {
            Buf a;
            LogCall(kind_, id_, "IsLost", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(Lock)(LPRECT r, LPDDSURFACEDESC d, DWORD flags, HANDLE ev) override
    {
        TRACE_STAT("Surface", "Lock");
        HRESULT hr = real_->Lock(r, d, flags, ev);
        if (Hit(s_stat, hr) || FAILED(hr)) {
            Buf a, o;
            a.add("rect=");
            AddRect(a, r);
            a.add(" flags=0x%lx", flags);
            if (SUCCEEDED(hr)) AddDesc(o, d);
            LogCall(kind_, id_, "Lock", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(ReleaseDC)(HDC dc) override
    {
        TRACE_STAT("Surface", "ReleaseDC");
        HRESULT hr = real_->ReleaseDC(dc);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("hdc=%p", static_cast<void *>(dc));
            LogCall(kind_, id_, "ReleaseDC", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(Restore)() override
    {
        TRACE_STAT("Surface", "Restore");
        HRESULT hr = real_->Restore();
        if (Hit(s_stat, hr) || FAILED(hr)) {
            Buf a;
            LogCall(kind_, id_, "Restore", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER c) override
    {
        TRACE_STAT("Surface", "SetClipper");
        HRESULT hr = real_->SetClipper(Unwrap(c));
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("clipper=%p", static_cast<void *>(c));
            LogCall(kind_, id_, "SetClipper", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(SetColorKey)(DWORD flags, LPDDCOLORKEY key) override
    {
        TRACE_STAT("Surface", "SetColorKey");
        HRESULT hr = real_->SetColorKey(flags, key);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            if (key) a.add(" key=[0x%lx,0x%lx]", key->dwColorSpaceLowValue, key->dwColorSpaceHighValue);
            LogCall(kind_, id_, "SetColorKey", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(SetOverlayPosition)(LONG x, LONG y) override
    {
        TRACE_STAT("Surface", "SetOverlayPosition");
        HRESULT hr = real_->SetOverlayPosition(x, y);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("x=%ld y=%ld", x, y);
            LogCall(kind_, id_, "SetOverlayPosition", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE p) override
    {
        TRACE_STAT("Surface", "SetPalette");
        HRESULT hr = real_->SetPalette(Unwrap(p));
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("palette=%p", static_cast<void *>(p));
            LogCall(kind_, id_, "SetPalette", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(Unlock)(LPVOID data) override
    {
        TRACE_STAT("Surface", "Unlock");
        HRESULT hr = real_->Unlock(data);
        if (Hit(s_stat, hr) || FAILED(hr)) {
            Buf a;
            a.add("data=%p", data);
            LogCall(kind_, id_, "Unlock", a, hr, CALLER());
        }
        MaybePeriodicSummary();
        return hr;
    }
    STDMETHOD(UpdateOverlay)(LPRECT srcRect, LPDIRECTDRAWSURFACE dst, LPRECT dstRect, DWORD flags,
                             LPDDOVERLAYFX fx) override
    {
        TRACE_STAT("Surface", "UpdateOverlay");
        HRESULT hr = real_->UpdateOverlay(srcRect, Unwrap(dst), dstRect, flags, fx);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            LogCall(kind_, id_, "UpdateOverlay", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(UpdateOverlayDisplay)(DWORD flags) override
    {
        TRACE_STAT("Surface", "UpdateOverlayDisplay");
        HRESULT hr = real_->UpdateOverlayDisplay(flags);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            LogCall(kind_, id_, "UpdateOverlayDisplay", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(UpdateOverlayZOrder)(DWORD flags, LPDIRECTDRAWSURFACE ref) override
    {
        TRACE_STAT("Surface", "UpdateOverlayZOrder");
        HRESULT hr = real_->UpdateOverlayZOrder(flags, Unwrap(ref));
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            LogCall(kind_, id_, "UpdateOverlayZOrder", a, hr, CALLER());
        }
        return hr;
    }

    // The wrapper behind a surface pointer passed by the game (nullptr if it is not one of ours).
    static SurfaceWrap *AsOurs(IDirectDrawSurface *p)
    {
        if (!p) {
            return nullptr;
        }
        AcquireSRWLockShared(&g_registryLock);
        bool ours = WrapperToReal().count(p) != 0;
        ReleaseSRWLockShared(&g_registryLock);
        return ours ? static_cast<SurfaceWrap *>(p) : nullptr;
    }
    // Wrapper id of a surface pointer passed by the game (0 if it is not one of ours).
    static unsigned IdOf(IDirectDrawSurface *p)
    {
        SurfaceWrap *w = AsOurs(p);
        return w ? w->id() : 0;
    }

    void MarkPrimary() { primary_ = true; }

private:
    // Size of the real surface, queried once (a DirectDraw surface never changes size).
    SIZE Size()
    {
        if (size_.cx == 0) {
            DDSURFACEDESC d{};
            d.dwSize = sizeof(d);
            if (SUCCEEDED(real_->GetSurfaceDesc(&d))) {
                size_ = {static_cast<LONG>(d.dwWidth), static_cast<LONG>(d.dwHeight)};
            }
        }
        return size_;
    }

    // Feeds the shared output monitor. fast: BltFast, whose destination is (x, y) with the source size;
    // otherwise Blt, whose NULL dst means the whole primary.
    void NotePresent(bool fast, IDirectDrawSurface *src, const RECT *srcRect, const RECT *dst, LONG x, LONG y,
                     void *caller)
    {
        SurfaceWrap *s = AsOurs(src);
        SIZE ss = s ? s->Size() : SIZE{0, 0};
        RECT sr = srcRect ? *srcRect : RECT{0, 0, ss.cx, ss.cy};
        SIZE ps = Size();
        RECT dr = fast ? RECT{x, y, x + sr.right - sr.left, y + sr.bottom - sr.top}
                       : dst ? *dst : RECT{0, 0, ps.cx, ps.cy};
        core::ddlog::NotePresent(fast ? "BltFast" : "Blt", s ? s->id() : 0, ss, sr, id_, ps, dr, caller);
    }

    bool primary_ = false;
    SIZE size_{0, 0};
};

// ---------------------------------------------------------------------------------------------
// IDirectDraw (v1)

class DirectDrawWrap final : public WrapBase<IDirectDraw> {
public:
    explicit DirectDrawWrap(IDirectDraw *r) : WrapBase(r, "DirectDraw") { ++g_liveDirectDraw; }
    HWND hwnd_ = nullptr; // from SetCooperativeLevel, for the window log after SetDisplayMode
    ~DirectDrawWrap() override
    {
        if (--g_liveDirectDraw == 0) {
            core_ddraw_write_summary("last IDirectDraw released");
        }
    }

    STDMETHOD(QueryInterface)(REFIID riid, void **out) override
    {
        TRACE_STAT("DirectDraw", "QueryInterface");
        return DoQueryInterface(s_stat, riid, out, IID_IDirectDraw, CALLER());
    }
    STDMETHOD_(ULONG, AddRef)() override
    {
        TRACE_STAT("DirectDraw", "AddRef");
        return DoAddRef(s_stat, CALLER());
    }
    STDMETHOD_(ULONG, Release)() override
    {
        TRACE_STAT("DirectDraw", "Release");
        return DoRelease(s_stat, CALLER());
    }
    STDMETHOD(Compact)() override
    {
        TRACE_STAT("DirectDraw", "Compact");
        HRESULT hr = real_->Compact();
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "Compact", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(CreateClipper)(DWORD flags, LPDIRECTDRAWCLIPPER *out, IUnknown *outer) override
    {
        TRACE_STAT("DirectDraw", "CreateClipper");
        HRESULT hr = real_->CreateClipper(flags, out, outer);
        if (SUCCEEDED(hr) && out && *out) {
            *out = WrapClipper(*out);
        }
        if (Hit(s_stat, hr)) {
            Buf a, o;
            a.add("flags=0x%lx", flags);
            if (SUCCEEDED(hr) && out) o.add("-> Clipper#%u", static_cast<ClipperWrap *>(*out)->id());
            LogCall(kind_, id_, "CreateClipper", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(CreatePalette)(DWORD flags, LPPALETTEENTRY table, LPDIRECTDRAWPALETTE *out, IUnknown *outer) override
    {
        TRACE_STAT("DirectDraw", "CreatePalette");
        HRESULT hr = real_->CreatePalette(flags, table, out, outer);
        if (SUCCEEDED(hr) && out && *out) {
            *out = WrapPalette(*out);
        }
        if (Hit(s_stat, hr)) {
            Buf a, o;
            a.add("flags=0x%lx", flags);
            if (SUCCEEDED(hr) && out) o.add("-> Palette#%u", static_cast<PaletteWrap *>(*out)->id());
            LogCall(kind_, id_, "CreatePalette", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE *out, IUnknown *outer) override
    {
        TRACE_STAT("DirectDraw", "CreateSurface");
        HRESULT hr = real_->CreateSurface(d, out, outer);
        if (SUCCEEDED(hr) && out && *out) {
            *out = WrapSurface(*out, "CreateSurface");
            if (d && (d->dwFlags & DDSD_CAPS) && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
                SurfaceWrap::AsOurs(*out)->MarkPrimary();
            }
        }
        if (Hit(s_stat, hr) || FAILED(hr)) {
            Buf a, o;
            AddDesc(a, d);
            if (SUCCEEDED(hr) && out) o.add("-> Surface#%u", SurfaceWrap::IdOf(*out));
            LogCall(kind_, id_, "CreateSurface", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE src, LPDIRECTDRAWSURFACE *out) override
    {
        TRACE_STAT("DirectDraw", "DuplicateSurface");
        HRESULT hr = real_->DuplicateSurface(Unwrap(src), out);
        if (SUCCEEDED(hr) && out && *out) {
            *out = WrapSurface(*out, "DuplicateSurface");
        }
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("src=Surface#%u", SurfaceWrap::IdOf(src));
            LogCall(kind_, id_, "DuplicateSurface", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(EnumDisplayModes)(DWORD flags, LPDDSURFACEDESC d, LPVOID ctx, LPDDENUMMODESCALLBACK cb) override
    {
        TRACE_STAT("DirectDraw", "EnumDisplayModes");
        HRESULT hr = real_->EnumDisplayModes(flags, d, ctx, cb);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx ", flags);
            AddDesc(a, d);
            LogCall(kind_, id_, "EnumDisplayModes", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(EnumSurfaces)(DWORD flags, LPDDSURFACEDESC d, LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) override
    {
        TRACE_STAT("DirectDraw", "EnumSurfaces");
        EnumCtx e{cb, ctx};
        HRESULT hr = real_->EnumSurfaces(flags, d, &e, EnumSurfacesThunk);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            LogCall(kind_, id_, "EnumSurfaces", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(FlipToGDISurface)() override
    {
        TRACE_STAT("DirectDraw", "FlipToGDISurface");
        HRESULT hr = real_->FlipToGDISurface();
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "FlipToGDISurface", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(GetCaps)(LPDDCAPS drv, LPDDCAPS hel) override
    {
        TRACE_STAT("DirectDraw", "GetCaps");
        HRESULT hr = real_->GetCaps(drv, hel);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            a.add("drv=%p hel=%p", static_cast<void *>(drv), static_cast<void *>(hel));
            if (SUCCEEDED(hr) && drv) o.add("drv.caps=0x%lx drv.caps2=0x%lx", drv->dwCaps, drv->dwCaps2);
            LogCall(kind_, id_, "GetCaps", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC d) override
    {
        TRACE_STAT("DirectDraw", "GetDisplayMode");
        HRESULT hr = real_->GetDisplayMode(d);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            if (SUCCEEDED(hr)) AddDesc(o, d);
            LogCall(kind_, id_, "GetDisplayMode", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetFourCCCodes)(LPDWORD num, LPDWORD codes) override
    {
        TRACE_STAT("DirectDraw", "GetFourCCCodes");
        HRESULT hr = real_->GetFourCCCodes(num, codes);
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "GetFourCCCodes", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE *out) override
    {
        TRACE_STAT("DirectDraw", "GetGDISurface");
        HRESULT hr = real_->GetGDISurface(out);
        if (SUCCEEDED(hr) && out && *out) {
            *out = WrapSurface(*out, "GetGDISurface");
        }
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "GetGDISurface", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(GetMonitorFrequency)(LPDWORD f) override
    {
        TRACE_STAT("DirectDraw", "GetMonitorFrequency");
        HRESULT hr = real_->GetMonitorFrequency(f);
        if (Hit(s_stat, hr)) {
            Buf a, o;
            if (SUCCEEDED(hr) && f) o.add("freq=%lu", *f);
            LogCall(kind_, id_, "GetMonitorFrequency", a, hr, CALLER(), &o);
        }
        return hr;
    }
    STDMETHOD(GetScanLine)(LPDWORD line) override
    {
        TRACE_STAT("DirectDraw", "GetScanLine");
        HRESULT hr = real_->GetScanLine(line);
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "GetScanLine", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(GetVerticalBlankStatus)(WINBOOL *inVb) override
    {
        TRACE_STAT("DirectDraw", "GetVerticalBlankStatus");
        HRESULT hr = real_->GetVerticalBlankStatus(inVb);
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "GetVerticalBlankStatus", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(Initialize)(GUID *g) override
    {
        TRACE_STAT("DirectDraw", "Initialize");
        HRESULT hr = real_->Initialize(g);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("guid=%p", static_cast<void *>(g));
            LogCall(kind_, id_, "Initialize", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(RestoreDisplayMode)() override
    {
        TRACE_STAT("DirectDraw", "RestoreDisplayMode");
        HRESULT hr = real_->RestoreDisplayMode();
        if (Hit(s_stat, hr)) {
            Buf a;
            LogCall(kind_, id_, "RestoreDisplayMode", a, hr, CALLER());
        }
        return hr;
    }
    STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD flags) override
    {
        TRACE_STAT("DirectDraw", "SetCooperativeLevel");
        HRESULT hr = real_->SetCooperativeLevel(hwnd, flags);
        hwnd_ = hwnd;
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("hwnd=%p flags=0x%lx", static_cast<void *>(hwnd), flags);
            LogCall(kind_, id_, "SetCooperativeLevel", a, hr, CALLER());
            core_dpi_log_window(hwnd, "after SetCooperativeLevel");
        }
        return hr;
    }
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp) override
    {
        TRACE_STAT("DirectDraw", "SetDisplayMode");
        HRESULT hr = real_->SetDisplayMode(w, h, bpp);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("%lux%lux%lu", w, h, bpp);
            LogCall(kind_, id_, "SetDisplayMode", a, hr, CALLER());
            core_dpi_log_window(hwnd_, "after SetDisplayMode");
        }
        return hr;
    }
    STDMETHOD(WaitForVerticalBlank)(DWORD flags, HANDLE ev) override
    {
        TRACE_STAT("DirectDraw", "WaitForVerticalBlank");
        HRESULT hr = real_->WaitForVerticalBlank(flags, ev);
        if (Hit(s_stat, hr)) {
            Buf a;
            a.add("flags=0x%lx", flags);
            LogCall(kind_, id_, "WaitForVerticalBlank", a, hr, CALLER());
        }
        return hr;
    }
};

// Wrap helpers: reuse an existing wrapper for the same real object (the real call already added a
// reference, so the wrapper takes over that reference).
template <typename W, typename I> I *WrapAs(I *real, const char *what, const char *origin)
{
    if (!real) {
        return nullptr;
    }
    if (void *w = FindWrapper(real)) {
        static_cast<W *>(static_cast<I *>(w))->AddWrapperRef();
        return static_cast<I *>(w);
    }
    W *w = new W(real);
    core_log("new %s#%u wraps real %p (from %s)", what, w->id(), static_cast<void *>(real), origin);
    return w;
}

IDirectDrawSurface *WrapSurface(IDirectDrawSurface *real, const char *origin)
{
    return WrapAs<SurfaceWrap>(real, "Surface", origin);
}
IDirectDrawClipper *WrapClipper(IDirectDrawClipper *real)
{
    return WrapAs<ClipperWrap>(real, "Clipper", "CreateClipper/GetClipper");
}
IDirectDrawPalette *WrapPalette(IDirectDrawPalette *real)
{
    return WrapAs<PaletteWrap>(real, "Palette", "CreatePalette/GetPalette");
}

} // namespace

extern "C" HRESULT core_ddraw_create(core_DirectDrawCreate_fn real_create, GUID *guid, void **dd, IUnknown *outer,
                                     void *caller)
{
    HRESULT hr = real_create(guid, dd, outer);
    char tmp[16];
    char where[MAX_PATH + 40];
    core_describe_address(caller, where, sizeof(where));
    if (SUCCEEDED(hr) && dd && *dd) {
        IDirectDraw *real = static_cast<IDirectDraw *>(*dd);
        DirectDrawWrap *w = new DirectDrawWrap(real);
        *dd = static_cast<IDirectDraw *>(w);
        core_log("DirectDrawCreate(guid=%p, outer=%p) -> %s, real %p wrapped as DirectDraw#%u  caller=%s",
                 static_cast<void *>(guid), static_cast<void *>(outer), HrName(hr, tmp, sizeof(tmp)),
                 static_cast<void *>(real), w->id(), where);
    }
    else {
        core_log("DirectDrawCreate(guid=%p, outer=%p) -> %s (not wrapped)  caller=%s", static_cast<void *>(guid),
                 static_cast<void *>(outer), HrName(hr, tmp, sizeof(tmp)), where);
    }
    return hr;
}
