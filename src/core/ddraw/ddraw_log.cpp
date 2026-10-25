#include "core/ddraw/ddraw_log.h"

#include "core/ddraw/ddraw_wrap.h"
#include "core/log.h"
#include "core/platform/timing.h"

namespace core::ddlog {

namespace {

std::atomic<Stat *> g_stats{nullptr};
std::atomic<DWORD> g_lastSummary{0};
std::atomic<long> g_isLostCalls{0};

} // namespace

Stat::Stat(const char *i, const char *m) : iface(i), method(m)
{
    Stat *head = g_stats.load();
    do {
        next = head;
    } while (!g_stats.compare_exchange_weak(head, this));
}

bool Hit(Stat &s, HRESULT hr)
{
    long n = ++s.calls;
    if (FAILED(hr)) {
        ++s.failures;
        s.lastFailure = hr;
    }
    return n <= kDetailCalls;
}

void MaybePeriodicSummary()
{
    DWORD now = GetTickCount();
    DWORD last = g_lastSummary.load();
    if (last == 0) {
        g_lastSummary.compare_exchange_strong(last, now);
        return;
    }
    if (now - last >= kSummaryMs && g_lastSummary.compare_exchange_strong(last, now)) {
        core_ddraw_write_summary("periodic");
    }
}

const char *HrName(HRESULT hr, char *tmp, size_t len)
{
    // Symbolic constants from ddraw.h (the v1 table used hand-written values, several of them wrong).
    switch (hr) {
    case DD_OK: return "DD_OK";
    case S_FALSE: return "S_FALSE";
    case DDERR_SURFACELOST: return "DDERR_SURFACELOST";
    case DDERR_WASSTILLDRAWING: return "DDERR_WASSTILLDRAWING";
    case DDERR_INVALIDPARAMS: return "DDERR_INVALIDPARAMS";
    case DDERR_UNSUPPORTED: return "DDERR_UNSUPPORTED";
    case DDERR_GENERIC: return "DDERR_GENERIC";
    case E_NOINTERFACE: return "E_NOINTERFACE";
    case DDERR_OUTOFMEMORY: return "DDERR_OUTOFMEMORY";
    case DDERR_NOEXCLUSIVEMODE: return "DDERR_NOEXCLUSIVEMODE";
    case DDERR_EXCLUSIVEMODEALREADYSET: return "DDERR_EXCLUSIVEMODEALREADYSET";
    case DDERR_ALREADYINITIALIZED: return "DDERR_ALREADYINITIALIZED";
    case DDERR_NOTFOUND: return "DDERR_NOTFOUND";
    case DDERR_INVALIDOBJECT: return "DDERR_INVALIDOBJECT";
    case DDERR_SURFACEBUSY: return "DDERR_SURFACEBUSY";
    case DDERR_INVALIDMODE: return "DDERR_INVALIDMODE";
    case DDERR_UNSUPPORTEDMODE: return "DDERR_UNSUPPORTEDMODE";
    case DDERR_NOTLOCKED: return "DDERR_NOTLOCKED";
    case DDERR_INVALIDPIXELFORMAT: return "DDERR_INVALIDPIXELFORMAT";
    case DDERR_INVALIDCAPS: return "DDERR_INVALIDCAPS";
    case DDERR_NOCOOPERATIVELEVELSET: return "DDERR_NOCOOPERATIVELEVELSET";
    default:
        std::snprintf(tmp, len, "0x%08lX", static_cast<unsigned long>(hr));
        return tmp;
    }
}

void AddRect(Buf &b, const RECT *r)
{
    if (r) {
        b.add("[%ld,%ld,%ld,%ld]", r->left, r->top, r->right, r->bottom);
    }
    else {
        b.add("NULL");
    }
}

void AddGuid(Buf &b, REFIID g)
{
    struct Named {
        const GUID *g;
        const char *name;
    };
    static const Named names[] = {
        {&IID_IUnknown, "IUnknown"},
        {&IID_IDirectDraw, "IDirectDraw"},
        {&IID_IDirectDraw2, "IDirectDraw2"},
        {&IID_IDirectDraw4, "IDirectDraw4"},
        {&IID_IDirectDraw7, "IDirectDraw7"},
        {&IID_IDirectDrawSurface, "IDirectDrawSurface"},
        {&IID_IDirectDrawSurface2, "IDirectDrawSurface2"},
        {&IID_IDirectDrawSurface3, "IDirectDrawSurface3"},
        {&IID_IDirectDrawSurface4, "IDirectDrawSurface4"},
        {&IID_IDirectDrawSurface7, "IDirectDrawSurface7"},
        {&IID_IDirectDrawClipper, "IDirectDrawClipper"},
        {&IID_IDirectDrawPalette, "IDirectDrawPalette"},
    };
    for (const Named &n : names) {
        if (IsEqualGUID(g, *n.g)) {
            b.add("%s", n.name);
            return;
        }
    }
    b.add("{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3, g.Data4[0],
          g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

void AddPixelFormat(Buf &b, const DDPIXELFORMAT &pf)
{
    b.add("pf{flags=0x%lx bits=%lu r=%08lx g=%08lx b=%08lx a=%08lx}", pf.dwFlags, pf.dwRGBBitCount, pf.dwRBitMask,
          pf.dwGBitMask, pf.dwBBitMask, pf.dwRGBAlphaBitMask);
}

void AddDesc(Buf &b, const DDSURFACEDESC *d)
{
    if (!d) {
        b.add("desc=NULL");
        return;
    }
    b.add("desc{size=%lu flags=0x%lx", d->dwSize, d->dwFlags);
    if (d->dwFlags & DDSD_WIDTH) b.add(" w=%lu", d->dwWidth);
    if (d->dwFlags & DDSD_HEIGHT) b.add(" h=%lu", d->dwHeight);
    if (d->dwFlags & DDSD_PITCH) b.add(" pitch=%ld", d->lPitch);
    if (d->dwFlags & DDSD_BACKBUFFERCOUNT) b.add(" backbuffers=%lu", d->dwBackBufferCount);
    if (d->dwFlags & DDSD_REFRESHRATE) b.add(" refresh=%lu", d->dwRefreshRate);
    if (d->dwFlags & DDSD_CAPS) b.add(" caps=0x%lx", d->ddsCaps.dwCaps);
    if (d->dwFlags & DDSD_PIXELFORMAT) {
        b.add(" ");
        AddPixelFormat(b, d->ddpfPixelFormat);
    }
    if (d->lpSurface) b.add(" surface=%p", d->lpSurface);
    b.add("}");
}

void LogCall(const char *iface, unsigned id, const char *method, const Buf &args, HRESULT hr, void *caller,
             const Buf *out)
{
    char tmp[16];
    char where[MAX_PATH + 40];
    core_log("%s#%u.%s(%s) -> %s%s%s  caller=%s", iface, id, method, args.s, HrName(hr, tmp, sizeof(tmp)),
             out ? "  " : "", out ? out->s : "", core_describe_address(caller, where, sizeof(where)));
}

void NotePresent(const char *how, unsigned srcId, SIZE srcSize, const RECT &sr, unsigned primaryId, SIZE ps,
                 const RECT &dr, void *caller)
{
    // Called from the game's render thread only, so plain statics are enough.
    static RECT s_lastSrc, s_lastDst;
    static unsigned s_lastPrimary;
    static long s_changes;
    if (primaryId != s_lastPrimary || !EqualRect(&sr, &s_lastSrc) || !EqualRect(&dr, &s_lastDst)) {
        s_lastPrimary = primaryId;
        s_lastSrc = sr;
        s_lastDst = dr;
        if (++s_changes <= kDetailCalls) {
            LONG dw = dr.right - dr.left, dh = dr.bottom - dr.top;
            bool full = dr.left == 0 && dr.top == 0 && dw == ps.cx && dh == ps.cy;
            char where[MAX_PATH + 40];
            core_log("present: Surface#%u -> primary Surface#%u (%ldx%ld) via %s: src [%ld,%ld,%ld,%ld] %ldx%ld"
                     " of %ldx%ld -> dst (%ld,%ld) %ldx%ld%s  caller=%s",
                     srcId, primaryId, ps.cx, ps.cy, how, sr.left, sr.top, sr.right, sr.bottom, sr.right - sr.left,
                     sr.bottom - sr.top, srcSize.cx, srcSize.cy, dr.left, dr.top, dw, dh,
                     full ? " = whole screen" : " (NOT the whole screen)",
                     core_describe_address(caller, where, sizeof(where)));
        }
    }
    static long s_presents;
    static DWORD s_since;
    DWORD now = GetTickCount();
    if (s_since == 0) {
        s_since = now;
    }
    ++s_presents;
    if (now - s_since >= kPresentRateMs) {
        long lost = g_isLostCalls.exchange(0);
        core_log("present rate: %ld frames in %lu ms = %.1f/s (last: %s %ldx%ld); IsLost calls %ld = %.0f/s; "
                 "timer resolution %u us",
                 s_presents, now - s_since, s_presents * 1000.0 / (now - s_since), how, sr.right - sr.left,
                 sr.bottom - sr.top, lost, lost * 1000.0 / (now - s_since), core_timer_resolution_us());
        s_presents = 0;
        s_since = now;
    }
}

void CountIsLost()
{
    ++g_isLostCalls;
}

} // namespace core::ddlog

extern "C" void core_ddraw_write_summary(const char *reason)
{
    using namespace core::ddlog;
    core_log("==== ddraw method summary (%s): interface.method calls failures last_failure ====", reason);
    for (Stat *s = g_stats.load(); s; s = s->next) {
        char tmp[16];
        long fails = s->failures.load();
        core_log("  %-10s %-24s %8ld %6ld %s", s->iface, s->method, s->calls.load(), fails,
                 fails ? HrName(static_cast<HRESULT>(s->lastFailure.load()), tmp, sizeof(tmp)) : "-");
    }
    core_log("==== end of summary ====");
}
