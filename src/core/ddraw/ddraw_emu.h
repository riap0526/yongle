// Our own DirectDraw (IDirectDraw v1 / IDirectDrawSurface v1), replacing the system ddraw.dll: surfaces are
// plain memory, the display mode is emulated (core/display/virtual_display.h) and frames reach the screen through
// SDL3 (core/output). Part of core: no game addresses here.
//
// Implemented: the methods the game was seen to use (docs/testing/ddraw_wrapper_v1.md, 14 of them) plus IUnknown and
// IDirectDrawSurface::Restore, which the game calls on its surface-lost paths. Only 16-bit RGB565 surfaces.
// Every other method is logged as UNIMPLEMENTED and returns DDERR_UNSUPPORTED. Surfaces are never lost.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DirectDrawCreate for the emulated back end. caller is the return address in the game (for the log). */
HRESULT core_ddraw_emu_create(GUID *guid, void **dd, IUnknown *outer, void *caller);

#ifdef __cplusplus
}
#endif
