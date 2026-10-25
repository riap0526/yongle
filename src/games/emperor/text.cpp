#include "games/emperor/text.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace emperor::text {

void GameToUtf8(const char *game, char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    if (!game) {
        return;
    }
    wchar_t wide[512];
    int n = MultiByteToWideChar(1252, 0, game, -1, wide, 512);
    if (n <= 0) {
        return;
    }
    wide[511] = L'\0';
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(size), nullptr, nullptr)) {
        out[0] = '\0';
    }
    out[size - 1] = '\0';
}

} // namespace emperor::text
