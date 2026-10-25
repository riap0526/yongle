#include "core/ui/perf_view.h"

#include "core/diag/perf.h"

#include <imgui.h>

#include <cstdio>

namespace core::ui {

namespace perf = core::diag::perf;

void PerfWidget()
{
    perf::View v;
    perf::Read(&v);
    ImGui::Text("FPS %.0f   game passes %.0f/s", v.outputPerSecond, v.gamePerSecond);
    // The main-loop pass time (the game thread's total), last 240 passes; 0..33 ms (30 FPS at the top).
    int total = -1;
    for (int i = 0; i < v.count; ++i) {
        if (v.phase[i].total && v.phase[i].thread == perf::Thread::Game) {
            total = i;
        }
    }
    if (total >= 0) {
        float h[perf::kHistory];
        const int n = perf::History(total, h, perf::kHistory);
        char label[48];
        std::snprintf(label, sizeof(label), "pass %.1f ms", v.phase[total].avgMs);
        ImGui::PlotLines("##pass", h, n, 0, label, 0.0f, 33.3f, ImVec2(ImGui::GetFontSize() * 22, ImGui::GetFontSize() * 4));
    }
    if (ImGui::BeginTable("##phases", 3, ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char *name, float ms, float pct, bool showPct) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::Text("%6.2f ms", ms);
            ImGui::TableNextColumn();
            if (showPct) {
                ImGui::Text("%5.1f%%", pct);
            }
        };
        for (int t = 0; t < 2; ++t) {
            const perf::Thread thread = t == 0 ? perf::Thread::Game : perf::Thread::Output;
            float totalMs = 0;
            for (int i = 0; i < v.count; ++i) {
                const perf::PhaseView &p = v.phase[i];
                if (p.thread == thread && p.total) {
                    totalMs = p.avgMs;
                    row(t == 0 ? "game pass (total)" : "output frame (total)", p.avgMs, 0, false);
                }
            }
            for (int i = 0; i < v.count; ++i) {
                const perf::PhaseView &p = v.phase[i];
                if (p.thread == thread && !p.total) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "  %s", p.name);
                    row(name, p.avgMs, p.percent, totalMs > 0);
                }
            }
            if (t == 0 && totalMs > 0) {
                row("  other (game)", v.gameOtherMs, 100.0f * v.gameOtherMs / totalMs, true);
            }
        }
        ImGui::EndTable();
    }
}

} // namespace core::ui
