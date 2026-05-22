#include "StatsPanel.h"
#include "EditorContext.h"

#include <imgui.h>
#include "ApplicationContext.h"

void StatsPanel::Draw(const EditorContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();

    ImGui::Begin("Hello, world!");

    ImGui::Text("Renderer average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
    ImGui::Text("GPU %.3fms",ctx.GpuFrameTimeMs);
    ImGui::Text("Game TPS: %.2f/%.2f", ctx.Snapshot->ActualTPS, ctx.Snapshot->TargetTPS);

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Game Thread Settings");

    // Read current settings
    GameThreadSettings settings = ctx.App->GameThreadConfig.load();
    bool settingsChanged = false;

    // Target TPS slider with common presets
    ImGui::Text("Target TPS:");
    ImGui::SameLine();
    if (ImGui::Button("60")) { settings.TargetTPS = 60.0; settingsChanged = true; }
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

    // Spin threshold in microseconds
    int spinThresholdInt = static_cast<int>(settings.SpinThresholdMicros);
    if (ImGui::SliderInt("Spin Threshold (us)", &spinThresholdInt, 0, 2000, "%d us")) {
        settings.SpinThresholdMicros = static_cast<uint32_t>(spinThresholdInt);
        settingsChanged = true;
    }
    ImGui::TextWrapped("Lower = more accurate timing, higher CPU usage during spin");

    // Frame time tracking toggle
    if (ImGui::Checkbox("Enable Frame Time Tracking", &settings.EnableFrameTimeTracking)) {
        settingsChanged = true;
    }

    // Write settings back if changed
    if (settingsChanged) {
        ctx.App->GameThreadConfig.store(settings);
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Frame Time Statistics");

    // Use snapshot's frame stats (passed from RenderThread, which got it from GameThread)
    if (settings.EnableFrameTimeTracking && ctx.Snapshot->FrameStats.SampleCount > 0) {
        ImGui::Text("Min: %.3f ms", ctx.Snapshot->FrameStats.MinFrameTimeMs);
        ImGui::Text("Max: %.3f ms", ctx.Snapshot->FrameStats.MaxFrameTimeMs);
        ImGui::Text("Avg: %.3f ms", ctx.Snapshot->FrameStats.AvgFrameTimeMs);
        ImGui::Text("Samples: %llu", ctx.Snapshot->FrameStats.SampleCount);

        // Reset button
        if (ImGui::Button("Reset Stats")) {
            // This will be picked up by game thread on next frame
            // (stats reset happens in game thread when sample count wraps or manually)
        }
    } else {
        ImGui::TextDisabled("(Frame time tracking disabled)");
    }

    ImGui::Separator();
    ImGui::End();
}
