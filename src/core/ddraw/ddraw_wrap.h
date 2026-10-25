// DirectDraw wrapper, version 1: logging only.
// Returns our own IDirectDraw / IDirectDrawSurface / IDirectDrawClipper / IDirectDrawPalette wrapper
// objects, forwards every call to the real system ddraw.dll unchanged, and logs which vtable methods are
// called with which arguments. Part of core: no game addresses here.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef HRESULT(WINAPI *core_DirectDrawCreate_fn)(GUID *guid, void **dd, IUnknown *outer);

/* Calls real_create, then wraps the returned IDirectDraw. caller is the return address in the game. */
HRESULT core_ddraw_create(core_DirectDrawCreate_fn real_create, GUID *guid, void **dd, IUnknown *outer,
                          void *caller);

/* Writes the per-method call statistics of both back ends to the log (defined in ddraw_log.cpp). */
void core_ddraw_write_summary(const char *reason);

#ifdef __cplusplus
}
#endif
