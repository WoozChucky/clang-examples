#pragma once
#include <cstdint>
#include <imgui.h>

struct EditorContext;

// Draws the "Material Manager" window: material list, preview, and load-from-file.
class MaterialManagerPanel {
public:
    void Draw(const EditorContext& ctx);
private:
    // Persistent UI selection/status state (formerly function-local statics in
    // ImGuiRenderer::Render's Material Manager block).
    uint64_t selectedMaterialId = 0; // asset HANDLE (0 = default material); see hasSelection
    bool     hasSelection = false;
    char   statusMessage[512] = "";
    ImVec4 statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
};
