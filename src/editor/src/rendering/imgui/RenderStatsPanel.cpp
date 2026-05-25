#include "RenderStatsPanel.h"

#include <imgui.h>

#include "RenderStats.h" // resolved via the editor's Engine PUBLIC include dirs (same as StagingBufferPool.h in MemoryPanel)
#include "EditorPreferences.h" // persist the toggles across runs

void DrawRenderStatsPanel(bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Render Stats", open)) { ImGui::End(); return; }

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

    ImGui::Separator();
    ImGui::TextDisabled("Shadows");
    ShadowSettings& sh = GetShadowSettings();
    changed |= ImGui::Checkbox("Shadows", &sh.Enabled);
    // The slider mutates Bias live every drag frame, but only persist once the drag
    // ends (IsItemDeactivatedAfterEdit) to avoid rewriting the file every frame.
    ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();

    // Checkboxes report change on their click frame; the bias slider is persisted on
    // release (above). So editor_preferences.json is written only on completed edits,
    // not every frame.
    if (changed)
        EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH);

    ImGui::End();
}
