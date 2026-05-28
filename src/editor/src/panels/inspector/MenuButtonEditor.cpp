#include "MenuButtonEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "Actions.h"
#include "ECSCommands.h"
#include "lib.h"

void MenuButtonEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, MenuButtonComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void MenuButtonEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<MenuButtonComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void MenuButtonEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    static const char* kActionNames[] = { "None", "Play", "Quit", "Back" };
    static const uint32_t kActionIds[] = { Actions::None, Actions::Play, Actions::Quit, Actions::Back };
    int curIdx = 0;
    for (int i = 0; i < 4; ++i) if (m_St.edit.ActionId == kActionIds[i]) curIdx = i;
    if (ImGui::Combo("Action", &curIdx, kActionNames, 4)) {
        m_St.edit.ActionId = kActionIds[curIdx];
        m_St.modified = true;
    }
    if (ImGui::ColorEdit4("Normal##MenuBtn", &m_St.edit.Normal.x)) m_St.modified = true;
    if (ImGui::ColorEdit4("Hover##MenuBtn",  &m_St.edit.Hover.x))  m_St.modified = true;
    if (ImGui::ColorEdit4("Press##MenuBtn",  &m_St.edit.Press.x))  m_St.modified = true;
    ImGui::Spacing();
    m_St.Commit(ctx, e);
}
