#include "core/capture/screenshot.h"

#include "core/log.h"
#include "core/output/sdl_output.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core::capture {

namespace {

TagProvider g_tag = nullptr;

struct Job {
    SDL_Surface *surface; // owned: destroyed after writing
    char name[160];       // shots\<file>, for the log
    char path[1024];      // UTF-8, full path
    BYTE *pixels;         // the game frame's copy (freed after writing), or nullptr
};

// The folder of this DLL, as UTF-8 with a trailing backslash ("" when unknown: the current directory).
void ModuleFolder(char *out, size_t size)
{
    out[0] = '\0';
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleFolder), &self)) {
        return;
    }
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    while (len > 0 && path[len - 1] != L'\\' && path[len - 1] != L'/') {
        --len;
    }
    path[len] = L'\0';
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, out, static_cast<int>(size), nullptr, nullptr)) {
        out[0] = '\0';
    }
}

// shots\ next to the DLL: its full path with a trailing backslash; false (logged) when it cannot be created.
bool ShotsFolder(char *out, size_t size)
{
    char folder[900];
    ModuleFolder(folder, sizeof(folder));
    std::snprintf(out, size, "%sshots", folder);
    wchar_t wdir[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, out, -1, wdir, 1024) && !CreateDirectoryW(wdir, nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        core_log("screenshot: cannot create %s (error %lu)", out, GetLastError());
        return false;
    }
    std::strncat(out, "\\", size - std::strlen(out) - 1);
    return true;
}

DWORD WINAPI Write(void *param)
{
    auto *job = static_cast<Job *>(param);
    DWORD start = GetTickCount();
    bool ok = job->surface && SDL_SavePNG(job->surface, job->path);
    if (ok) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        wchar_t wpath[1024];
        long kb = 0;
        if (MultiByteToWideChar(CP_UTF8, 0, job->path, -1, wpath, 1024) &&
            GetFileAttributesExW(wpath, GetFileExInfoStandard, &fa)) {
            kb = static_cast<long>((fa.nFileSizeLow + 1023) / 1024);
        }
        core_log("screenshot: saved %s (%dx%d, %ld KB, %lu ms)", job->name, job->surface->w, job->surface->h, kb,
                 GetTickCount() - start);
    }
    else {
        core_log("screenshot: could not save %s: %s", job->name, SDL_GetError());
    }
    if (job->surface) {
        SDL_DestroySurface(job->surface);
    }
    std::free(job->pixels);
    std::free(job);
    return 0;
}

// Writes the job on a worker thread (or here when no thread can be started).
void Start(Job *job)
{
    HANDLE th = CreateThread(nullptr, 0, &Write, job, 0, nullptr);
    if (!th) {
        Write(job);
        return;
    }
    CloseHandle(th);
}

Job *NewJob(const char *stem, const char *kind, int w, int h, const char *time, const char *folder)
{
    auto *job = static_cast<Job *>(std::calloc(1, sizeof(Job)));
    if (!job) {
        return nullptr;
    }
    std::snprintf(job->name, sizeof(job->name), "shots\\%s_%s_%dx%d_%s.png", stem, kind, w, h, time);
    std::snprintf(job->path, sizeof(job->path), "%s%s_%s_%dx%d_%s.png", folder, stem, kind, w, h, time);
    return job;
}

} // namespace

void SetTagProvider(TagProvider provider)
{
    g_tag = provider;
}

namespace {

// <label>_<tag> (or <tag>) with anything but letters, digits, '_', '-' and '.' made '_'.
void Stem(const char *label, char *out, size_t size)
{
    char tag[48] = "frame";
    if (g_tag) {
        g_tag(tag, sizeof(tag));
    }
    if (label && *label) {
        std::snprintf(out, size, "%s_%s", label, tag);
    }
    else {
        std::snprintf(out, size, "%s", tag);
    }
    for (char *c = out; *c; ++c) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' ||
                  *c == '-' || *c == '.';
        if (!ok) {
            *c = '_';
        }
    }
}

