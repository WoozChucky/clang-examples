#pragma once

#include "MetricHistory.h"

struct EditorContext;

// Read-only live performance graphs: FPS, CPU frame time, GPU time, TPS. Samples one value per
// metric each Draw into a rolling MetricHistory and plots it with ImGui::PlotLines.
class PerformancePanel {
public:
    void Draw(const EditorContext& ctx);

private:
    static constexpr int kSamples = 240; // ~4 s at 60 fps
    MetricHistory<kSamples> m_Fps;
    MetricHistory<kSamples> m_CpuMs;
    MetricHistory<kSamples> m_GpuMs;
    MetricHistory<kSamples> m_Tps;
};
