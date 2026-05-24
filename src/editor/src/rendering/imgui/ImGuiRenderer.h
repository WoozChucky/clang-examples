#pragma once

#include <memory>
#include <nvrhi/nvrhi.h>
#include <GLFW/glfw3.h>

#include "imgui_nvrhi.h"
#include "SceneViewport.h"
#include "StatsPanel.h"
#include "MaterialManagerPanel.h"
#include "MeshManagerPanel.h"
#include "EcsInspectorPanel.h"
#include "MainMenuBar.h"
#include "EditorCamera.h"
#include "lib.h"

class MeshSystem;
class MaterialSystem;
class MeshPreviewRenderer;
class Renderer;
class RegisteredFont;
class ECS;
struct ImDrawList;
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

    // Editor scene target: the gameplay passes render into this offscreen FB (sized to the
    // Viewport panel); the panel samples its color texture. Returns the FB to render into.
    nvrhi::IFramebuffer* GetSceneFramebuffer(nvrhi::IFramebuffer* swapChainFb);
private:
    std::shared_ptr<RegisteredFont> CreateFontFromFile(const char* fontFile, float fontSize);
    void ProcessInputEvents();

private:
    std::vector<std::shared_ptr<RegisteredFont>> m_fonts;
    std::unique_ptr<ImGui_NVRHI> m_ImGuiNvrhi;
    std::unique_ptr<MeshPreviewRenderer> m_MeshPreviewRenderer;
    ApplicationContext* m_AppContext = nullptr;
    MeshSystem* m_MeshSystem = nullptr;
    MaterialSystem* m_MaterialSystem = nullptr;
    Renderer* m_Renderer = nullptr;  // retained for hot-swap preview re-init

    SceneViewport m_SceneViewport;          // offscreen scene render target
    uint32_t m_LastViewportW = 0;           // last Viewport panel content size (pixels)
    uint32_t m_LastViewportH = 0;
    float    m_ViewportImageMinX = 0.0f;    // Viewport panel image screen top-left (ImGuizmo rect)
    float    m_ViewportImageMinY = 0.0f;
    ImDrawList* m_ViewportDrawList = nullptr; // Viewport window draw list (ImGuizmo target; null when hidden)
    bool m_ViewportHovered = false;  // Viewport panel hovered this frame (mouse routing)
    bool m_ViewportFocused = false;  // Viewport panel focused this frame (keyboard routing)

    MainMenuBar m_MenuBar;
    StatsPanel m_StatsPanel;
    MaterialManagerPanel m_MaterialManager;
    MeshManagerPanel m_MeshManager;
    EcsInspectorPanel m_EcsInspector;

    EditorCamera m_EditorCamera;
    bool         m_EditMode = true; // default to Edit (free camera) on launch
};
