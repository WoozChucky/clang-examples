#pragma once
#include <imgui.h>
#include <ImGuizmo.h>
struct EditorContext;

// Owns the transform-gizmo settings and drives ImGuizmo for the selected entity, mapped to the
// scene Viewport panel (rect/drawlist come from EditorContext).
class GizmoController {
public:
    // The inline operation/mode/snap controls (radio buttons + snap inputs + F1-F4 shortcuts).
    void DrawControls();
    // Manipulate `matrix` (column-major float[16]) with the camera, targeting the Viewport panel.
    // No-op if the Viewport is hidden (ctx.ViewportDrawList == nullptr).
    void EditTransform(float* cameraView, float* cameraProjection, float* matrix,
                       const EditorContext& ctx);
private:
    ImGuizmo::OPERATION m_Operation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      m_Mode      = ImGuizmo::LOCAL;
    bool                m_UseSnap   = false;
    float               m_Snap[3]   = { 1.0f, 1.0f, 1.0f };
};
