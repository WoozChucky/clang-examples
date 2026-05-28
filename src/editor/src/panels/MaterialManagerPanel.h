#pragma once
#include <imgui.h>

struct EditorContext;

// Draws the "Material Manager" window: material list, preview, and load-from-file.
class MaterialManagerPanel {
public:
    void Draw(const EditorContext& ctx);
private:
    // Persistent UI selection/status state (formerly function-local statics in
    // ImGuiRenderer::Render's Material Manager block).
    int    selectedMaterialId = -1;
    char   statusMessage[512] = "";
    ImVec4 statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
};
