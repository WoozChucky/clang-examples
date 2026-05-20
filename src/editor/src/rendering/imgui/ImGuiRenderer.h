#pragma once

#include <memory>
#include <string>
#include <nvrhi/nvrhi.h>
#include <GLFW/glfw3.h>

#include "imgui_nvrhi.h"
#include "lib.h"

class MeshSystem;
class MaterialSystem;
class MeshPreviewRenderer;
class Renderer;
class RegisteredFont;
class ECS;
struct ApplicationContext;
struct SimulationSnapshot;

class ImGuiRenderer final {
public:
    ImGuiRenderer(); // Constructor defined in .cpp to allow unique_ptr with forward-declared type
    ~ImGuiRenderer(); // Destructor defined in .cpp to allow unique_ptr with forward-declared type

    bool Init(nvrhi::IDevice* device, ApplicationContext* appContext, MeshSystem* meshSystem, MaterialSystem* materialSystem, Renderer* renderer);
    void Render(nvrhi::IFramebuffer* framebuffer, double deltaTime, SimulationSnapshot& snapshot, const ECS* world, float gpuFrameTimeMs);
    void Shutdown();

    // Hot-swap: tear down only the device-bound NVRHI backend + preview
    // renderer; keep the ImGui context, dock layout, and loaded fonts.
    void ShutdownNvrhiOnly();
    // Hot-swap: recreate the NVRHI backend + preview renderer against a new
    // device, and re-upload the font atlas. Returns false on failure.
    bool InitNvrhiForDevice(nvrhi::IDevice* device);
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
    std::unique_ptr<MeshPreviewRenderer> m_MeshPreviewRenderer;
    ApplicationContext* m_AppContext = nullptr;
    MeshSystem* m_MeshSystem = nullptr;
    MaterialSystem* m_MaterialSystem = nullptr;
    Renderer* m_Renderer = nullptr;  // retained for hot-swap preview re-init

    // Mesh preview camera state
    struct MeshPreviewState {
        float cameraDistance = 3.0f;
        float cameraYaw = 0.0f;
        float cameraPitch = 0.3f;
        bool isDragging = false;
        float lastMouseX = 0.0f;
        float lastMouseY = 0.0f;
    };
    MeshPreviewState m_MeshPreviewState;

    // Settings menu state — pending backend selection until user clicks Apply.
    // Initialized lazily on first menu open from m_AppContext->Settings.Backend.
    RendererAPI m_PendingBackend = RendererAPI::Invalid;
    bool        m_PendingBackendInitialized = false;
    std::string m_SettingsSaveError;          // empty when no error
};
