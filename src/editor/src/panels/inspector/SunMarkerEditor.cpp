#include "SunMarkerEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void SunMarkerEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, SunMarker{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void SunMarkerEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<SunMarker>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void SunMarkerEditor::DrawEditor(const EditorContext&, EntityId) {
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Tagged as Sun");
    ImGui::TextDisabled("Day/night cycle drives this entity.");
}
