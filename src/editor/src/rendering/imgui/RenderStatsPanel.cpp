#include "RenderStatsPanel.h"

#include <imgui.h>

#include "RenderStats.h" // resolved via the editor's Engine PUBLIC include dirs (same as StagingBufferPool.h in MemoryPanel)
#include "Fog.h" // resolved via the editor's Engine PUBLIC include dirs (same as RenderStats.h)
#include "Sky.h"

void DrawRenderStatsPanel(bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Render Stats", open)) { ImGui::End(); return; }

    ImGui::Checkbox("Frustum culling", &GetCullingSettings().Enabled);
    ImGui::Separator();

    const RenderStats& s = GetRenderStats();
    ImGui::Text("Mesh entities: %u", s.MeshEntitiesTotal);
    ImGui::Text("  drawn:  %u", s.MeshEntitiesDrawn);
    ImGui::Text("  culled: %u", s.MeshEntitiesCulled);
    ImGui::Text("Instances: %u", s.InstancesDrawn);
    ImGui::Text("Batches:   %u", s.BatchesDrawn);

    ImGui::Separator();
    ImGui::TextDisabled("Debug Draw");
    DebugDrawSettings& dd = GetDebugDrawSettings();
    ImGui::Checkbox("Light gizmos",   &dd.ShowLightGizmos);
    ImGui::Checkbox("Camera frustum", &dd.ShowCameraFrustum);
    ImGui::Checkbox("Selected AABB",  &dd.ShowSelectedAABB);
    ImGui::Checkbox("Wireframe",      &dd.Wireframe);

    ImGui::Separator();
    ImGui::TextDisabled("Shadows");
    ShadowSettings& sh = GetShadowSettings();
    ImGui::Checkbox("Shadows", &sh.Enabled);
    ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");

    ImGui::Separator();
    ImGui::TextDisabled("Fog");
    FogSettings& fog = GetFogSettings();
    ImGui::Checkbox("Fog enabled", &fog.Enabled);
    ImGui::SliderFloat("Day density",   &fog.DayDensity,   0.0f, 0.05f, "%.4f");
    ImGui::SliderFloat("Night density", &fog.NightDensity, 0.0f, 0.30f, "%.3f");
    ImGui::ColorEdit3("Day color",   &fog.DayColor.x);
    ImGui::ColorEdit3("Night color", &fog.NightColor.x);

    ImGui::Separator();
    ImGui::TextDisabled("Sky");
    SkySettings& sky = GetSkySettings();
    ImGui::Checkbox("Sky enabled", &sky.Enabled);
    ImGui::ColorEdit3("Day zenith",    &sky.DayZenith.x);
    ImGui::ColorEdit3("Day horizon",   &sky.DayHorizon.x);
    ImGui::ColorEdit3("Night zenith",  &sky.NightZenith.x);
    ImGui::ColorEdit3("Night horizon", &sky.NightHorizon.x);
    ImGui::ColorEdit3("Sun color",     &sky.SunColor.x);
    ImGui::SliderFloat("Sun radius (deg)",  &sky.SunRadiusDeg, 0.5f, 15.0f, "%.1f");
    ImGui::SliderFloat("Sun glow",          &sky.SunGlow, 1.0f, 512.0f, "%.0f");
    ImGui::ColorEdit3("Moon color",    &sky.MoonColor.x);
    ImGui::SliderFloat("Moon radius (deg)", &sky.MoonRadiusDeg, 0.5f, 15.0f, "%.1f");
    ImGui::SliderFloat("Moon glow",         &sky.MoonGlow, 1.0f, 512.0f, "%.0f");

    ImGui::End();
}
