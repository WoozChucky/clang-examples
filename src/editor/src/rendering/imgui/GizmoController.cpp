#include "GizmoController.h"
#include "EditorContext.h"
#include <imgui.h>

void GizmoController::DrawControls()
{
    // Small inline gizmo controls specific to Transform component
    ImGui::Separator();
    ImGui::Text("Gizmo:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Translate##GZ", m_Operation == ImGuizmo::TRANSLATE)) m_Operation = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate##GZ", m_Operation == ImGuizmo::ROTATE)) m_Operation = ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale##GZ", m_Operation == ImGuizmo::SCALE)) m_Operation = ImGuizmo::SCALE;

    if (m_Operation != ImGuizmo::SCALE)
    {
        if (ImGui::RadioButton("Local##GZ", m_Mode == ImGuizmo::LOCAL)) m_Mode = ImGuizmo::LOCAL;
        ImGui::SameLine();
        if (ImGui::RadioButton("World##GZ", m_Mode == ImGuizmo::WORLD)) m_Mode = ImGuizmo::WORLD;
    }

    // Keyboard shortcuts for convenience
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) m_Operation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) m_Operation = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) m_Operation = ImGuizmo::SCALE;
    if (ImGui::IsKeyPressed(ImGuiKey_F4)) m_UseSnap = !m_UseSnap;

    ImGui::Checkbox("Snap##GZ", &m_UseSnap);
    ImGui::SameLine();
    if (m_Operation == ImGuizmo::TRANSLATE)
    {
        ImGui::InputFloat3("Step##GZ", &m_Snap[0]);
    }
    else if (m_Operation == ImGuizmo::ROTATE)
    {
        ImGui::InputFloat("Angle##GZ", &m_Snap[0]);
    }
    else // SCALE
    {
        ImGui::InputFloat("Scale##GZ", &m_Snap[0]);
    }
}

void GizmoController::EditTransform(float* cameraView, float* cameraProjection, float* matrix,
                                   const EditorContext& ctx)
{
    // The scene is rendered into the dockable "Viewport" panel, so the gizmo must map to that
    // panel's screen rect (captured during the Viewport draw earlier this frame) and draw on the
    // foreground draw list so it appears on top of the scene image — not the inspector window the
    // call originates from. Skip when the Viewport is collapsed/zero-sized.
    if (!ctx.ViewportDrawList || ctx.ViewportW < 1 || ctx.ViewportH < 1)
        return;
    ImGuizmo::SetDrawlist(ctx.ViewportDrawList);
    ImGuizmo::SetRect(ctx.ViewportMinX, ctx.ViewportMinY,
                      static_cast<float>(ctx.ViewportW), static_cast<float>(ctx.ViewportH));
    ImGuizmo::Manipulate(cameraView, cameraProjection, m_Operation, m_Mode, matrix, nullptr, m_UseSnap ? &m_Snap[0] : nullptr);
}