// The time part of the names: two pictures within the same millisecond get -2, -3, ... (names never collide).
void TimeStamp(char *time, size_t size)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::snprintf(time, size, "%04u%02u%02u-%02u%02u%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond, t.wMilliseconds);
    {
        static SRWLOCK lock = SRWLOCK_INIT;
        static char last[32] = "";
        static int sequence = 1;
        AcquireSRWLockExclusive(&lock);
        if (std::strcmp(time, last) == 0) {
            char base[32];
            std::memcpy(base, time, sizeof(base));
            std::snprintf(time, size, "%s-%d", base, ++sequence);
        }
        else {
            std::snprintf(last, sizeof(last), "%s", time);
            sequence = 1;
        }
        ReleaseSRWLockExclusive(&lock);
    }
}

// Copies the picture and writes it on a worker thread. False (logged) when that could not be started.
bool Start(const void *pixels, int pitch, int w, int h, const char *stem, const char *kind, const char *time)
{
    char folder[1024];
    if (!ShotsFolder(folder, sizeof(folder))) {
        return false;
    }
    Job *job = NewJob(stem, kind, w, h, time, folder);
    size_t bytes = static_cast<size_t>(pitch) * h;
    if (job) {
        job->pixels = static_cast<BYTE *>(std::malloc(bytes));
    }
    if (!job || !job->pixels) {
        if (job) {
            std::free(job);
        }
        core_log("screenshot: out of memory (%dx%d)", w, h);
        return false;
    }
    std::memcpy(job->pixels, pixels, bytes);
    job->surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGB565, job->pixels, pitch);
    Start(job);
    return true;
}

} // namespace

bool Save(const void *pixels, int pitch, int w, int h, const char *label)
{
    if (!pixels || w <= 0 || h <= 0 || pitch < w * 2) {
        core_log("screenshot: no picture to save yet");
        return false;
    }
    char stem[96], time[32];
    Stem(label, stem, sizeof(stem));
    TimeStamp(time, sizeof(time));
    if (!Start(pixels, pitch, w, h, stem, "game", time)) {
        return false;
    }
    core::output::RequestScreenCapture(stem, time);
    return true;
}

bool SaveOne(const void *pixels, int pitch, int w, int h, const char *label, const char *kind)
{
    if (!pixels || w <= 0 || h <= 0 || pitch < w * 2) {
        core_log("screenshot: no %s picture to save", kind);
        return false;
    }
    char stem[96], time[32];
    Stem(label, stem, sizeof(stem));
    TimeStamp(time, sizeof(time));
    return Start(pixels, pitch, w, h, stem, kind, time);
}

bool SaveAs(const void *pixels, int pitch, int w, int h, const char *fileName, bool wait)
{
    if (!pixels || w <= 0 || h <= 0 || pitch < w * 2 || !fileName || !*fileName) {
        return false;
    }
    char folder[900];
    ModuleFolder(folder, sizeof(folder));
    auto *job = static_cast<Job *>(std::calloc(1, sizeof(Job)));
    const size_t bytes = static_cast<size_t>(pitch) * h;
    if (job) {
        job->pixels = static_cast<BYTE *>(std::malloc(bytes));
    }
    if (!job || !job->pixels) {
        std::free(job);
        core_log("screenshot: out of memory for %s (%dx%d)", fileName, w, h);
        return false;
    }
    std::snprintf(job->name, sizeof(job->name), "%s", fileName);
    std::snprintf(job->path, sizeof(job->path), "%s%s", folder, fileName);
    std::memcpy(job->pixels, pixels, bytes);
    job->surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGB565, job->pixels, pitch);
    if (wait) {
        Write(job);
    }
    else {
        Start(job);
    }
    return true;
}

void SaveScreen(SDL_Surface *surface, const char *stem, const char *time)
{
    char folder[1024];
    Job *job = surface && ShotsFolder(folder, sizeof(folder)) ? NewJob(stem, "screen", surface->w, surface->h, time,
                                                                        folder)
                                                               : nullptr;
    if (!job) {
        core_log("screenshot: screen picture for %s_%s not saved", stem, time);
        if (surface) {
            SDL_DestroySurface(surface);
        }
        return;
    }
    job->surface = surface;
    Start(job);
}

} // namespace core::capture
