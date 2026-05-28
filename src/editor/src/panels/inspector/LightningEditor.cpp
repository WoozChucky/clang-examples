#include "LightningEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <glm/glm.hpp>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void LightningEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    LightningComponent newLightning{};
    newLightning.Type = LightningType::Directional;
    newLightning.Direction = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
    newLightning.Color = glm::vec4(1.0f);
    newLightning.Intensity = 1.0f;
    ECSCommand addCmd = ECSCommand::AddComponent(e, newLightning);
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void LightningEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<LightningComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void LightningEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    if (!m_St.Begin(ctx, e)) return;

    // Type editor
    const char* types[] = { "Directional", "Point", "Spot" };
    int currentType = static_cast<int>(m_St.edit.Type);
    if (ImGui::Combo("Type", &currentType, types, IM_ARRAYSIZE(types))) {
        m_St.edit.Type = static_cast<LightningType>(currentType);
        m_St.modified = true;
    }
    // Direction editor
    ImGui::Text("Direction:");
    if (ImGui::DragFloat3("##Direction", &m_St.edit.Direction.x, 0.1f, -1.0f, 1.0f)) {
        m_St.modified = true;
    }
    // Color editor
    ImGui::Text("Color:");
    if (ImGui::ColorEdit4("##Color", &m_St.edit.Color.r)) {
        m_St.modified = true;
    }
    // Intensity editor
    if (ImGui::DragFloat("Intensity", &m_St.edit.Intensity, 0.1f, 0.0f, 100.0f)) {
        m_St.modified = true;
    }
    // Range editor (for Point and Spot lights)
    if (m_St.edit.Type != LightningType::Directional) {
        if (ImGui::DragFloat("Range", &m_St.edit.Range, 0.1f, 0.0f, 1000.0f)) {
            m_St.modified = true;
        }
    }
    ImGui::Spacing();

    m_St.Commit(ctx, e);
}
