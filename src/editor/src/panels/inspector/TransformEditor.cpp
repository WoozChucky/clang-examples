#include "TransformEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/glm.hpp>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "TransformMath.h"   // ModelMatrix
#include "lib.h"

void TransformEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    TransformComponent newTransform{};
    newTransform.Position = glm::vec3(0.0f, 0.0f, 0.0f);
    newTransform.Rotation = glm::vec3(0.0f, 0.0f, 0.0f);
    newTransform.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

    ECSCommand addCmd = ECSCommand::AddComponent(e, newTransform);
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void TransformEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<TransformComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void TransformEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    if (!m_St.Begin(ctx, e)) return;

    // Position editor
    ImGui::Text("Position:");
    if (ImGui::InputFloat3("##Position", &m_St.edit.Position.x)) {
        m_St.modified = true;
    }

    // Rotation editor (in degrees for user-friendliness)
    ImGui::Text("Rotation:");
    glm::vec3 rotationDegrees = glm::degrees(m_St.edit.Rotation);
    if (ImGui::InputFloat3("##Rotation", &rotationDegrees.x)) {
        m_St.edit.Rotation = glm::radians(rotationDegrees);
        m_St.modified = true;
    }

    // Scale editor
    ImGui::Text("Scale:");
    if (ImGui::InputFloat3("##Scale", &m_St.edit.Scale.x)) {
        m_St.modified = true;
    }

    const auto hasTextTransform = ctx.WorldSnapshot->HasComponent<TextComponent>(e);
    // ImGuizmo integration: manipulate this entity's transform using camera
    if (!hasTextTransform) {
        glm::mat4 cameraView(1.0f), cameraProjection(1.0f);
        if (ctx.EditorCameraActive) {
            cameraView = ctx.EditorCamView;
            cameraProjection = ctx.EditorCamProj;
        } else if (const auto* cam = ctx.World ? ctx.World->GetSingleton<WorldCameraComponent>() : nullptr) {
            cameraView = cam->View;
            cameraProjection = cam->Projection;
        }

        m_Gizmo.DrawControls();

        // Build model matrix from current editable transform
        glm::mat4 M = ModelMatrix(m_St.edit);

        // Manipulate matrix within this inspector window
        m_Gizmo.EditTransform(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection), glm::value_ptr(M), ctx);

        // If user manipulated the gizmo, decompose back into component fields
        if (ImGuizmo::IsUsing())
        {
            float tr[3], rtDeg[3], sc[3];
            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(M), tr, rtDeg, sc);
            m_St.edit.Position = glm::vec3(tr[0], tr[1], tr[2]);
            m_St.edit.Rotation = glm::radians(glm::vec3(rtDeg[0], rtDeg[1], rtDeg[2]));
            m_St.edit.Scale    = glm::vec3(sc[0], sc[1], sc[2]);
            m_St.modified = true;
        }
    }

    // Buttons to apply or revert changes
    ImGui::Spacing();

    if (m_St.modified) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
    }

    m_St.Commit(ctx, e);
}
