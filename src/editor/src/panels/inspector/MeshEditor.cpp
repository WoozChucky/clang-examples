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

    // Mesh ID editor with dropdown
    if (ctx.MeshSys) {
        const uint32_t meshCount = ctx.MeshSys->GetMeshCount();
        if (meshCount > 0) {
            // Build combo items
            std::vector<std::string> meshItems;
            meshItems.reserve(meshCount);
            for (uint32_t i = 0; i < meshCount; ++i) {
                meshItems.push_back("Mesh " + std::to_string(i));
            }

            // Current selection
            int currentMeshIdx = static_cast<int>(m_St.edit.MeshId);
            if (currentMeshIdx >= static_cast<int>(meshCount)) {
                currentMeshIdx = 0; // Default to first mesh if invalid
            }

            // Combo dropdown
            if (ImGui::BeginCombo("Mesh ID", meshItems[currentMeshIdx].c_str())) {
                for (uint32_t i = 0; i < meshCount; ++i) {
                    const bool isSelected = (currentMeshIdx == static_cast<int>(i));
                    if (ImGui::Selectable(meshItems[i].c_str(), isSelected)) {
                        m_St.edit.MeshId = i;
                        m_St.modified = true;
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextDisabled("No meshes loaded");
        }
    } else {
        // Fallback if MeshSystem is not available
        if (ImGui::InputScalar("Mesh ID", ImGuiDataType_U32, &m_St.edit.MeshId)) {
            m_St.modified = true;
        }
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
