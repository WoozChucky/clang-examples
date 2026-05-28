#include "UIRectEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void UIRectEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, UIRectComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void UIRectEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<UIRectComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void UIRectEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    if (ImGui::DragFloat2("Size (px)", &m_St.edit.Size.x, 1.0f, 1.0f, 4096.0f, "%.0f")) {
        m_St.modified = true;
    }
    if (ImGui::ColorEdit4("Color##UIRect", &m_St.edit.Color.x)) {
        m_St.modified = true;
    }
    ImGui::Spacing();
    m_St.Commit(ctx, e);
}
