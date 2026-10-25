// The performance view (R2b.1-3): FPS, a graph of the main-loop pass time and where the time goes, from
// core/diag/perf. Drawn as our panel's "Performance" section and, with debug.perf_overlay = on, as a small overlay in
// the top left corner while playing (no input; core/ui/panel RenderOverlay). Output thread (ImGui).
#pragma once

namespace core::ui {

void PerfWidget(); // inside an ImGui window

} // namespace core::ui
