#include "core/ui/settings_view.h"

#include "core/display/common_sizes.h"
#include "core/display/logical_size.h"
#include "core/display/virtual_display.h"
#include "core/log.h"
#include "core/settings/settings.h"

#include <imgui.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core::ui {

namespace {

namespace cs = core::settings;
using cs::Effect;
using cs::Type;

constexpr int kMax = 48;
constexpr DWORD kConfirmMs = 15000;
constexpr DWORD kGraceMs = 1500; // frames after the answer, so a revert is drawn too

// Drafts: what the widgets show while the user edits (output thread).
char g_draft[kMax][cs::kValueSize];
bool g_dirty[kMax];

// "Keep these settings?": the values before the change (output thread, PanelClosed from the game thread).
SRWLOCK g_lock = SRWLOCK_INIT;
struct Confirm {
    bool pending = false;
    DWORD until = 0;
    int count = 0;
    int index[8];
    char before[8][cs::kValueSize];
} g_confirm;
std::atomic<DWORD> g_framesUntil{0};
std::atomic<bool> g_framesPending{false};
bool g_askDefaults = false;

void Revert(const char *why)
{
    AcquireSRWLockExclusive(&g_lock);
    Confirm c = g_confirm;
    g_confirm.pending = false;
    ReleaseSRWLockExclusive(&g_lock);
    if (!c.pending) {
        return;
    }
    for (int k = 0; k < c.count; ++k) {
        cs::Request(c.index[k], c.before[k], false); // the file still has these values
        g_dirty[c.index[k]] = false;
    }
    g_framesUntil = GetTickCount() + kGraceMs;
    g_framesPending = false;
    core_log("panel: display settings reverted (%s)", why);
}

void Keep()
{
    AcquireSRWLockExclusive(&g_lock);
    bool was = g_confirm.pending;
    g_confirm.pending = false;
    ReleaseSRWLockExclusive(&g_lock);
    if (was) {
        cs::RequestSave();
        g_framesUntil = GetTickCount() + kGraceMs;
        g_framesPending = false;
        core_log("panel: display settings kept");
    }
}

// Applies the drafts of the re-layout items and asks to keep them.
void ApplyRelayout()
{
    Confirm c;
    for (int i = 0; i < cs::Count() && c.count < 8; ++i) {
        const cs::Item &it = cs::Get(i);
        if (it.effect != Effect::Relayout || !g_dirty[i]) {
            continue;
        }
        char now[cs::kValueSize];
        cs::Value(i, now, sizeof(now));
        g_dirty[i] = false;
        if (std::strcmp(now, g_draft[i]) == 0) {
            continue;
        }
        c.index[c.count] = i;
        std::snprintf(c.before[c.count], cs::kValueSize, "%s", now);
        ++c.count;
        cs::Request(i, g_draft[i], false);
    }
    if (c.count == 0) {
        return;
    }
    c.pending = true;
    c.until = GetTickCount() + kConfirmMs;
    AcquireSRWLockExclusive(&g_lock);
    g_confirm = c;
    ReleaseSRWLockExclusive(&g_lock);
    g_framesPending = true;
}

void Commit(int i)
{
    const cs::Item &it = cs::Get(i);
    if (it.effect == Effect::Relayout) {
        ApplyRelayout();
        return;
    }
    g_dirty[i] = false;
    cs::Request(i, g_draft[i], true);
}

const char *EffectTag(const cs::Item &it)
{
    switch (it.effect) {
    case Effect::Live: return "applies at once";
    case Effect::Relayout: return "re-layout, asks to keep";
    case Effect::Restart: return "after a restart";
    }
    return "";
}

void Help(const cs::Item &it)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(it.help);
        ImGui::Text("[%s] %s", it.section, it.key);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Int with an optional "auto": a checkbox and a slider (released: commit).
void IntWidget(int i, const cs::Item &it, const char *id, float width, const char *format)
{
    bool isAuto = it.autoWord && std::strcmp(g_draft[i], it.autoWord) == 0;
    int v = isAuto ? it.min : std::atoi(g_draft[i]);
    if (it.autoWord) {
        bool a = isAuto;
        char label[48];
        std::snprintf(label, sizeof(label), "Auto##%s", id);
        if (ImGui::Checkbox(label, &a)) {
            if (a) {
                std::snprintf(g_draft[i], cs::kValueSize, "%s", it.autoWord);
            }
            else {
                std::snprintf(g_draft[i], cs::kValueSize, "%d", v);
            }
            g_dirty[i] = true;
            Commit(i);
        }
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(isAuto);
    ImGui::SetNextItemWidth(width);
    char sid[48];
    std::snprintf(sid, sizeof(sid), "##%s", id);
    if (ImGui::SliderInt(sid, &v, it.min, it.max, format, ImGuiSliderFlags_AlwaysClamp)) {
        if (it.step > 1) {
            v = it.min + (v - it.min + it.step / 2) / it.step * it.step;
        }
        std::snprintf(g_draft[i], cs::kValueSize, "%d", v);
        g_dirty[i] = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Commit(i);
    }
    ImGui::EndDisabled();
}

// display.resolution: the game's own size, scaled, a common size from a list (R2b.1-9), or a custom size. The game's
// own size, scaled and a common size apply at once (with the question whether to keep it); a custom size with the
// Apply button. All are stored the same way ("1024x768", "scaled", "1920x1080").
bool g_allAspects = false; // the common sizes of every aspect, not only the screen's (not saved)

void ResolutionWidget(int i, float width)
{
    static const char *const kKinds[] = {"1024x768 (the game's own size)", "Scaled (native / display scale)",
                                         "Common size", "Custom size"};
    const bool scaled = std::strcmp(g_draft[i], "scaled") == 0;
    int w = 1024, h = 768;
    if (!scaled) {
        w = std::atoi(g_draft[i]);
        const char *x = std::strchr(g_draft[i], 'x');
        h = x ? std::atoi(x + 1) : 768;
    }
    const SIZE n = core::display::NativeSize();
    int minW = 0, minH = 0;
    core::display::GameMinimum(&minW, &minH);
    core::display::CommonSize sizes[48];
    const int count = core::display::CommonSizes(n.cx, n.cy, minW, minH, g_allAspects, sizes, 48);
    int listed = -1;
    for (int k = 0; k < count; ++k) {
        if (sizes[k].w == w && sizes[k].h == h) {
            listed = k;
        }
    }
    int kind = scaled ? 1 : (w == 1024 && h == 768) ? 0 : listed >= 0 ? 2 : 3;
    // The kind picked in the box shows until the value is changed some other way (applied, reverted, defaults):
    // "Common size" before a size is picked from the list, "Custom size" while a size is typed.
    static int picked = -1;
    static char pickedValue[cs::kValueSize] = "";
    if (picked >= 0 && std::strcmp(pickedValue, g_draft[i]) != 0 && !g_dirty[i]) {
        picked = -1;
    }
    if (picked >= 0) {
        kind = picked;
    }
    ImGui::SetNextItemWidth(width);
    if (ImGui::Combo("##resolution", &kind, kKinds, 4)) {
        picked = kind;
        if (kind == 0) {
            std::snprintf(g_draft[i], cs::kValueSize, "1024x768");
        }
        else if (kind == 1) {
            std::snprintf(g_draft[i], cs::kValueSize, "scaled");
        }
        else if (kind == 3) {
            std::snprintf(g_draft[i], cs::kValueSize, "%ldx%ld", n.cx, n.cy);
        }
        std::snprintf(pickedValue, sizeof(pickedValue), "%s", g_draft[i]);
        if (kind == 0 || kind == 1) {
            g_dirty[i] = true;
            Commit(i);
        }
        else if (kind == 3) {
            g_dirty[i] = true; // applied with the Apply button
        }
        // kind 2: nothing changes until a size is picked from the list
    }
    if (kind == 2) {
        // The list: e.g. "1920 x 1080  16:9  (scale 133%)", "3840 x 2160  16:9  native, heavy".
        auto label = [&](const core::display::CommonSize &c, char *out, size_t size) {
            const int pct = c.h > 0 ? static_cast<int>((static_cast<long long>(n.cy) * 100 + c.h / 2) / c.h) : 100;
            std::snprintf(out, size, "%d x %d  %s%s%s", c.w, c.h, c.aspect ? c.aspect : "",
                          c.native ? "  native" : "", c.heavy ? "  (heavy: slower in large cities)" : "");
            if (!c.native) {
                size_t len = std::strlen(out);
                std::snprintf(out + len, size - len, "  (scale %d%%)", pct);
            }
        };
        char current[96] = "";
        if (listed >= 0) {
            label(sizes[listed], current, sizeof(current));
        }
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##common", current)) {
            for (int k = 0; k < count; ++k) {
                char text[96];
                label(sizes[k], text, sizeof(text));
                if (ImGui::Selectable(text, k == listed)) {
                    std::snprintf(g_draft[i], cs::kValueSize, "%dx%d", sizes[k].w, sizes[k].h);
                    g_dirty[i] = true;
                    picked = 2;
                    Commit(i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Show all aspects", &g_allAspects);
    }
    if (kind == 3) {
        ImGui::SetNextItemWidth(width * 0.3f);
        bool changed = ImGui::InputInt("##w", &w, 0, 0);
        ImGui::SameLine();
        ImGui::TextUnformatted("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width * 0.3f);
        changed = ImGui::InputInt("##h", &h, 0, 0) || changed;
        if (changed) {
            w = w < 320 ? 320 : w > 16384 ? 16384 : w;
            h = h < 320 ? 320 : h > 16384 ? 16384 : h;
            std::snprintf(g_draft[i], cs::kValueSize, "%dx%d", w, h);
            g_dirty[i] = true;
        }
    }
}

// What the resolution and scale drafts give on this screen.
void RelayoutPreview(int resolution, int scale)
{
    if (resolution < 0 || scale < 0) {
        return;
    }
    SIZE n = core::display::NativeSize();
    int minW = 0, minH = 0;
    core::display::GameMinimum(&minW, &minH);
    const char *r = g_draft[resolution];
    core_resolution_kind kind = std::strcmp(r, "scaled") == 0 ? CORE_RESOLUTION_SCALED : CORE_RESOLUTION_FIXED;
    int w = std::atoi(r), h = 0;
    if (const char *x = std::strchr(r, 'x')) {
        h = std::atoi(x + 1);
    }
    int pct = std::strcmp(g_draft[scale], "auto") == 0 ? 0 : std::atoi(g_draft[scale]);
    core::display::LogicalSize l =
        core::display::ChooseLogicalSize(n.cx, n.cy, kind, w, h, pct, minW, minH);
    if (kind == CORE_RESOLUTION_SCALED) {
        ImGui::TextDisabled("Result: %d x %d (scale %d%% of %ld x %ld)%s", l.w, l.h, l.scale_percent, n.cx, n.cy,
                            l.limited ? ", limited to the minimum" : "");
    }
    else {
        ImGui::TextDisabled("Result: %d x %d on a %ld x %ld screen%s", l.w, l.h, n.cx, n.cy,
                            l.limited ? " (limited to the minimum)" : "");
    }
}

const char *SectionTitle(const char *section)
{
    if (_stricmp(section, "display") == 0) return "Display";
    if (_stricmp(section, "gameplay") == 0) return "Gameplay";
    if (_stricmp(section, "timing") == 0) return "Timing";
    if (_stricmp(section, "ui") == 0) return "Panel";
    if (_stricmp(section, "debug") == 0) return "Testing";
    return section;
}

// The display scale only applies to Resolution = Scaled; it is hidden otherwise (R2b.1, playtest 2026-10-24).
bool ScaleShown(int resolution)
{
    return resolution < 0 || std::strcmp(g_draft[resolution], "scaled") == 0;
}

// The result of the resolution and scale drafts, and Apply / Cancel for a custom size not applied yet: under the
// scale item, or under the resolution item when the scale is hidden. (The scale and the other choices apply at once
// -- the slider when it is let go -- so they need no buttons; R2b.1, playtest 2026-10-24.)
void RelayoutControls(int resolution, int displayScale)
{
    RelayoutPreview(resolution, displayScale);
    bool dirty = resolution >= 0 && g_dirty[resolution];
    if (dirty) {
        if (ImGui::Button("Apply")) {
            ApplyRelayout();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            if (resolution >= 0) g_dirty[resolution] = false;
            if (displayScale >= 0) g_dirty[displayScale] = false;
        }
    }
}

void DrawItem(int i, float scale, int resolution, int displayScale)
{
    const cs::Item &it = cs::Get(i);
    const float width = 300.0f * scale;
    ImGui::PushID(i);
    ImGui::TextUnformatted(it.label);
    Help(it);
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", EffectTag(it));
    if (!it.editable) {
        ImGui::BeginDisabled();
    }
    switch (it.type) {
    case Type::Bool: {
        bool on = std::strcmp(g_draft[i], "on") == 0;
        if (ImGui::Checkbox("On", &on)) {
            std::snprintf(g_draft[i], cs::kValueSize, "%s", on ? "on" : "off");
            g_dirty[i] = true;
            Commit(i);
        }
        break;
    }
    case Type::Choice: {
        int cur = 0;
        for (int k = 0; k < it.choiceCount; ++k) {
            if (std::strcmp(g_draft[i], it.choices[k]) == 0) cur = k;
        }
        ImGui::SetNextItemWidth(width);
        if (ImGui::Combo("##choice", &cur, it.choiceLabels ? it.choiceLabels : it.choices, it.choiceCount)) {
            std::snprintf(g_draft[i], cs::kValueSize, "%s", it.choices[cur]);
            g_dirty[i] = true;
            Commit(i);
        }
        break;
    }
    case Type::Int: {
        bool percent = _stricmp(it.key, "panel_scale") == 0 || _stricmp(it.key, "scale") == 0;
        IntWidget(i, it, it.key, width, percent ? "%d%%" : "%d");
        break;
    }
    case Type::Resolution:
        ResolutionWidget(i, width);
        break;
    case Type::Hotkey:
        ImGui::Text("%s", g_draft[i]);
        ImGui::SameLine();
        ImGui::TextDisabled("(set it in emperor_engine.ini; changing it here comes later)");
        break;
    }
    if (!it.editable) {
        ImGui::EndDisabled();
    }
    if (i == displayScale || (i == resolution && !ScaleShown(resolution))) {
        RelayoutControls(resolution, displayScale);
    }
    if (cs::RestartPending(i)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Takes effect after restarting the game.");
    }
    if (_stricmp(it.key, "present") == 0 && std::strcmp(g_draft[i], "original") == 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "With original this panel is not available (set present back in emperor_engine.ini).");
    }
    ImGui::PopID();
    ImGui::Spacing();
}

void RestoreDefaults()
{
    bool relayout = false;
    for (int i = 0; i < cs::Count(); ++i) {
        const cs::Item &it = cs::Get(i);
        if (!it.inPanel || !it.editable) {
            continue;
        }
        char now[cs::kValueSize];
        cs::Value(i, now, sizeof(now));
        if (std::strcmp(now, it.defaultValue) == 0) {
            continue;
        }
        std::snprintf(g_draft[i], cs::kValueSize, "%s", it.defaultValue);
        g_dirty[i] = true;
        if (it.effect == Effect::Relayout) {
            relayout = true;
        }
        else {
            Commit(i);
        }
    }
    if (relayout) {
        ApplyRelayout();
    }
    core_log("panel: restore defaults");
}

void ConfirmWindow(float scale)
{
    AcquireSRWLockShared(&g_lock);
    bool pending = g_confirm.pending;
    DWORD until = g_confirm.until;
    ReleaseSRWLockShared(&g_lock);
    if (!pending) {
        return;
    }
    LONG left = static_cast<LONG>(until - GetTickCount());
    if (left <= 0) {
        Revert("no answer within 15 s");
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.25f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowFocus();
    if (ImGui::Begin("Keep these display settings?", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Text("Reverting to the previous settings in %ld s.", (left + 999) / 1000);
        if (ImGui::Button("Keep", ImVec2(120 * scale, 0))) {
            Keep();
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert", ImVec2(120 * scale, 0))) {
            Revert("Revert");
        }
    }
    ImGui::End();
}

} // namespace

void DrawSettings(float scale)
{
    const int n = cs::Count() < kMax ? cs::Count() : kMax;
    int resolution = cs::Find("display", "resolution"), displayScale = cs::Find("display", "scale");
    for (int i = 0; i < n; ++i) {
        if (!g_dirty[i]) {
            cs::Value(i, g_draft[i], cs::kValueSize);
        }
    }
    AcquireSRWLockShared(&g_lock);
    bool pending = g_confirm.pending;
    ReleaseSRWLockShared(&g_lock);
    ImGui::BeginDisabled(pending);
    // Sections in the order of their first item.
    const char *done[16];
    int doneCount = 0;
    for (int i = 0; i < n; ++i) {
        const char *section = cs::Get(i).section;
        bool seen = false;
        for (int k = 0; k < doneCount; ++k) {
            seen = seen || _stricmp(done[k], section) == 0;
        }
        if (seen || doneCount >= 16) {
            continue;
        }
        done[doneCount++] = section;
        bool any = false;
        for (int j = i; j < n; ++j) {
            any = any || (cs::Get(j).inPanel && _stricmp(cs::Get(j).section, section) == 0);
        }
        if (!any || !ImGui::CollapsingHeader(SectionTitle(section), ImGuiTreeNodeFlags_DefaultOpen)) {
            continue;
        }
        for (int j = i; j < n; ++j) {
            if (cs::Get(j).inPanel && _stricmp(cs::Get(j).section, section) == 0) {
                if (j == displayScale && !ScaleShown(resolution)) {
                    continue;
                }
                DrawItem(j, scale, resolution, displayScale);
            }
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Restore defaults")) {
        g_askDefaults = true;
    }
    if (g_askDefaults) {
        ImGui::SameLine();
        ImGui::TextUnformatted("Restore every setting above?");
        ImGui::SameLine();
        if (ImGui::Button("Yes")) {
            g_askDefaults = false;
            RestoreDefaults();
        }
        ImGui::SameLine();
        if (ImGui::Button("No")) {
            g_askDefaults = false;
        }
    }
    char saved[160];
    cs::LastSaveResult(saved, sizeof(saved));
    if (saved[0]) {
        ImGui::TextDisabled("Settings file: %s", saved);
    }
    ImGui::EndDisabled();
    ConfirmWindow(scale);
}

bool WantsGameFrames()
{
    return g_framesPending.load() || static_cast<LONG>(g_framesUntil.load() - GetTickCount()) > 0;
}

void PanelClosed()
{
    Revert("panel closed without an answer");
}

} // namespace core::ui
