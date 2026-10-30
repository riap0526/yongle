// PNG encoding with a chosen compression level (O1b, 2026-10-30). SDL_SavePNG_IO always compresses at miniz level
// 6; this is the same miniz (third_party/miniz, the copy in SDL 3.4.18) built the way SDL builds it, with the level
// given. Nothing here knows a game.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A whole PNG file in memory (free it with core_png_free), or NULL. channels 3 (RGB) or 4 (RGBA), 8 bits each; pitch in
// bytes; level 0 (stored) to 10 (slowest).
void *core_png_encode(const void *pixels, int w, int h, int channels, int pitch, unsigned level, size_t *size);
void core_png_free(void *png);

#ifdef __cplusplus
}
#endif
