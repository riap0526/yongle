#include "core/ui/panel.h"

#include "core/config.h"
#include "core/log.h"
#include "core/output/sdl_output.h"
#include "core/ui/hotkey.h"
#include "core/ui/settings_view.h"

#include <imgui.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <SDL3/SDL.h>

#include <atomic>
#include <cfloat>
#include <cstring>
#include <windowsx.h>

namespace core::ui {

namespace {

// ---------------------------------------------------------------------------------------------
// Shared state

std::atomic<bool> g_open{false};
std::atomic<bool> g_abandoned{false}; // Abandon(): unavailable for the rest of the run

// Input queue: filled on the game thread (window messages), drained on the output thread before each panel frame.
struct Event {
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
    bool ctrl, shift, alt;
};
constexpr int kQueue = 256;
Event g_queue[kQueue];
int g_queued = 0;
SRWLOCK g_lock = SRWLOCK_INIT;

char g_title[64] = "Engine";
struct SectionEntry {
    char title[64];
    Section draw;
};
SectionEntry g_sections[8];
int g_sectionCount = 0;

// ---------------------------------------------------------------------------------------------
// Output thread only

SDL_Renderer *g_backendRenderer = nullptr; // the renderer the ImGui backend was initialised with
bool g_context = false;
float g_scale = 0;
LARGE_INTEGER g_freq{}, g_last{};

ImGuiKey KeyFor(WPARAM vk)
{
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case 'A': return ImGuiKey_A;
    case 'C': return ImGuiKey_C;
    case 'V': return ImGuiKey_V;
    case 'X': return ImGuiKey_X;
    case 'Y': return ImGuiKey_Y;
    case 'Z': return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

void Feed(const Event &e)
{
    ImGuiIO &io = ImGui::GetIO();
    switch (e.message) {
    case WM_MOUSEMOVE:
        io.AddMousePosEvent(static_cast<float>(GET_X_LPARAM(e.lparam)), static_cast<float>(GET_Y_LPARAM(e.lparam)));
        return;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: io.AddMouseButtonEvent(0, true); return;
    case WM_LBUTTONUP: io.AddMouseButtonEvent(0, false); return;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: io.AddMouseButtonEvent(1, true); return;
    case WM_RBUTTONUP: io.AddMouseButtonEvent(1, false); return;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: io.AddMouseButtonEvent(2, true); return;
    case WM_MBUTTONUP: io.AddMouseButtonEvent(2, false); return;
    case WM_MOUSEWHEEL:
        io.AddMouseWheelEvent(0, static_cast<float>(GET_WHEEL_DELTA_WPARAM(e.wparam)) / WHEEL_DELTA);
        return;
    case WM_MOUSEHWHEEL:
        io.AddMouseWheelEvent(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(e.wparam)) / WHEEL_DELTA, 0);
        return;
    case WM_CHAR:
        if (e.wparam >= 0x20 && e.wparam != 0x7f) {
            io.AddInputCharacterUTF16(static_cast<ImWchar16>(e.wparam));
        }
        return;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        bool down = e.message == WM_KEYDOWN || e.message == WM_SYSKEYDOWN;
        io.AddKeyEvent(ImGuiMod_Ctrl, e.ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, e.shift);
        io.AddKeyEvent(ImGuiMod_Alt, e.alt);
        ImGuiKey k = KeyFor(e.wparam);
        if (k != ImGuiKey_None) {
            io.AddKeyEvent(k, down);
        }
        return;
    }
    default:
        return;
    }
}

void DrainQueue()
{
    Event local[kQueue];
    int n;
    AcquireSRWLockExclusive(&g_lock);
    n = g_queued;
    std::memcpy(local, g_queue, sizeof(Event) * n);
    g_queued = 0;
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < n; ++i) {
        Feed(local[i]);
    }
}

// The panel is drawn at the window's real resolution. Its size (ui.panel_scale): auto = output height / 1080, at
// least 1 (1080p: 1.0, 1440p: 1.33, 2160p: 2.0); or 100..300 %.
bool g_recentre = false;

void ApplyScale(int oh)
{
    int percent = core_config_get()->panel_scale_percent;
    float s = percent > 0 ? percent / 100.0f : oh > 1080 ? oh / 1080.0f : 1.0f;
    if (s == g_scale) {
        return;
    }
    g_recentre = g_scale != 0;
    g_scale = s;
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.ScaleAllSizes(s);
    style.FontScaleMain = s;
    core_log("panel: scale %.2f for a %d px high output (%s)", s, oh, percent > 0 ? "ui.panel_scale" : "auto");
}

bool EnsureContext(SDL_Renderer *renderer)
{
    if (!g_context) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr; // nothing is written next to the game; settings live in the engine's ini
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        g_context = true;
        core_log("panel: Dear ImGui %s context created", ImGui::GetVersion());
    }
    if (g_backendRenderer != renderer) {
        if (g_backendRenderer) {
            ImGui_ImplSDLRenderer3_Shutdown();
            g_backendRenderer = nullptr;
        }
        if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
            core_log("panel: ImGui SDL_Renderer backend failed to start");
            return false;
        }
        g_backendRenderer = renderer;
    }
    return true;
}

