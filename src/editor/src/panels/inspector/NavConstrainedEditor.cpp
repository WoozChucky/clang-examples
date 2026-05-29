#include "NavConstrainedEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void NavConstrainedEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, NavConstrainedComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void NavConstrainedEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<NavConstrainedComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void NavConstrainedEditor::DrawEditor(const EditorContext&, EntityId) {
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Movement constrained to navmesh");
    ImGui::TextDisabled("Wall-slides along walkable edges; off-mesh recovers to nearest poly.");
}
