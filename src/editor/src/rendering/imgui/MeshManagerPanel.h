#pragma once
#include <imgui.h>

struct EditorContext;

// Draws the "Mesh Manager" window: mesh list, 3D preview (orbit camera), and load-from-file.
class MeshManagerPanel {
public:
    void Draw(const EditorContext& ctx);
private:
    // Mesh preview camera state (orbit yaw/pitch/distance + drag tracking).
    struct PreviewState {
        float cameraDistance = 3.0f;
        float cameraYaw = 0.0f;
        float cameraPitch = 0.3f;
        bool  isDragging = false;
        float lastMouseX = 0.0f;
        float lastMouseY = 0.0f;
    };
    PreviewState m_Preview;

    // Persistent UI selection/status state (formerly function-local statics in
    // ImGuiRenderer::Render's Mesh Manager block).
    int    selectedMeshId = -1;
    int    lastViewedMesh = -1;
    char   statusMessage[512] = "";
    ImVec4 statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
};
