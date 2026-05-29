#include "RenderStatsPanel.h"

#include <imgui.h>

#include "RenderStats.h" // resolved via the editor's Engine PUBLIC include dirs (same as StagingBufferPool.h in MemoryPanel)

bool DrawRenderStatsPanel(bool* open)
{
    if (open && !*open) return false;
    if (!ImGui::Begin("Render Stats", open)) { ImGui::End(); return false; }

    bool changed = false;
    changed |= ImGui::Checkbox("Frustum culling", &GetCullingSettings().Enabled);
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
    changed |= ImGui::Checkbox("Light gizmos",   &dd.ShowLightGizmos);
    changed |= ImGui::Checkbox("Camera frustum", &dd.ShowCameraFrustum);
    changed |= ImGui::Checkbox("Selected AABB",  &dd.ShowSelectedAABB);
    changed |= ImGui::Checkbox("Wireframe",      &dd.Wireframe);
    changed |= ImGui::Checkbox("Grid",           &dd.ShowGrid);
    changed |= ImGui::Checkbox("Colliders",      &dd.ShowColliders);
    changed |= ImGui::Checkbox("NavMesh",        &dd.ShowNavMesh);
    changed |= ImGui::Checkbox("Obstacles",      &dd.ShowObstacles);
    changed |= ImGui::Checkbox("Nav Paths",      &dd.ShowNavPaths);

    ImGui::Separator();
    ImGui::TextDisabled("Shadows");
    ShadowSettings& sh = GetShadowSettings();
    changed |= ImGui::Checkbox("Shadows", &sh.Enabled);
    // The slider mutates Bias live every drag frame; report the change only once the
    // drag ends (IsItemDeactivatedAfterEdit) so the caller doesn't rewrite every frame.
    ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::SliderFloat("Shadow coverage", &sh.ShadowCoverage, 5.0f, 200.0f, "%.0f");
    changed |= ImGui::SliderFloat("Shadow near-extend", &sh.NearExtend, 0.0f, 200.0f, "%.0f");

    ImGui::Separator();
    ImGui::TextDisabled("Anti-Aliasing");
    AntiAliasingSettings& aa = GetAntiAliasingSettings();
    {
        int mode = static_cast<int>(aa.Mode);
        const char* names[] = { "Off", "FXAA", "SMAA" };
        if (ImGui::Combo("Anti-aliasing", &mode, names, IM_ARRAYSIZE(names))) {
            aa.Mode = static_cast<AAMode>(mode);
            changed = true;
        }
    }

    ImGui::End();
    return changed;
}
