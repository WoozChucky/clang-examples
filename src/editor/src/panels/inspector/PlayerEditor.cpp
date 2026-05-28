#include "PlayerEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void PlayerEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, PlayerComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void PlayerEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<PlayerComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void PlayerEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    if (ImGui::DragFloat("Move speed", &m_St.edit.MoveSpeed, 0.1f, 0.5f, 50.0f, "%.1f")) {
        m_St.modified = true;
    }
    ImGui::Spacing();
    m_St.Commit(ctx, e);
}
