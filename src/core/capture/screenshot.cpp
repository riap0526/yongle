#include "core/capture/screenshot.h"

#include "core/capture/png_encode.h"
#include "core/diag/environment.h"
#include "core/log.h"
#include "core/output/sdl_output.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>

#include <atomic>
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
    DWORD queued;         // GetTickCount when handed to the writer
    char note[48];        // e.g. "read back 12 ms, " for the screen picture
    Job *next;            // the writer's queue
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

void Drop(Job *job);

// Encodes the picture as PNG in memory, then writes the file; logs the time of each step (O1b).
// The calling thread's processor time so far (kernel + user), in ms.
ULONGLONG ThreadCpuMs()
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) {
        return 0;
    }
    const ULONGLONG t = (static_cast<ULONGLONG>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                        (static_cast<ULONGLONG>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
    return t / 10000;
}

// miniz level 1 (O1b, 2026-10-30): the whole process runs on one logical processor (GOG's SingleProcAffinity,
// docs/optimization.md section 1.7), so the writer takes its time from the game; SDL_SavePNG_IO's level 6 took
// 0.7-0.9 s of processor time per 2560x1440 picture.
constexpr unsigned kPngLevel = 1;

void Write(Job *job)
{
    const DWORD start = GetTickCount();
    const ULONGLONG cpu0 = ThreadCpuMs();
    bool ok = false;
    long kb = 0;
    DWORD encodeMs = 0, writeMs = 0;
    SDL_Surface *rgb = job->surface ? SDL_ConvertSurface(job->surface, SDL_PIXELFORMAT_RGB24) : nullptr;
    size_t size = 0;
    void *data = rgb ? core_png_encode(rgb->pixels, rgb->w, rgb->h, 3, rgb->pitch, kPngLevel, &size) : nullptr;
    if (rgb) {
        SDL_DestroySurface(rgb);
    }
    if (rgb && !data) {
        SDL_SetError("PNG encoding failed");
    }
    if (data) {
        encodeMs = GetTickCount() - start;
        wchar_t wpath[1024];
        if (size > 0 && MultiByteToWideChar(CP_UTF8, 0, job->path, -1, wpath, 1024)) {
            const DWORD t = GetTickCount();
            HANDLE f = CreateFileW(wpath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written = 0;
            ok = f != INVALID_HANDLE_VALUE && WriteFile(f, data, static_cast<DWORD>(size), &written, nullptr) &&
                 written == static_cast<DWORD>(size);
            const DWORD error = GetLastError();
            if (f != INVALID_HANDLE_VALUE) {
                CloseHandle(f);
            }
            if (!ok) {
                SDL_SetError("writing the file failed (error %lu)", error);
            }
            writeMs = GetTickCount() - t;
            kb = static_cast<long>((size + 1023) / 1024);
        }
        core_png_free(data);
    }
    if (ok) {
        // processor time against the time it took (O1, 2026-10-30): a picture that took long with little
        // processor time waited for a processor; one with as much ran slowly
        core_log("screenshot: saved %s (%dx%d, %ld KB at PNG level %u; %swaited %lu ms, encoded %lu ms, written %lu ms; "
                 "this picture took %lu ms, %lu ms of it on a processor)",
                 job->name, job->surface->w, job->surface->h, kb, kPngLevel, job->note, start - job->queued, encodeMs,
                 writeMs, GetTickCount() - start, static_cast<unsigned long>(ThreadCpuMs() - cpu0));
    }
    else {
        core_log("screenshot: could not save %s: %s", job->name, SDL_GetError());
    }
    Drop(job);
}

// One writer thread for all pictures, one at a time, at normal priority: encoding a large PNG takes seconds, and three
// at once on their own threads slowed the game on a laptop three times over (O1b, docs/optimization.md section 1.2).
// A lower priority made each picture slow while the game ran: Windows' background mode 26-200 s for 2560x1440 (second
// test, 2026-10-28), THREAD_PRIORITY_LOWEST 13-18 s (third test, 2026-10-30), against 0.8 s at normal priority: the
// whole process runs on one logical processor (section 1.7), so a lower priority only runs while the game waits.
// Started on the first picture; it lives as long as the game.
SRWLOCK g_queueLock = SRWLOCK_INIT;
Job *g_queueHead = nullptr, *g_queueTail = nullptr;
HANDLE g_queueEvent = nullptr; // auto-reset: a job was queued
bool g_writerFailed = false;   // no writer thread: jobs are written on the calling thread
std::atomic<long> g_pending{0}; // jobs queued or being written

DWORD WINAPI Writer(void *)
{
    core::diag::env::RegisterThread(core::diag::env::Thread::Writer); // its affinity and processor time in the log
    for (;;) {
        WaitForSingleObject(g_queueEvent, INFINITE);
        for (;;) {
            AcquireSRWLockExclusive(&g_queueLock);
            Job *job = g_queueHead;
            if (job) {
                g_queueHead = job->next;
                if (!g_queueHead) {
                    g_queueTail = nullptr;
                }
            }
            ReleaseSRWLockExclusive(&g_queueLock);
            if (!job) {
                break;
            }
            Write(job);
            g_pending.fetch_sub(1);
        }
    }
}

// At most this many pictures queued or being written (each holds a copy of the frame, about 15 MB at 2560x1440): a
// picture asked for beyond it is not saved (logged). 2026-10-29, after F12 pressed many times took the memory
// from 367 to 511 MB.
constexpr long kMaxQueued = 6;

} // namespace

bool HasRoom(const char *what)
{
    const long pending = g_pending.load();
    if (pending < kMaxQueued) {
        return true;
    }
    core_log("screenshot: %s not saved: %ld pictures are still being written (at most %ld at a time)", what, pending,
             kMaxQueued);
    return false;
}

namespace {

// Frees a job that is not written.
void Drop(Job *job)
{
    if (job->surface) {
        SDL_DestroySurface(job->surface);
    }
    std::free(job->pixels);
    std::free(job);
}

// Queues the job for the writer (or writes it here when no writer can be started).
void Start(Job *job)
{
    if (g_pending.load() >= kMaxQueued) {
        core_log("screenshot: %s not saved: %ld pictures are still being written (at most %ld at a time)", job->name,
                 g_pending.load(), kMaxQueued);
        Drop(job);
        return;
    }
    job->queued = GetTickCount();
    job->next = nullptr;
    AcquireSRWLockExclusive(&g_queueLock);
    if (!g_queueEvent && !g_writerFailed) {
        g_queueEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE th = g_queueEvent ? CreateThread(nullptr, 0, &Writer, nullptr, 0, nullptr) : nullptr;
        if (th) {
            CloseHandle(th);
        }
        else {
            g_writerFailed = true;
            core_log("screenshot: no writer thread (error %lu): pictures are written on the calling thread",
                     GetLastError());
        }
    }
    const bool here = g_writerFailed;
    if (!here) {
        (g_queueTail ? g_queueTail->next : g_queueHead) = job;
        g_queueTail = job;
        g_pending.fetch_add(1);
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    if (here) {
        Write(job);
    }
    else {
        SetEvent(g_queueEvent);
    }
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
    char what[200];
    std::snprintf(what, sizeof(what), "%s_%s_%dx%d_%s", stem, kind, w, h, time);
    if (!HasRoom(what)) { // before the frame is copied (O1, 2026-10-30)
        return false;
    }
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

bool Save(const void *pixels, int pitch, int w, int h, const char *label, bool screen)
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
    if (screen) {
        core::output::RequestScreenCapture(stem, time);
    }
    else {
        core_log("screenshot: %s_screen_%s not saved: the screen shows the game picture 1:1 with nothing around it "
                 "(the same as the game picture)", stem, time);
    }
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
    if (!wait && !HasRoom(fileName)) {
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
        job->queued = GetTickCount();
        Write(job);
    }
    else {
        Start(job);
    }
    return true;
}

int Flush(unsigned long timeoutMs)
{
    const long pending = g_pending.load();
    if (pending <= 0) {
        return 0;
    }
    const DWORD start = GetTickCount();
    while (g_pending.load() > 0 && GetTickCount() - start < timeoutMs) {
        Sleep(20);
    }
    const long left = g_pending.load();
    if (left > 0) {
        core_log("screenshot: the game ends with %ld of %ld pictures not written after %lu ms; they are lost", left,
                 pending, GetTickCount() - start);
    }
    else {
        core_log("screenshot: waited %lu ms at the end of the game for %ld pictures to be written",
                 GetTickCount() - start, pending);
    }
    return static_cast<int>(left);
}

void SaveScreen(SDL_Surface *surface, const char *stem, const char *time, unsigned long readBackMs)
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
    std::snprintf(job->note, sizeof(job->note), "read back %lu ms, ", readBackMs);
    Start(job);
}

} // namespace core::capture
