#include "ColliderEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void ColliderEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, ColliderComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void ColliderEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<ColliderComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void ColliderEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    static const char* kShapeNames[] = { "Box", "Sphere", "Capsule" };
    static const ColliderShape kShapes[] = { ColliderShape::Box, ColliderShape::Sphere, ColliderShape::Capsule };
    int curIdx = 0;
    for (int i = 0; i < 3; ++i) if (m_St.edit.Shape == kShapes[i]) curIdx = i;
    if (ImGui::Combo("Shape", &curIdx, kShapeNames, 3)) {
        m_St.edit.Shape = kShapes[curIdx];
        m_St.modified = true;
    }
    const char* sizeLabel = "Size (half extents / radius-height)";
    if (m_St.edit.Shape == ColliderShape::Sphere) sizeLabel = "Size (radius in X)";
    else if (m_St.edit.Shape == ColliderShape::Capsule) sizeLabel = "Size (radius X, half-height Y)";
    if (ImGui::InputFloat3(sizeLabel, &m_St.edit.Size.x)) m_St.modified = true;
    if (ImGui::InputFloat3("Offset", &m_St.edit.Offset.x)) m_St.modified = true;
    if (ImGui::Checkbox("Trigger", &m_St.edit.IsTrigger)) m_St.modified = true;
    if (ImGui::Checkbox("Static", &m_St.edit.IsStatic)) m_St.modified = true;
    if (ImGui::InputScalar("Layer", ImGuiDataType_U32, &m_St.edit.Layer)) m_St.modified = true;
    if (ImGui::InputScalar("Mask", ImGuiDataType_U32, &m_St.edit.Mask)) m_St.modified = true;
    ImGui::Spacing();
    m_St.Commit(ctx, e);
}
