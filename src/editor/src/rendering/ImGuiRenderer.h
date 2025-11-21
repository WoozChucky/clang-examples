#pragma once

#include <memory>
#include <nvrhi/nvrhi.h>
#include <GLFW/glfw3.h>

#include "imgui_nvrhi.h"

class MeshSystem;
class MaterialSystem;
class RegisteredFont;
struct ApplicationContext;
struct SimulationSnapshot;

class ImGuiRenderer final {
public:
    ImGuiRenderer() = default;
    ~ImGuiRenderer() {
        Shutdown();
    }

    bool Init(nvrhi::IDevice* device, ApplicationContext* appContext, MeshSystem* meshSystem, MaterialSystem* materialSystem);
    void Render(nvrhi::IFramebuffer* framebuffer, double deltaTime, SimulationSnapshot& snapshot);
    void Shutdown();
private:
    std::shared_ptr<RegisteredFont> CreateFontFromFile(const char* fontFile, float fontSize);
    void ProcessInputEvents();

    // ImGuizmo helpers as member functions (no external self parameter required)
    void TransformStart(float* cameraView, float* cameraProjection, float* matrix);
    void TransformEnd();
    void EditTransform(float* cameraView, float* cameraProjection, float* matrix);

    // File dialog helper (Windows native)
    // Returns true if file was selected, false if cancelled
    bool OpenFileDialog(char* outPath, size_t outPathSize, const char* filter);

private:
    std::vector<std::shared_ptr<RegisteredFont>> m_fonts;
    std::unique_ptr<ImGui_NVRHI> m_ImGuiNvrhi;
    ApplicationContext* m_AppContext = nullptr;
    MeshSystem* m_MeshSystem = nullptr;
    MaterialSystem* m_MaterialSystem = nullptr;
};
