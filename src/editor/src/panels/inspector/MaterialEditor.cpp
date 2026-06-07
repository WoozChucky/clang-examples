#include "MaterialEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <string>
#include <vector>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "MaterialSystem.h"
#include "lib.h"

void MaterialEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    MaterialComponent newMaterial{};
    newMaterial.MaterialId = 0;
    newMaterial.BaseColor = glm::vec4(1.0f);
    newMaterial.Flags = 0;

    ECSCommand addCmd = ECSCommand::AddComponent(e, newMaterial);
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void MaterialEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<MaterialComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void MaterialEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    if (!m_St.Begin(ctx, e)) return;

    // Material picker keyed by logical asset key.
    if (ctx.MatSys) {
        const auto assets = ctx.MatSys->GetAssetList();
        std::string current = ctx.MatSys->KeyForHandle(m_St.edit.MaterialId);
        if (current.empty()) current = "(default)";
        if (ImGui::BeginCombo("Material", current.c_str())) {
            for (const auto& [handle, key] : assets) {
                const bool isSelected = (handle == m_St.edit.MaterialId);
                const char* label = key.empty() ? "(default)" : key.c_str();
                if (ImGui::Selectable(label, isSelected)) { m_St.edit.MaterialId = handle; m_St.modified = true; }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (assets.empty()) ImGui::TextDisabled("No materials loaded");
    } else {
        if (ImGui::InputScalar("Material handle", ImGuiDataType_U64, &m_St.edit.MaterialId)) m_St.modified = true;
    }
    // Base color editor
    ImGui::Text("Base Color:");
    if (ImGui::ColorEdit4("##BaseColor", &m_St.edit.BaseColor.r)) {
        m_St.modified = true;
    }
    ImGui::Spacing();

    // Flags editor - Use Texture checkbox (bit 0)
    bool useTexture = (m_St.edit.Flags & 1u) != 0;
    if (ImGui::Checkbox("Use Texture", &useTexture)) {
        if (useTexture) {
            m_St.edit.Flags |= 1u;  // Set bit 0
        } else {
            m_St.edit.Flags &= ~1u; // Clear bit 0
        }
        m_St.modified = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable texture sampling in shader (requires valid Material ID with texture)");
    }
    ImGui::Spacing();

    m_St.Commit(ctx, e);
}
