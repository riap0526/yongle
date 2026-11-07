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

// City picture (docs/archive/city_export.md section 4.4): the same deflate, fed a little at a time.
void *core_deflate_begin(unsigned level, core_deflate_sink sink, void *user)
{
    // miniz's PNG writer (tdefl_write_image_to_png_file_in_memory_ex) uses these probes for each level
    static const mz_uint probes[11] = {0, 1, 6, 32, 16, 32, 128, 256, 512, 768, 1500};
    tdefl_compressor *d = (tdefl_compressor *)MZ_MALLOC(sizeof(tdefl_compressor));
    if (!d) {
        return NULL;
    }
    if (tdefl_init(d, (tdefl_put_buf_func_ptr)sink, user, (int)(probes[MZ_MIN(10, level)] | TDEFL_WRITE_ZLIB_HEADER)) !=
        TDEFL_STATUS_OKAY) {
        MZ_FREE(d);
        return NULL;
    }
    return d;
}

int core_deflate_add(void *d, const void *data, size_t size, int finish)
{
    const tdefl_status s =
        tdefl_compress_buffer((tdefl_compressor *)d, data, size, finish ? TDEFL_FINISH : TDEFL_NO_FLUSH);
    return finish ? s == TDEFL_STATUS_DONE : s == TDEFL_STATUS_OKAY;
}

void core_deflate_end(void *d)
{
    if (d) {
        MZ_FREE(d);
    }
}

unsigned long core_crc32(unsigned long crc, const void *data, size_t size)
{
    return (unsigned long)mz_crc32((mz_ulong)crc, (const unsigned char *)data, size);
}
