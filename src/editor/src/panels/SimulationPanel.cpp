#include "SimulationPanel.h"
#include "EditorContext.h"

#include <imgui.h>
#include <cstdint>

#include "ApplicationContext.h"

void SimulationPanel::Draw(const EditorContext& ctx)
{
    ImGui::Begin("Simulation");

    GameThreadSettings settings = ctx.App->GameThreadConfig.load();
    bool settingsChanged = false;

    // Target TPS: presets + slider.
    ImGui::Text("Target TPS:");
    ImGui::SameLine();
    if (ImGui::Button("60"))  { settings.TargetTPS = 60.0;  settingsChanged = true; }
    ImGui::SameLine();
    if (ImGui::Button("120")) { settings.TargetTPS = 120.0; settingsChanged = true; }
    ImGui::SameLine();
    if (ImGui::Button("144")) { settings.TargetTPS = 144.0; settingsChanged = true; }
    ImGui::SameLine();
    if (ImGui::Button("165")) { settings.TargetTPS = 165.0; settingsChanged = true; }

    float targetTpsFloat = static_cast<float>(settings.TargetTPS);
    if (ImGui::SliderFloat("##TargetTPS", &targetTpsFloat, 60.0f, 240.0f, "%.1f Hz")) {
        settings.TargetTPS = static_cast<double>(targetTpsFloat);
        settingsChanged = true;
    }

    // Spin threshold (microseconds).
    int spinThresholdInt = static_cast<int>(settings.SpinThresholdMicros);
    if (ImGui::SliderInt("Spin Threshold (us)", &spinThresholdInt, 0, 2000, "%d us")) {
        settings.SpinThresholdMicros = static_cast<uint32_t>(spinThresholdInt);
        settingsChanged = true;
    }
    ImGui::TextWrapped("Lower = more accurate timing, higher CPU usage during spin");

    // Frame-time tracking toggle.
    if (ImGui::Checkbox("Enable Frame Time Tracking", &settings.EnableFrameTimeTracking)) {
        settingsChanged = true;
    }

    if (settingsChanged) {
        ctx.App->GameThreadConfig.store(settings);
    }

    ImGui::End();
}
