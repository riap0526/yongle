/*
 * Smoke test for the proxy: loads the given ddraw.dll by path, calls
 * DirectDrawCreate(NULL, &dd, NULL) and releases the object. Does not create
 * a window, set a cooperative level or change the display mode.
 * Usage: test_load.exe <path-to-proxy-ddraw.dll>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef HRESULT (WINAPI *DirectDrawCreate_fn)(void *guid, void **dd, void *outer);

int main(int argc, char **argv)
{
    HMODULE mod;
    DirectDrawCreate_fn create;
    void **dd = NULL;
    HRESULT hr;
    char name[MAX_PATH];

    if (argc < 2) {
        fprintf(stderr, "usage: %s <ddraw.dll>\n", argv[0]);
        return 2;
    }
    mod = LoadLibraryA(argv[1]);
    if (!mod) {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    GetModuleFileNameA(mod, name, MAX_PATH);
    printf("loaded %s at %p\n", name, (void *)mod);

    create = (DirectDrawCreate_fn)GetProcAddress(mod, "DirectDrawCreate");
    if (!create) {
        printf("GetProcAddress(DirectDrawCreate) failed: %lu\n", GetLastError());
        return 1;
    }
    hr = create(NULL, (void **)&dd, NULL);
    printf("DirectDrawCreate hr=0x%08lX obj=%p\n", (unsigned long)hr, (void *)dd);
    if (SUCCEEDED(hr) && dd) {
        /* IUnknown::Release is vtable slot 2. */
        typedef ULONG (WINAPI *Release_fn)(void *self);
        Release_fn release = (Release_fn)((void **)*dd)[2];
        printf("Release -> %lu\n", release(dd));
    }
    FreeLibrary(mod);
    return SUCCEEDED(hr) ? 0 : 1;
}
