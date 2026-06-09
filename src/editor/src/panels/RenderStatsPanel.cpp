#include "RenderStatsPanel.h"

#include <imgui.h>

#include "RenderStats.h" // resolved via the editor's Engine PUBLIC include dirs (same as StagingBufferPool.h in MemoryPanel)
#include "ECS.h"         // kMaxNavClasses (NavMesh debug class selector bound)

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
    if (dd.ShowNavMesh) {
        // Which class mesh to draw — the player's class may differ from class 0.
        if (ImGui::DragInt("NavMesh class", &dd.NavMeshClass, 0.1f, 0, kMaxNavClasses - 1)) {
            dd.NavMeshClass = dd.NavMeshClass < 0 ? 0 : (dd.NavMeshClass > kMaxNavClasses - 1 ? kMaxNavClasses - 1 : dd.NavMeshClass);
            changed = true;
        }
    }
    changed |= ImGui::Checkbox("Obstacles",      &dd.ShowObstacles);
    changed |= ImGui::Checkbox("Nav Paths",      &dd.ShowNavPaths);
    changed |= ImGui::Checkbox("Skeleton",       &dd.ShowSkeleton);

    ImGui::Separator();
    ImGui::TextDisabled("Shadows");
    ShadowSettings& sh = GetShadowSettings();
    changed |= ImGui::Checkbox("Shadows", &sh.Enabled);
    changed |= ImGui::SliderFloat("Shadow distance", &sh.ShadowDistance, 10.0f, 150.0f, "%.0f");
    changed |= ImGui::SliderFloat("Shadow near-extend", &sh.NearExtend, 0.0f, 200.0f, "%.0f");
    changed |= ImGui::SliderFloat("Shadow normal-offset", &sh.NormalOffset, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Shadow PCF radius", &sh.PcfRadius, 0.5f, 4.0f, "%.2f");

    ImGui::TextDisabled("SSAO");
    SsaoSettings& ao = GetSsaoSettings();
    changed |= ImGui::Checkbox("SSAO enabled", &ao.Enabled);
    changed |= ImGui::SliderFloat("SSAO radius",    &ao.Radius,    0.05f, 3.0f, "%.2f");
    changed |= ImGui::SliderFloat("SSAO intensity", &ao.Intensity, 0.0f,  3.0f, "%.2f");
    changed |= ImGui::SliderFloat("SSAO power",     &ao.Power,     0.5f,  6.0f, "%.2f");
    changed |= ImGui::SliderFloat("SSAO bias",      &ao.Bias,      0.0f,  0.2f, "%.3f");

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
