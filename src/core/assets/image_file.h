// Picture files for backgrounds (R2d.1: the front-end screens' city background): PNG or BMP, read with SDL3's own
// loaders (SDL_LoadPNG, SDL_LoadBMP; no other library). Nothing here knows a game.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace core::assets {

// Reads the picture at path (.png or .bmp, by its extension) as RGB565, tightly packed. A picture larger than needed
// is first reduced (bilinear, keeping its aspect) to the smallest size that still covers coverW x coverH, so a very
// large file does not stay in memory. False (logged) when the file is missing, unreadable or of another kind.
bool LoadPicture565(const wchar_t *path, int coverW, int coverH, std::vector<WORD> *px, int *w, int *h);

} // namespace core::assets