void Contents()
{
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), g_recentre ? ImGuiCond_Always : ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    g_recentre = false;
    // At most 80% of the screen's height (and 95% of its width): more content scrolls inside the panel.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(screen.x * 0.95f, screen.y * 0.8f));
    bool open = true;
    if (ImGui::Begin(g_title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        const core_config *cfg = core_config_get();
        char key[40];
        FormatHotkey(Hotkey{cfg->panel_key_vk, cfg->panel_key_mods}, key, sizeof(key));
        ImGui::TextDisabled("%s or Esc closes the panel. The game is paused while it is open.", key);
        DrawSettings(g_scale);
        for (int i = 0; i < g_sectionCount; ++i) {
            if (ImGui::CollapsingHeader(g_sections[i].title, ImGuiTreeNodeFlags_DefaultOpen)) {
                g_sections[i].draw();
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Close")) {
            open = false;
        }
    }
    ImGui::End();
    if (!open) {
        PanelClosed();
        g_open = false; // read by the game thread on its next loop iteration
        core_log("panel: closed (button)");
    }
}

} // namespace

bool Toggle()
{
    if (g_abandoned.load()) {
        return false;
    }
    if (!core::output::Threaded()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            core_log("panel: needs display.present = display or cap60 (drawn by the output thread)");
        }
        return false;
    }
    bool now = !g_open.load();
    if (!now) {
        PanelClosed();
    }
    if (now) {
        AcquireSRWLockExclusive(&g_lock);
        g_queued = 0;
        ReleaseSRWLockExclusive(&g_lock);
    }
    g_open = now;
    core_log("panel: %s", now ? "opened" : "closed");
    return true;
}

bool IsOpen()
{
    return g_open.load();
}

void QueueMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    Event e{message, wparam, lparam, (GetKeyState(VK_CONTROL) & 0x8000) != 0, (GetKeyState(VK_SHIFT) & 0x8000) != 0,
            (GetKeyState(VK_MENU) & 0x8000) != 0};
    AcquireSRWLockExclusive(&g_lock);
    if (g_queued < kQueue) {
        g_queue[g_queued++] = e;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void Render(SDL_Renderer *renderer, int ow, int oh)
{
    if (!g_open.load() || ow <= 0 || oh <= 0 || !EnsureContext(renderer)) {
        return;
    }
    ApplyScale(oh);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ow), static_cast<float>(oh));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(g_freq.QuadPart);
    g_last = now;
    io.DeltaTime = dt > 0 && dt < 1 ? dt : 1.0f / 60;
    DrainQueue();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui::NewFrame();
    Contents();
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

void RenderCaption(SDL_Renderer *renderer, int ow, int oh, const char *utf8, float x, float y, float w, float h,
                   float fontPx, bool strip)
{
    if (!utf8 || !utf8[0] || ow <= 0 || oh <= 0 || !EnsureContext(renderer)) {
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ow), static_cast<float>(oh));
    io.DeltaTime = 1.0f / 1000; // a frame of its own; the panel's frame keeps its own timing
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui::NewFrame();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    const ImVec2 size = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, utf8);
    const float tx = x + (w - size.x) / 2;
    if (strip) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0, 0, 0, 150));
        dl->AddText(font, fontPx, ImVec2(tx, y + (h - size.y) / 2), IM_COL32(255, 255, 255, 255), utf8);
    }
    else {
        dl->AddRectFilled(ImVec2(tx, y), ImVec2(tx + size.x, y + size.y), IM_COL32(0, 0, 0, 255));
        dl->AddText(font, fontPx, ImVec2(tx, y), IM_COL32(255, 255, 255, 255), utf8);
    }
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

void RenderOverlay(SDL_Renderer *renderer, int ow, int oh, Section draw)
{
    if (!draw || ow <= 0 || oh <= 0 || !EnsureContext(renderer)) {
        return;
    }
    ApplyScale(oh);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ow), static_cast<float>(oh));
    io.DeltaTime = 1.0f / 1000; // a frame of its own; the panel's frame keeps its own timing
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(8, 48));
    ImGui::SetNextWindowBgAlpha(0.65f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##overlay", nullptr, flags)) {
        draw();
    }
    ImGui::End();
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

void Abandon()
{
    if (!g_abandoned.exchange(true)) {
        g_open = false;
        core_log("panel: closed after an exception; unavailable for the rest of the run");
    }
}

void RendererLost()
{
    if (g_backendRenderer) {
        ImGui_ImplSDLRenderer3_Shutdown();
        g_backendRenderer = nullptr;
    }
}

void SetTitle(const char *utf8)
{
    std::strncpy(g_title, utf8, sizeof(g_title) - 1);
}

void AddSection(const char *utf8Title, Section draw)
{
    if (g_sectionCount >= 8) {
        return;
    }
    SectionEntry &s = g_sections[g_sectionCount++];
    std::strncpy(s.title, utf8Title, sizeof(s.title) - 1);
    s.draw = draw;
}

} // namespace core::ui
