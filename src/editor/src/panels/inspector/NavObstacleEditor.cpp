#include "NavObstacleEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void NavObstacleEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, NavObstacleComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void NavObstacleEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<NavObstacleComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void NavObstacleEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    static const char* kShapeNames[] = { "Cylinder", "Box" };
    static const NavObstacleShape kShapes[] = {
        NavObstacleShape::Cylinder, NavObstacleShape::Box,
    };
    int idx = 0;
    for (int i = 0; i < 2; ++i) if (m_St.edit.Shape == kShapes[i]) idx = i;
    if (ImGui::Combo("Shape", &idx, kShapeNames, 2)) {
        m_St.edit.Shape = kShapes[idx];
        m_St.modified = true;
    }
    const char* sizeLabel = (m_St.edit.Shape == NavObstacleShape::Cylinder)
        ? "Size (X=radius, Y=height)"
        : "Size (half-extents)";
    if (ImGui::InputFloat3(sizeLabel, &m_St.edit.Size.x)) m_St.modified = true;
    if (ImGui::InputFloat3("Offset", &m_St.edit.Offset.x)) m_St.modified = true;
    ImGui::Spacing();
    m_St.Commit(ctx, e);
}
