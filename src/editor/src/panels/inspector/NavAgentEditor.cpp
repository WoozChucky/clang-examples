#include "NavAgentEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void NavAgentEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, NavAgentComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void NavAgentEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<NavAgentComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void NavAgentEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    if (ImGui::DragFloat("Move Speed",      &m_St.edit.MoveSpeed,      0.05f, 0.0f, 50.0f, "%.2f m/s")) m_St.modified = true;
    if (ImGui::DragFloat("Radius",          &m_St.edit.Radius,         0.01f, 0.05f, 5.0f, "%.2f m"))   m_St.modified = true;
    if (ImGui::DragFloat("Reached Epsilon", &m_St.edit.ReachedEpsilon, 0.01f, 0.01f, 2.0f, "%.2f m"))   m_St.modified = true;
    ImGui::Spacing();
    m_St.Commit(ctx, e);
}
