#include "NavMeshSourceEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void NavMeshSourceEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, NavMeshSourceComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void NavMeshSourceEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<NavMeshSourceComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void NavMeshSourceEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    // AreaId: 0-63, Recast convention (63 == RC_WALKABLE_AREA default).
    int areaId = static_cast<int>(m_St.edit.AreaId);
    if (ImGui::SliderInt("Area ID", &areaId, 0, 63)) {
        m_St.edit.AreaId = static_cast<uint8_t>(areaId);
        m_St.modified = true;
    }
    // Geometry: Unset sentinel forces explicit author choice. "-- choose --" entry
    // surfaces the unselected state loudly so authors don't ship Unset accidentally.
    static const char* kGeomNames[]   = { "-- choose --", "Collider", "Mesh" };
    static const NavMeshGeometrySource kGeoms[] = {
        NavMeshGeometrySource::Unset,
        NavMeshGeometrySource::Collider,
        NavMeshGeometrySource::Mesh,
    };
    int geomIdx = 0;
    for (int i = 0; i < 3; ++i) if (m_St.edit.Geometry == kGeoms[i]) geomIdx = i;
    if (ImGui::Combo("Geometry", &geomIdx, kGeomNames, 3)) {
        m_St.edit.Geometry = kGeoms[geomIdx];
        m_St.modified = true;
    }
    if (m_St.edit.Geometry == NavMeshGeometrySource::Unset) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "Unset: SM_WARN + skip at build. Pick a geometry source.");
    }
    ImGui::Spacing();
    m_St.Commit(ctx, e);
}
