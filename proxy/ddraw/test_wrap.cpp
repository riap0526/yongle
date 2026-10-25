// Offline test of the logging ddraw wrapper (not the game). Loads the given ddraw.dll by path and drives
// it the way Emperor does: DirectDrawCreate, SetCooperativeLevel (DDSCL_NORMAL, so the desktop mode is
// never changed), primary surface + clipper, an 800x600 16-bit offscreen back buffer, Lock/Unlock with
// pixel writes, Blt to the primary, GetSurfaceDesc/GetPixelFormat, a QueryInterface that escapes the
// wrapper, and full release. Checks that every call succeeds and that the final Releases return 0.
// Usage: test_wrap.exe <path-to-ddraw.dll>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>

#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what, HRESULT hr = 0)
{
    std::printf("%-44s %s (hr=0x%08lX)\n", what, ok ? "ok" : "FAILED", static_cast<unsigned long>(hr));
    if (!ok) {
        ++g_failures;
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ddraw.dll>\n", argv[0]);
        return 2;
    }
    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    using CreateFn = HRESULT(WINAPI *)(GUID *, IDirectDraw **, IUnknown *);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(mod, "DirectDrawCreate"));
    Check(create != nullptr, "GetProcAddress(DirectDrawCreate)");
    if (!create) {
        return 1;
    }

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "test_wrap";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "test_wrap", WS_OVERLAPPEDWINDOW, 50, 50, 816, 639, nullptr,
                                nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    // The DLL was loaded after this exe's entry point, so DPI awareness comes from the CreateWindowExA fallback.
    // The window above was created inside that call, after the awareness change.
    Check(IsProcessDPIAware() != 0, "DPI aware after first CreateWindowExA (fallback)");

    IDirectDraw *dd = nullptr;
    HRESULT hr = create(nullptr, &dd, nullptr);
    Check(SUCCEEDED(hr) && dd, "DirectDrawCreate", hr);
    if (!dd) {
        return 1;
    }
    hr = dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    Check(SUCCEEDED(hr), "SetCooperativeLevel(DDSCL_NORMAL)", hr);

    DDSURFACEDESC mode{};
    mode.dwSize = sizeof(mode);
    hr = dd->GetDisplayMode(&mode);
    Check(SUCCEEDED(hr), "GetDisplayMode", hr);

    DDSURFACEDESC pd{};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DDSD_CAPS;
    pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface *primary = nullptr;
    hr = dd->CreateSurface(&pd, &primary, nullptr);
    Check(SUCCEEDED(hr) && primary, "CreateSurface(primary)", hr);

    IDirectDrawClipper *clip = nullptr;
    hr = dd->CreateClipper(0, &clip, nullptr);
    Check(SUCCEEDED(hr) && clip, "CreateClipper", hr);
    if (clip) {
        hr = clip->SetHWnd(0, hwnd);
        Check(SUCCEEDED(hr), "Clipper.SetHWnd", hr);
        hr = primary->SetClipper(clip);
        Check(SUCCEEDED(hr), "Surface.SetClipper(wrapped clipper)", hr);
    }

    DDSURFACEDESC bd{};
    bd.dwSize = sizeof(bd);
    bd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    bd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    bd.dwWidth = 800;
    bd.dwHeight = 600;
    bd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    bd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    bd.ddpfPixelFormat.dwRGBBitCount = 16;
    bd.ddpfPixelFormat.dwRBitMask = 0xF800;
    bd.ddpfPixelFormat.dwGBitMask = 0x07E0;
    bd.ddpfPixelFormat.dwBBitMask = 0x001F;
    IDirectDrawSurface *back = nullptr;
    hr = dd->CreateSurface(&bd, &back, nullptr);
    Check(SUCCEEDED(hr) && back, "CreateSurface(800x600 565 offscreen)", hr);

    for (int frame = 0; frame < 100 && back; ++frame) {
        DDSURFACEDESC ld{};
        ld.dwSize = sizeof(ld);
        hr = back->Lock(nullptr, &ld, DDLOCK_WAIT, nullptr);
        if (frame == 0) {
            Check(SUCCEEDED(hr) && ld.lpSurface, "Surface.Lock(back)", hr);
        }
        if (SUCCEEDED(hr)) {
            for (DWORD y = 0; y < 600; ++y) {
                auto *row = reinterpret_cast<WORD *>(static_cast<BYTE *>(ld.lpSurface) + y * ld.lPitch);
                for (DWORD x = 0; x < 800; ++x) {
                    row[x] = static_cast<WORD>(((x + frame) & 0x1F) << 11 | (y & 0x3F) << 5);
                }
            }
            back->Unlock(ld.lpSurface);
        }
        RECT dst;
        GetClientRect(hwnd, &dst);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT *>(&dst), 2);
        hr = primary->Blt(&dst, back, nullptr, DDBLT_WAIT, nullptr);
        if (frame == 0) {
            Check(SUCCEEDED(hr), "Surface.Blt(primary <- wrapped back)", hr);
        }
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageA(&m);
        }
        Sleep(5);
    }

    DDSURFACEDESC gd{};
    gd.dwSize = sizeof(gd);
    hr = back->GetSurfaceDesc(&gd);
    Check(SUCCEEDED(hr) && gd.dwWidth == 800 && gd.dwHeight == 600, "Surface.GetSurfaceDesc", hr);
    DDPIXELFORMAT pf{};
    pf.dwSize = sizeof(pf);
    hr = back->GetPixelFormat(&pf);
    Check(SUCCEEDED(hr) && pf.dwRGBBitCount == 16, "Surface.GetPixelFormat", hr);

    IDirectDrawSurface3 *s3 = nullptr;
    hr = back->QueryInterface(IID_IDirectDrawSurface3, reinterpret_cast<void **>(&s3));
    Check(SUCCEEDED(hr) && s3, "QueryInterface(IDirectDrawSurface3) escapes", hr);
    if (s3) {
        s3->Release();
    }
    IUnknown *unk = nullptr;
    hr = back->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&unk));
    Check(SUCCEEDED(hr) && unk == back, "QueryInterface(IUnknown) returns the wrapper", hr);
    if (unk) {
        unk->Release();
    }

    ULONG r;
    if (clip) {
        primary->SetClipper(nullptr);
        r = clip->Release();
        Check(r == 0, "Clipper final Release == 0", static_cast<HRESULT>(r));
    }
    r = back->Release();
    Check(r == 0, "back final Release == 0", static_cast<HRESULT>(r));
    r = primary->Release();
    Check(r == 0, "primary final Release == 0", static_cast<HRESULT>(r));
    r = dd->Release();
    Check(r == 0, "DirectDraw final Release == 0", static_cast<HRESULT>(r));

    DestroyWindow(hwnd);
    FreeLibrary(mod);
    // FreeLibrary must have removed the hooks: a CreateWindowExA now would crash if the detour were left behind.
    HWND after = CreateWindowExA(0, wc.lpszClassName, "after unload", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr,
                                 nullptr, wc.hInstance, nullptr);
    Check(after != nullptr, "CreateWindowExA after FreeLibrary (hooks removed)");
    DestroyWindow(after);
    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
