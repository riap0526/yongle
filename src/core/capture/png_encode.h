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

// Streaming deflate with a zlib header (the data of a PNG's IDAT chunks), for pictures too large to hold encoded in
// memory. sink gets the compressed bytes as they come (returns nonzero to go on). core_deflate_add: finish = 0 while
// more follows, 1 with the last part (size may be 0); returns nonzero on success. core_crc32: start with crc 0.
typedef int (*core_deflate_sink)(const void *data, int size, void *user);
void *core_deflate_begin(unsigned level, core_deflate_sink sink, void *user);
int core_deflate_add(void *deflate, const void *data, size_t size, int finish);
void core_deflate_end(void *deflate);
unsigned long core_crc32(unsigned long crc, const void *data, size_t size);

#ifdef __cplusplus
}
#endif
