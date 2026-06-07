#include "MeshEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <string>
#include <vector>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "MeshSystem.h"
#include "lib.h"

void MeshEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    MeshComponent newMesh{};
    newMesh.MeshId = 0;
    newMesh.Visible = false;

    ECSCommand addCmd = ECSCommand::AddComponent(e, newMesh);
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void MeshEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<MeshComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void MeshEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    if (!m_St.Begin(ctx, e)) return;

    // Mesh picker keyed by logical asset path
    if (ctx.MeshSys) {
        const auto assets = ctx.MeshSys->GetAssetList(); // vector<pair<uint64_t,string>>
        std::string current = ctx.MeshSys->KeyForHandle(m_St.edit.MeshId);
        if (current.empty()) current = "(none)";
        if (ImGui::BeginCombo("Mesh", current.c_str())) {
            for (const auto& [handle, key] : assets) {
                const bool isSelected = (handle == m_St.edit.MeshId);
                const char* label = key.empty() ? "(default)" : key.c_str();
                if (ImGui::Selectable(label, isSelected)) { m_St.edit.MeshId = handle; m_St.modified = true; }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (assets.empty()) ImGui::TextDisabled("No meshes loaded");
    } else {
        if (ImGui::InputScalar("Mesh handle", ImGuiDataType_U64, &m_St.edit.MeshId)) m_St.modified = true;
    }

    // Visibility toggle
    if (ImGui::Checkbox("Visible", &m_St.edit.Visible)) {
        m_St.modified = true;
    }


    ImGui::Spacing();

    if (m_St.modified) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
    }

    if (ImGui::Button("Apply Changes##Mesh", ImVec2(150, 0))) {
        ECSCommand modifyCmd = ECSCommand::ModifyComponent(e, m_St.edit);
        if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
            SM_WARN("ECS command queue full! Modify command dropped.");
        }
        m_St.modified = false;
    }
}
