// See png_encode.h. The defines are those of SDL 3.4.18's src/video/SDL_stb.c before it includes miniz.h.
#include "core/capture/png_encode.h"

#include <SDL3/SDL.h>

#define MZ_ASSERT(x) SDL_assert(x)
#define MINIZ_LITTLE_ENDIAN 1
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#define MINIZ_SDL_NOUNUSED
#include "miniz.h"

void *core_png_encode(const void *pixels, int w, int h, int channels, int pitch, unsigned level, size_t *size)
{
    *size = 0;
    return tdefl_write_image_to_png_file_in_memory_ex(pixels, w, h, channels, pitch, size, level, MZ_FALSE, NULL, 0,
                                                      NULL, 0);
}

void core_png_free(void *png)
{
    mz_free(png);
}
