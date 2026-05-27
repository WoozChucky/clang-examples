#include "NavigationPanel.h"
#include "EditorContext.h"

#include <imgui.h>

#include "ECS.h"
#include "ECSCommands.h"
#include "ApplicationContext.h" // ctx.App->ECSCommandRing
#include "lib.h"                // SM_WARN

#include "navigation/NavMeshSystem.h"
#include "navigation/NavMesh.h"

namespace {
    // Push a singleton-component edit through the command ring (RenderThread ->
    // GameThread). AddComponent upserts on the singleton entity. Matches the
    // DayNightPanel flow.
    template <typename T>
    void PushSingletonEdit(const EditorContext& ctx, const ECS* world, const T& value, const char* what)
    {
        ECSCommand cmd = ECSCommand::AddComponent(world->SingletonEntity(), value);
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("NavigationPanel: ECSCommandRing full, %s edit dropped", what);
        }
    }
}

void DrawNavigationPanel(const EditorContext& ctx, bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Navigation", open)) { ImGui::End(); return; }

    const ECS* world = ctx.World;
    if (!world) { ImGui::TextDisabled("No world"); ImGui::End(); return; }

    // --- Config ---
    NavMeshConfigComponent cfg{};
    if (const auto* cur = world->GetSingleton<NavMeshConfigComponent>()) cfg = *cur;
    NavMeshConfigComponent edited = cfg;

    ImGui::SeparatorText("Config");
    bool changed = false;
    changed |= ImGui::DragFloat("Cell Size",     &edited.CellSize,      0.01f, 0.05f, 2.0f,   "%.2f m");
    changed |= ImGui::DragFloat("Cell Height",   &edited.CellHeight,    0.01f, 0.05f, 2.0f,   "%.2f m");
    changed |= ImGui::DragFloat("Agent Radius",  &edited.AgentRadius,   0.05f, 0.05f, 5.0f,   "%.2f m");
    changed |= ImGui::DragFloat("Agent Height",  &edited.AgentHeight,   0.05f, 0.10f, 5.0f,   "%.2f m");
    changed |= ImGui::DragFloat("Max Climb",     &edited.AgentMaxClimb, 0.05f, 0.00f, 2.0f,   "%.2f m");
    changed |= ImGui::DragFloat("Max Slope",     &edited.AgentMaxSlope, 1.00f, 0.00f, 85.0f,  "%.0f deg");
    changed |= ImGui::DragFloat("Tile Size",     &edited.TileSize,      1.00f, 8.00f, 128.0f, "%.0f voxels");
    changed |= ImGui::DragInt  ("Max Obstacles", &edited.MaxObstacles,  1.0f,  0,     4096);

    if (changed) PushSingletonEdit(ctx, world, edited, "nav config");

    // --- Build trigger ---
    ImGui::Spacing();
    if (ImGui::Button("Rebuild NavMesh", ImVec2(-1, 0))) {
        ECSCommand cmd = ECSCommand::RebuildNavMesh();
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("NavigationPanel: ECSCommandRing full, RebuildNavMesh dropped");
        }
    }

    // --- Status ---
    ImGui::Spacing();
    ImGui::SeparatorText("Status");
    auto nm = NavMeshSystem::Instance().Current();
    if (!nm) {
        ImGui::TextUnformatted("(no navmesh built yet)");
    } else {
        const auto s = nm->GetStats();
        ImGui::Text("Tiles built: %d", s.TilesBuilt);
        ImGui::Text("Polys: %d",       s.PolyCount);
        ImGui::Text("Vert count: %d",  s.VertCount);
        ImGui::Text("Memory: %d KB",   s.MemoryKB);
    }

    ImGui::End();
}
