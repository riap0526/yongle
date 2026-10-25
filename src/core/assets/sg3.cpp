#include "core/assets/sg3.h"

#include "core/log.h"

#include <cstring>
#include <cwchar>

namespace core::assets {

namespace {

constexpr int kEntriesAt = 40680;
constexpr WORD kTransparent555 = 0x7c1f;
constexpr WORD kTransparent555b = 0xf81f; // China_Fonts' plain glyphs; bit 15 is unused in 555: never a colour

bool ReadAt(const wchar_t *path, long offset, void *out, DWORD size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD got = 0;
    bool ok = SetFilePointer(f, offset, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
              ReadFile(f, out, size, &got, nullptr) && got == size;
    CloseHandle(f);
    return ok;
}

WORD To565(WORD v)
{
    WORD r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
    return static_cast<WORD>((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}

} // namespace

bool Sg3Pack::Open(const wchar_t *sg3Path)
{
    entries_.clear();
    HANDLE f = CreateFileW(sg3Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        core_log("sg3: cannot open %ls (error %lu)", sg3Path, GetLastError());
        return false;
    }
    DWORD size = GetFileSize(f, nullptr);
    std::vector<BYTE> d(size);
    DWORD got = 0;
    bool ok = size > kEntriesAt && ReadFile(f, d.data(), size, &got, nullptr) && got == size;
    CloseHandle(f);
    if (!ok) {
        core_log("sg3: cannot read %ls", sg3Path);
        return false;
    }
    DWORD version, last;
    std::memcpy(&version, &d[4], 4);
    std::memcpy(&last, &d[16], 4);
    const int esz = 64 + (version >= 214 ? 8 : 0);
    const size_t count = static_cast<size_t>(last) + 1;
    if (count == 0 || kEntriesAt + count * esz > size) {
        core_log("sg3: %ls: %zu entries do not fit in %lu bytes (version %lu)", sg3Path, count, size, version);
        return false;
    }
    entries_.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const BYTE *o = &d[kEntriesAt + i * esz];
        Entry &e = entries_[i];
        std::memcpy(&e.offset, o, 4);
        std::memcpy(&e.length, o + 4, 4);
        short w, h;
        std::memcpy(&w, o + 20, 2);
        std::memcpy(&h, o + 22, 2);
        e.w = w;
        e.h = h;
        e.type = o[50];
        e.compressed = o[51];
        e.external = o[52];
    }
    // The pixels: the .555 file next to the .sg3.
    std::wcsncpy(pixels_, sg3Path, MAX_PATH - 1);
    wchar_t *dot = std::wcsrchr(pixels_, L'.');
    if (!dot || dot + 4 >= pixels_ + MAX_PATH) {
        return false;
    }
    std::wcscpy(dot, L".555");
    core_log("sg3: %ls: version %lu, %zu entries", sg3Path, version, count);
    return true;
}

bool Sg3Pack::Size(int index, int *w, int *h) const
{
    if (index < 0 || index >= Count()) {
        return false;
    }
    *w = entries_[index].w;
    *h = entries_[index].h;
    return true;
}

bool Sg3Pack::Decode565(int index, WORD transparent, std::vector<WORD> *out) const
{
    if (index < 0 || index >= Count()) {
        return false;
    }
    const Entry &e = entries_[index];
    if (e.external || e.w <= 0 || e.h <= 0 || e.length <= 0 || e.type == 30 || e.w * e.h > 4096 * 4096) {
        core_log("sg3: image %d is not a plain internal image (%dx%d, type %u, external %u)", index, e.w, e.h, e.type,
                 e.external);
        return false;
    }
    std::vector<BYTE> buf(e.length);
    if (!ReadAt(pixels_, e.offset, buf.data(), static_cast<DWORD>(e.length))) {
        core_log("sg3: cannot read image %d (%d bytes at %d) from %ls", index, e.length, e.offset, pixels_);
        return false;
    }
    out->assign(static_cast<size_t>(e.w) * e.h, transparent);
    auto put = [&](int x, int y, WORD v) { (*out)[static_cast<size_t>(y) * e.w + x] = v == kTransparent555 || v == kTransparent555b ? transparent : To565(v); };
    if (e.compressed) {
        int x = 0, y = 0;
        size_t k = 0;
        while (k < buf.size() && y < e.h) {
            BYTE c = buf[k++];
            if (c == 0xff) {
                if (k >= buf.size()) break;
                x += buf[k++];
                while (x >= e.w) {
                    x -= e.w;
                    ++y;
                }
            }
            else {
                for (int n = 0; n < c && y < e.h && k + 1 < buf.size(); ++n) {
                    WORD v = static_cast<WORD>(buf[k] | (buf[k + 1] << 8));
                    k += 2;
                    put(x, y, v);
                    if (++x >= e.w) {
                        x = 0;
                        ++y;
                    }
                }
            }
        }
    }
    else {
        if (buf.size() < static_cast<size_t>(e.w) * e.h * 2) {
            core_log("sg3: image %d: %zu bytes for %dx%d", index, buf.size(), e.w, e.h);
            return false;
        }
        for (int y = 0; y < e.h; ++y) {
            for (int x = 0; x < e.w; ++x) {
                size_t k = 2 * (static_cast<size_t>(y) * e.w + x);
                put(x, y, static_cast<WORD>(buf[k] | (buf[k + 1] << 8)));
            }
        }
    }
    return true;
}

} // namespace core::assets
