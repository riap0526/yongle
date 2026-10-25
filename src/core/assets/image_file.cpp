#include "core/assets/image_file.h"

#include "core/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace core::assets {

bool LoadPicture565(const wchar_t *path, int coverW, int coverH, std::vector<WORD> *px, int *w, int *h)
{
    char utf8[1024];
    if (!path || !WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr)) {
        return false;
    }
    const size_t n = std::wcslen(path);
    const bool bmp = n > 4 && _wcsicmp(path + n - 4, L".bmp") == 0;
    SDL_Surface *src = bmp ? SDL_LoadBMP(utf8) : SDL_LoadPNG(utf8);
    if (!src) {
        core_log("picture: cannot read %s: %s", utf8, SDL_GetError());
        return false;
    }
    SDL_Surface *scaled = src;
    if (coverW > 0 && coverH > 0) {
        const double k = std::max(static_cast<double>(coverW) / src->w, static_cast<double>(coverH) / src->h);
        if (k < 1.0) {
            const int sw = std::max(coverW, static_cast<int>(std::lround(src->w * k)));
            const int sh = std::max(coverH, static_cast<int>(std::lround(src->h * k)));
            scaled = SDL_ScaleSurface(src, sw, sh, SDL_SCALEMODE_LINEAR);
            if (!scaled) {
                core_log("picture: cannot reduce %s to %dx%d: %s", utf8, sw, sh, SDL_GetError());
                SDL_DestroySurface(src);
                return false;
            }
        }
    }
    SDL_Surface *c = SDL_ConvertSurface(scaled, SDL_PIXELFORMAT_RGB565);
    const int srcW = src->w, srcH = src->h;
    if (scaled != src) {
        SDL_DestroySurface(scaled);
    }
    SDL_DestroySurface(src);
    if (!c) {
        core_log("picture: cannot convert %s: %s", utf8, SDL_GetError());
        return false;
    }
    px->resize(static_cast<size_t>(c->w) * c->h);
    for (int y = 0; y < c->h; ++y) {
        std::memcpy(px->data() + static_cast<size_t>(y) * c->w, static_cast<const BYTE *>(c->pixels) + y * c->pitch,
                    static_cast<size_t>(c->w) * 2);
    }
    *w = c->w;
    *h = c->h;
    core_log("picture: %s read (%dx%d%s)", utf8, srcW, srcH,
             c->w != srcW ? ", reduced to cover the screen" : "");
    SDL_DestroySurface(c);
    return true;
}

} // namespace core::assets
