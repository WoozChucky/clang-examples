#include "NavClassEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void NavClassEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, NavClassComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void NavClassEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<NavClassComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd))
        SM_WARN("ECS command queue full! Remove component command dropped.");
}
void NavClassEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;
    int classId = static_cast<int>(m_St.edit.ClassId);
    if (ImGui::DragInt("Class Id", &classId, 0.1f, 0, kMaxNavClasses - 1)) {
        m_St.edit.ClassId = static_cast<uint8_t>(classId);
        m_St.modified = true;
    }
    ImGui::TextDisabled("Index into NavMeshConfig classes; out-of-range falls back to 0.");
    m_St.Commit(ctx, e);
}
