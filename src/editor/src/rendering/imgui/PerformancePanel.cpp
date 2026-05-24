#include "PerformancePanel.h"
#include "EditorContext.h"

#include <imgui.h>
#include <cstdio>
#include <cfloat>

#include "ApplicationContext.h"

namespace {
// Push the latest sample, then draw "<header>" + a scrolling sparkline with a cur/min/max/avg
// overlay. `id` is a hidden ImGui id (e.g. "##fps") so the four plots don't collide.
template <int N>
void DrawMetric(const char* header, const char* id, MetricHistory<N>& h, float current)
{
    h.Push(current);
    char overlay[96];
    std::snprintf(overlay, sizeof(overlay), "cur %.2f   min %.2f   max %.2f   avg %.2f",
                  h.Last(), h.Min(), h.Max(), h.Avg());
    ImGui::TextUnformatted(header);
    ImGui::PlotLines(id, h.Data(), h.Count(), h.Offset(), overlay,
                     FLT_MAX, FLT_MAX, ImVec2(-1.0f, 40.0f)); // auto-scale, full width, 40px tall
}
} // namespace

void PerformancePanel::Draw(const EditorContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Begin("Performance");

    const float fps   = io.Framerate;
    const float cpuMs = io.DeltaTime * 1000.0f;
    const float gpuMs = ctx.GpuFrameTimeMs;
    const float tps   = ctx.Snapshot ? static_cast<float>(ctx.Snapshot->ActualTPS) : 0.0f;

    DrawMetric("FPS",    "##fps", m_Fps,   fps);
    ImGui::Separator();
    DrawMetric("CPU ms", "##cpu", m_CpuMs, cpuMs);
    ImGui::Separator();
    DrawMetric("GPU ms", "##gpu", m_GpuMs, gpuMs);
    ImGui::Separator();

    char tpsHeader[48];
    std::snprintf(tpsHeader, sizeof(tpsHeader), "TPS (target %.0f)",
                  ctx.Snapshot ? ctx.Snapshot->TargetTPS : 0.0);
    DrawMetric(tpsHeader, "##tps", m_Tps, tps);

    ImGui::End();
}
