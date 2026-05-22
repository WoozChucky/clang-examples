#include "ImGuiRenderer.h"

#include <fstream>
#include <cstdio>
#include <vector>
#include <string>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>

#include "MeshSystem.h"
#include "MaterialSystem.h"
#include "MeshLoader.h"
#include "MaterialLoader.h"
#include "MeshPreviewRenderer.h"
#include "MemoryPanel.h"
#include "RenderStatsPanel.h"
#include "EditorContext.h"
#include "EditorFileDialog.h"
#include "StatsPanel.h"
#include "MaterialManagerPanel.h"

#include "ApplicationContext.h"
#include "registered_font.h"
#include "WorldManager.h"
#include "SettingsManager.h"
#include "tracy/Tracy.hpp"


// Helper function to convert GLFW keys to ImGuiKey
static ImGuiKey GlfwKeyToImGuiKey(int key) {
    switch (key) {
        case GLFW_KEY_TAB: return ImGuiKey_Tab;
        case GLFW_KEY_LEFT: return ImGuiKey_LeftArrow;
        case GLFW_KEY_RIGHT: return ImGuiKey_RightArrow;
        case GLFW_KEY_UP: return ImGuiKey_UpArrow;
        case GLFW_KEY_DOWN: return ImGuiKey_DownArrow;
        case GLFW_KEY_PAGE_UP: return ImGuiKey_PageUp;
        case GLFW_KEY_PAGE_DOWN: return ImGuiKey_PageDown;
        case GLFW_KEY_HOME: return ImGuiKey_Home;
        case GLFW_KEY_END: return ImGuiKey_End;
        case GLFW_KEY_INSERT: return ImGuiKey_Insert;
        case GLFW_KEY_DELETE: return ImGuiKey_Delete;
        case GLFW_KEY_BACKSPACE: return ImGuiKey_Backspace;
        case GLFW_KEY_SPACE: return ImGuiKey_Space;
        case GLFW_KEY_ENTER: return ImGuiKey_Enter;
        case GLFW_KEY_ESCAPE: return ImGuiKey_Escape;
        case GLFW_KEY_APOSTROPHE: return ImGuiKey_Apostrophe;
        case GLFW_KEY_COMMA: return ImGuiKey_Comma;
        case GLFW_KEY_MINUS: return ImGuiKey_Minus;
        case GLFW_KEY_PERIOD: return ImGuiKey_Period;
        case GLFW_KEY_SLASH: return ImGuiKey_Slash;
        case GLFW_KEY_SEMICOLON: return ImGuiKey_Semicolon;
        case GLFW_KEY_EQUAL: return ImGuiKey_Equal;
        case GLFW_KEY_LEFT_BRACKET: return ImGuiKey_LeftBracket;
        case GLFW_KEY_BACKSLASH: return ImGuiKey_Backslash;
        case GLFW_KEY_RIGHT_BRACKET: return ImGuiKey_RightBracket;
        case GLFW_KEY_GRAVE_ACCENT: return ImGuiKey_GraveAccent;
        case GLFW_KEY_CAPS_LOCK: return ImGuiKey_CapsLock;
        case GLFW_KEY_SCROLL_LOCK: return ImGuiKey_ScrollLock;
        case GLFW_KEY_NUM_LOCK: return ImGuiKey_NumLock;
        case GLFW_KEY_PRINT_SCREEN: return ImGuiKey_PrintScreen;
        case GLFW_KEY_PAUSE: return ImGuiKey_Pause;
        case GLFW_KEY_KP_0: return ImGuiKey_Keypad0;
        case GLFW_KEY_KP_1: return ImGuiKey_Keypad1;
        case GLFW_KEY_KP_2: return ImGuiKey_Keypad2;
        case GLFW_KEY_KP_3: return ImGuiKey_Keypad3;
        case GLFW_KEY_KP_4: return ImGuiKey_Keypad4;
        case GLFW_KEY_KP_5: return ImGuiKey_Keypad5;
        case GLFW_KEY_KP_6: return ImGuiKey_Keypad6;
        case GLFW_KEY_KP_7: return ImGuiKey_Keypad7;
        case GLFW_KEY_KP_8: return ImGuiKey_Keypad8;
        case GLFW_KEY_KP_9: return ImGuiKey_Keypad9;
        case GLFW_KEY_KP_DECIMAL: return ImGuiKey_KeypadDecimal;
        case GLFW_KEY_KP_DIVIDE: return ImGuiKey_KeypadDivide;
        case GLFW_KEY_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
        case GLFW_KEY_KP_SUBTRACT: return ImGuiKey_KeypadSubtract;
        case GLFW_KEY_KP_ADD: return ImGuiKey_KeypadAdd;
        case GLFW_KEY_KP_ENTER: return ImGuiKey_KeypadEnter;
        case GLFW_KEY_KP_EQUAL: return ImGuiKey_KeypadEqual;
        case GLFW_KEY_LEFT_SHIFT: return ImGuiKey_LeftShift;
        case GLFW_KEY_LEFT_CONTROL: return ImGuiKey_LeftCtrl;
        case GLFW_KEY_LEFT_ALT: return ImGuiKey_LeftAlt;
        case GLFW_KEY_LEFT_SUPER: return ImGuiKey_LeftSuper;
        case GLFW_KEY_RIGHT_SHIFT: return ImGuiKey_RightShift;
        case GLFW_KEY_RIGHT_CONTROL: return ImGuiKey_RightCtrl;
        case GLFW_KEY_RIGHT_ALT: return ImGuiKey_RightAlt;
        case GLFW_KEY_RIGHT_SUPER: return ImGuiKey_RightSuper;
        case GLFW_KEY_MENU: return ImGuiKey_Menu;
        case GLFW_KEY_0: return ImGuiKey_0;
        case GLFW_KEY_1: return ImGuiKey_1;
        case GLFW_KEY_2: return ImGuiKey_2;
        case GLFW_KEY_3: return ImGuiKey_3;
        case GLFW_KEY_4: return ImGuiKey_4;
        case GLFW_KEY_5: return ImGuiKey_5;
        case GLFW_KEY_6: return ImGuiKey_6;
        case GLFW_KEY_7: return ImGuiKey_7;
        case GLFW_KEY_8: return ImGuiKey_8;
        case GLFW_KEY_9: return ImGuiKey_9;
        case GLFW_KEY_A: return ImGuiKey_A;
        case GLFW_KEY_B: return ImGuiKey_B;
        case GLFW_KEY_C: return ImGuiKey_C;
        case GLFW_KEY_D: return ImGuiKey_D;
        case GLFW_KEY_E: return ImGuiKey_E;
        case GLFW_KEY_F: return ImGuiKey_F;
        case GLFW_KEY_G: return ImGuiKey_G;
        case GLFW_KEY_H: return ImGuiKey_H;
        case GLFW_KEY_I: return ImGuiKey_I;
        case GLFW_KEY_J: return ImGuiKey_J;
        case GLFW_KEY_K: return ImGuiKey_K;
        case GLFW_KEY_L: return ImGuiKey_L;
        case GLFW_KEY_M: return ImGuiKey_M;
        case GLFW_KEY_N: return ImGuiKey_N;
        case GLFW_KEY_O: return ImGuiKey_O;
        case GLFW_KEY_P: return ImGuiKey_P;
        case GLFW_KEY_Q: return ImGuiKey_Q;
        case GLFW_KEY_R: return ImGuiKey_R;
        case GLFW_KEY_S: return ImGuiKey_S;
        case GLFW_KEY_T: return ImGuiKey_T;
        case GLFW_KEY_U: return ImGuiKey_U;
        case GLFW_KEY_V: return ImGuiKey_V;
        case GLFW_KEY_W: return ImGuiKey_W;
        case GLFW_KEY_X: return ImGuiKey_X;
        case GLFW_KEY_Y: return ImGuiKey_Y;
        case GLFW_KEY_Z: return ImGuiKey_Z;
        case GLFW_KEY_F1: return ImGuiKey_F1;
        case GLFW_KEY_F2: return ImGuiKey_F2;
        case GLFW_KEY_F3: return ImGuiKey_F3;
        case GLFW_KEY_F4: return ImGuiKey_F4;
        case GLFW_KEY_F5: return ImGuiKey_F5;
        case GLFW_KEY_F6: return ImGuiKey_F6;
        case GLFW_KEY_F7: return ImGuiKey_F7;
        case GLFW_KEY_F8: return ImGuiKey_F8;
        case GLFW_KEY_F9: return ImGuiKey_F9;
        case GLFW_KEY_F10: return ImGuiKey_F10;
        case GLFW_KEY_F11: return ImGuiKey_F11;
        case GLFW_KEY_F12: return ImGuiKey_F12;
        default: return ImGuiKey_None;
    }
}

namespace {
void BuildDefaultDockLayout(ImGuiID dockId)
{
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockId;
    ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.20f, nullptr, &center);
    ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    ImGuiID leftBottom  = ImGui::DockBuilderSplitNode(left,  ImGuiDir_Down, 0.5f, nullptr, &left);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.5f, nullptr, &right);

    ImGui::DockBuilderDockWindow("Mesh Manager",           left);
    ImGui::DockBuilderDockWindow("Material Manager",       left);
    ImGui::DockBuilderDockWindow("Hello, world!",          leftBottom);
    ImGui::DockBuilderDockWindow("ECS Inspector & Editor", right);
    ImGui::DockBuilderDockWindow("Render Stats",           rightBottom);
    ImGui::DockBuilderDockWindow("Memory",                 rightBottom);
    ImGui::DockBuilderDockWindow("Viewport",               center);

    ImGui::DockBuilderFinish(dockId);
}
} // namespace

bool ImGuiRenderer::Init(nvrhi::IDevice* device, ApplicationContext* appContext, MeshSystem* meshSystem, MaterialSystem* materialSystem, Renderer* renderer) {
    m_AppContext = appContext;
    m_MeshSystem = meshSystem;
    m_MaterialSystem = materialSystem;
    m_Renderer = renderer;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable GamePad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows
    io.ConfigViewportsNoAutoMerge = true;
    //io.ConfigViewportsNoTaskBarIcon = true;
    io.ConfigDockingAlwaysTabBar = true;
    io.ConfigDockingTransparentPayload = true;

    ImGui::StyleColorsDark();

    // Get monitor scale
    float xscale = 1.0f, yscale = 1.0f;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor) {
        glfwGetMonitorContentScale(monitor, &xscale, &yscale);
    }
    const float mainScale = xscale;

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(mainScale);
    style.FontScaleDpi = mainScale;
    io.ConfigDpiScaleFonts = true;          // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
    io.ConfigDpiScaleViewports = true;      // [Experimental] Scale Dear ImGui and Platform Windows when Monitor DPI changes.

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        style.Colors[ImGuiCol_DockingEmptyBg].w = 0.0f;
    }

    // Setup backend flags for ImGui
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendPlatformName = "imgui_impl_custom";

    m_ImGuiNvrhi = std::make_unique<ImGui_NVRHI>();
    m_ImGuiNvrhi->init(device);

    // Initialize mesh preview renderer
    m_MeshPreviewRenderer = std::make_unique<MeshPreviewRenderer>();
    if (!m_MeshPreviewRenderer->Initialize(device, renderer, 256, 256))
    {
        SM_ERROR("ImGuiRenderer::Init: Failed to initialize MeshPreviewRenderer");
        m_MeshPreviewRenderer.reset();
    }

    m_SceneViewport.Init(device);

    CreateFontFromFile("assets/LiberationSans-Regular.ttf", 14.f);

    return true;
}

// ImGuizmo persistent state (made part of the renderer state instead of local statics)
static ImGuizmo::OPERATION m_GizmoOperation = ImGuizmo::TRANSLATE;
static ImGuizmo::MODE      m_GizmoMode      = ImGuizmo::LOCAL;
static bool                m_GizmoUseSnap   = false;
static float               m_GizmoSnap[3]   = { 1.0f, 1.0f, 1.0f };
static bool                m_GizmoUseWindow = false; // draw gizmo in its own window vs full-screen overlay
static float               m_GizmoCamDistance = 8.0f; // for ViewManipulate widget

// --- ImGuizmo integration helpers ---
// Implemented as ImGuiRenderer member functions (no external self argument)
void ImGuiRenderer::TransformStart(float* cameraView, float* cameraProjection, float* matrix)
{
    static float bounds[] = { -0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f };
    static float boundsSnap[] = { 0.1f, 0.1f, 0.1f };
    static bool boundSizing = false;
    static bool boundSizingSnap = false;

    if (ImGui::IsKeyPressed(ImGuiKey_T))
        m_GizmoOperation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_E))
        m_GizmoOperation = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) // r Key
        m_GizmoOperation = ImGuizmo::SCALE;
    if (ImGui::RadioButton("Translate", m_GizmoOperation == ImGuizmo::TRANSLATE))
        m_GizmoOperation = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate", m_GizmoOperation == ImGuizmo::ROTATE))
        m_GizmoOperation = ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale", m_GizmoOperation == ImGuizmo::SCALE))
        m_GizmoOperation = ImGuizmo::SCALE;
    float matrixTranslation[3], matrixRotation[3], matrixScale[3];
    ImGuizmo::DecomposeMatrixToComponents(matrix, matrixTranslation, matrixRotation, matrixScale);
    ImGui::InputFloat3("Tr", matrixTranslation);
    ImGui::InputFloat3("Rt", matrixRotation);
    ImGui::InputFloat3("Sc", matrixScale);
    ImGuizmo::RecomposeMatrixFromComponents(matrixTranslation, matrixRotation, matrixScale, matrix);

    if (m_GizmoOperation != ImGuizmo::SCALE)
    {
        if (ImGui::RadioButton("Local", m_GizmoMode == ImGuizmo::LOCAL))
            m_GizmoMode = ImGuizmo::LOCAL;
        ImGui::SameLine();
        if (ImGui::RadioButton("World", m_GizmoMode == ImGuizmo::WORLD))
            m_GizmoMode = ImGuizmo::WORLD;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_S))
        m_GizmoUseSnap = !m_GizmoUseSnap;
    ImGui::Checkbox(" ", &m_GizmoUseSnap);
    ImGui::SameLine();
    switch (m_GizmoOperation)
    {
    case ImGuizmo::TRANSLATE:
        ImGui::InputFloat3("Snap", &m_GizmoSnap[0]);
        break;
    case ImGuizmo::ROTATE:
        ImGui::InputFloat("Angle Snap", &m_GizmoSnap[0]);
        break;
    case ImGuizmo::SCALE:
        ImGui::InputFloat("Scale Snap", &m_GizmoSnap[0]);
        break;
    }

    ImGuiIO& io = ImGui::GetIO();
    float viewManipulateRight = io.DisplaySize.x;
    float viewManipulateTop = 0;
    static ImGuiWindowFlags gizmoWindowFlags = 0;
    ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(400, 20), ImGuiCond_Appearing);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, (ImVec4)ImColor(0.35f, 0.3f, 0.3f));
    if (m_GizmoUseWindow)
    {
       ImGui::Begin("Gizmo", 0, gizmoWindowFlags);
       ImGuizmo::SetDrawlist();
    }
    float windowWidth = (float)ImGui::GetWindowWidth();
    float windowHeight = (float)ImGui::GetWindowHeight();

    if (!m_GizmoUseWindow)
    {
       ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    }
    else
    {
       ImGuizmo::SetRect(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, windowWidth, windowHeight);
    }
    viewManipulateRight = ImGui::GetWindowPos().x + windowWidth;
    viewManipulateTop = ImGui::GetWindowPos().y;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    gizmoWindowFlags = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(window->InnerRect.Min, window->InnerRect.Max) ? ImGuiWindowFlags_NoMove : 0;

    // Draw reference grid and the manipulated object as a cube overlay
    glm::mat4 identity(1.0f);
    //ImGuizmo::DrawGrid(cameraView, cameraProjection, glm::value_ptr(identity), 100.f);
    //ImGuizmo::DrawCubes(cameraView, cameraProjection, matrix, 1);

    ImGuizmo::ViewManipulate(cameraView, m_GizmoCamDistance, ImVec2(viewManipulateRight - 128, viewManipulateTop), ImVec2(128, 128), 0x10101010);
}

void ImGuiRenderer::TransformEnd()
{
   if (m_GizmoUseWindow)
   {
      ImGui::End();
   }
   ImGui::PopStyleColor(1);
}

void ImGuiRenderer::EditTransform(float* cameraView, float* cameraProjection, float* matrix)
{
    // The scene is rendered into the dockable "Viewport" panel, so the gizmo must map to that
    // panel's screen rect (captured during the Viewport draw earlier this frame) and draw on the
    // foreground draw list so it appears on top of the scene image — not the inspector window the
    // call originates from. Skip when the Viewport is collapsed/zero-sized.
    if (!m_ViewportDrawList || m_LastViewportW < 1 || m_LastViewportH < 1)
        return;
    ImGuizmo::SetDrawlist(m_ViewportDrawList);
    ImGuizmo::SetRect(m_ViewportImageMinX, m_ViewportImageMinY,
                      static_cast<float>(m_LastViewportW), static_cast<float>(m_LastViewportH));
    ImGuizmo::Manipulate(cameraView, cameraProjection, m_GizmoOperation, m_GizmoMode, matrix, nullptr, m_GizmoUseSnap ? &m_GizmoSnap[0] : nullptr);
}

void ImGuiRenderer::Render(nvrhi::IFramebuffer* framebuffer, double deltaTime, SimulationSnapshot& snapshot, const ECS* world, float gpuFrameTimeMs) {
    ZoneScopedN("ImGui");
    {
        ZoneScopedN("ImGui_ProcessInput");
        ProcessInputEvents();

		auto viewPort = framebuffer->getFramebufferInfo().getViewport();

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(
            viewPort.width(),
            viewPort.height()
        );

        io.DeltaTime = static_cast<float>(deltaTime);
        io.MouseDrawCursor = false;
    }


    {
        ZoneScopedN("ImGuiNvrhi:Fonts");
        for (auto& font : m_fonts)
        {
            if (!font->GetScaledFont())
                font->CreateScaledFont(1.f);
        }

        m_ImGuiNvrhi->updateFontTexture();
    }


    ImGuiIO& io = ImGui::GetIO();
    {
        ZoneScopedN("ImGui:Stack");
        ImGui::NewFrame();

        // Load the ECS world snapshot atomically from ApplicationContext
        std::shared_ptr<const ECS> worldSnapshot = std::atomic_load(&m_AppContext->LatestWorldSnapshot);

        EditorContext ctx;
        ctx.App = m_AppContext;
        ctx.MeshSys = m_MeshSystem;
        ctx.MatSys = m_MaterialSystem;
        ctx.Preview = m_MeshPreviewRenderer.get();
        ctx.World = world;
        ctx.WorldSnapshot = worldSnapshot;
        ctx.Snapshot = &snapshot;
        ctx.GpuFrameTimeMs = gpuFrameTimeMs;

        ImGui::PushFont(m_fonts[0]->GetScaledFont(), 14.f);

        static bool s_LayoutInitialized = false;
        static bool s_ResetLayout = false;

        // Main Menu Bar (File / Edit / About) with placeholder items
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New", "Ctrl+N")) { /* no-op */ }
                if (ImGui::MenuItem("Open...", "Ctrl+O")) { /* no-op */ }
                ImGui::Separator();
                if (ImGui::MenuItem("Save", "Ctrl+S") && worldSnapshot) {
                    WorldManager::SaveWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, worldSnapshot.get());
                }
                if (ImGui::MenuItem("Save As...")) { /* no-op */ }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) { /* no-op */ }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                if (ImGui::MenuItem("Undo", "Ctrl+Z")) { /* no-op */ }
                if (ImGui::MenuItem("Redo", "Ctrl+Y")) { /* no-op */ }
                ImGui::Separator();
                if (ImGui::MenuItem("Cut", "Ctrl+X")) { /* no-op */ }
                if (ImGui::MenuItem("Copy", "Ctrl+C")) { /* no-op */ }
                if (ImGui::MenuItem("Paste", "Ctrl+V")) { /* no-op */ }
                if (ImGui::MenuItem("Delete", "Del")) { /* no-op */ }
                ImGui::Separator();
                if (ImGui::MenuItem("Select All", "Ctrl+A")) { /* no-op */ }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("About"))
            {
                if (ImGui::MenuItem("About This App...")) { /* no-op */ }
                if (ImGui::MenuItem("Check for Updates")) { /* no-op */ }
                if (ImGui::MenuItem("Credits")) { /* no-op */ }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Settings"))
            {
                ImGui::TextDisabled("Renderer");
                ImGui::Separator();

                // Lazy-initialize pending choice from the current persisted setting.
                if (!m_PendingBackendInitialized) {
                    m_PendingBackend = m_AppContext->Settings.Backend;
                    m_PendingBackendInitialized = true;
                }

                const char* current = SettingsManager::BackendToString(m_PendingBackend);
                if (ImGui::BeginCombo("Backend", current))
                {
                    if (ImGui::Selectable("directx12", m_PendingBackend == RendererAPI::DirectX12)) {
                        m_PendingBackend = RendererAPI::DirectX12;
                    }
                    if (ImGui::Selectable("vulkan", m_PendingBackend == RendererAPI::Vulkan)) {
                        m_PendingBackend = RendererAPI::Vulkan;
                    }

                    // DirectX 11 — disabled; backend not implemented.
                    ImGui::BeginDisabled(true);
                    ImGui::Selectable("directx11 (not implemented)", false);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("DirectX 11 backend not implemented yet.");
                    }

                    ImGui::EndCombo();
                }

                const bool dirty = (m_PendingBackend != m_AppContext->Settings.Backend);
                ImGui::BeginDisabled(!dirty);
                if (ImGui::Button("Apply##SettingsBackendApply"))
                {
                    const RendererAPI previous = m_AppContext->Settings.Backend;
                    m_AppContext->Settings.Backend = m_PendingBackend;
                    if (SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH,
                                              m_AppContext->Settings))
                    {
                        m_SettingsSaveError.clear();
                        RendererCommand swapCmd{};
                        swapCmd.Type = RendererCommandType::SwapBackend;
                        swapCmd.SwapBackend.TargetApi = m_PendingBackend;
                        if (!m_AppContext->PRCommandRing.Push(swapCmd)) {
                            m_SettingsSaveError = "Renderer busy; could not start swap. Restart to apply.";
                        }
                    }
                    else
                    {
                        m_AppContext->Settings.Backend = previous;
                        m_SettingsSaveError = "Failed to save editor_settings.json";
                    }
                }
                ImGui::EndDisabled();

                if (!m_SettingsSaveError.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                       "%s", m_SettingsSaveError.c_str());
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Reset Layout")) { s_ResetLayout = true; }
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }

        // Dockspace covering the full work area (no banner offset in Phase B).
        const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (s_ResetLayout || !s_LayoutInitialized)
        {
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
            // Build when explicitly reset, or on first run when there is no saved split layout
            // (fresh install / no imgui.ini). An existing user layout is otherwise preserved.
            if (s_ResetLayout || node == nullptr || !node->IsSplitNode())
                BuildDefaultDockLayout(dockId);
            s_LayoutInitialized = true;
            s_ResetLayout = false;
        }

        static bool s_ShowMemoryPanel = true;
        DrawMemoryPanel(&s_ShowMemoryPanel, world);

        static bool s_ShowRenderStatsPanel = true;
        DrawRenderStatsPanel(&s_ShowRenderStatsPanel);

        // Scene viewport: shows the offscreen scene RT. Zero padding so the image fills the panel.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        // NoMove so a click on the image body doesn't start a window-move (which sets an active
        // id and blocks ImGuizmo::CanActivate -> the gizmo wouldn't be draggable). The tab can
        // still be dragged to redock.
        m_ViewportDrawList = nullptr;
        if (ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoMove))
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const ImVec2 imgPos = ImGui::GetCursorScreenPos(); // image top-left in screen space (zero padding)
            m_ViewportImageMinX = imgPos.x;
            m_ViewportImageMinY = imgPos.y;
            m_LastViewportW = static_cast<uint32_t>(avail.x > 1.0f ? avail.x : 1.0f);
            m_LastViewportH = static_cast<uint32_t>(avail.y > 1.0f ? avail.y : 1.0f);
            // ImGuizmo hover-tests via the draw list's owner window (FindWindowByName), so it must
            // target THIS "Viewport" window's draw list — the foreground draw list has no owner
            // window and would make ImGuizmo think the mouse is never over the gizmo.
            m_ViewportDrawList = ImGui::GetWindowDrawList();
            if (nvrhi::ITexture* sceneTex = m_SceneViewport.ColorTexture())
                ImGui::Image(reinterpret_cast<ImTextureID>(sceneTex), avail);
        }
        ImGui::End();
        ImGui::PopStyleVar();

        // Publish the panel size for the GameThread (camera aspect + UI ortho).
        if (m_AppContext)
            m_AppContext->SceneViewportSize.store(
                (static_cast<uint64_t>(m_LastViewportW) << 32) | static_cast<uint64_t>(m_LastViewportH),
                std::memory_order_relaxed);

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::BeginFrame();

        m_StatsPanel.Draw(ctx);

        // ECS Inspector Window - demonstrates reading from ECS snapshot AND modifying via commands
        ImGui::Begin("ECS Inspector & Editor");

        if (worldSnapshot) {
            ImGui::Text("ECS World Snapshot (Tick: %llu)", snapshot.Tick);
            ImGui::Text("Entity Count: %zu", worldSnapshot->GetEntityCount());
            ImGui::Separator();

            // === ENTITY CREATION SECTION ===
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Create New Entity");

            if (ImGui::Button("Create Entity", ImVec2(200, 0))) {
                // Create entity command
                ECSCommand createCmd = ECSCommand::CreateEntity();
                if (!m_AppContext->ECSCommandRing.Push(createCmd)) {
                    SM_WARN("ECS command queue full! Create entity command dropped.");
                }

                // Note: We don't know the new entity ID yet!
                // For now, we'll just create the entity and manually add components to "last entity"
                // This is a limitation of the current one-way command system.
                ImGui::OpenPopup("Entity Created");
            }

            // Popup notification
            if (ImGui::BeginPopupModal("Entity Created", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Entity creation command sent!");
                ImGui::Text("Note: Components will be added to the newest entity.");
                ImGui::Text("Check the entity list below after next frame.");
                if (ImGui::Button("OK", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Separator();

            // === ENTITY LIST AND EDITING SECTION ===
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Entity List & Editor");

            // Track selected entity
            static EntityId selectedEntity = INVALID_ENTITY;

            // Display all entities
            for (EntityId entity : worldSnapshot->GetActiveEntities()) {
                ImGui::PushID(static_cast<int>(entity));

                // Selectable entity item
                bool isSelected = (selectedEntity == entity);
                char entityLabel[64];
                snprintf(entityLabel, sizeof(entityLabel), "Entity %llu", entity);
                if (ImGui::Selectable(entityLabel, isSelected)) {
                    selectedEntity = entity;
                }

                // Right-click context menu
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Delete Entity")) {
                        ECSCommand deleteCmd = ECSCommand::DestroyEntity(entity);
                        if (!m_AppContext->ECSCommandRing.Push(deleteCmd)) {
                            SM_WARN("ECS command queue full! Delete command dropped.");
                        }
                        if (selectedEntity == entity) {
                            selectedEntity = INVALID_ENTITY;
                        }
                    }

                    ImGui::Separator();

                    // Add component options
                    if (!worldSnapshot->HasComponent<TransformComponent>(entity)) {
                        if (ImGui::MenuItem("Add Transform Component")) {
                            TransformComponent newTransform{};
                            newTransform.Position = glm::vec3(0.0f, 0.0f, 0.0f);
                            newTransform.Rotation = glm::vec3(0.0f, 0.0f, 0.0f);
                            newTransform.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

                            ECSCommand addCmd = ECSCommand::AddComponent(entity, newTransform);
                            if (!m_AppContext->ECSCommandRing.Push(addCmd)) {
                                SM_WARN("ECS command queue full! Add component command dropped.");
                            }
                        }
                    }

                    if (!worldSnapshot->HasComponent<LightningComponent>(entity)) {
                        if (ImGui::MenuItem("Add Lightning Component")) {
                            LightningComponent newLightning{};
                            newLightning.Type = LightningType::Directional;
                            newLightning.Direction = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
                            newLightning.Color = glm::vec4(1.0f);
                            newLightning.Intensity = 1.0f;
                            ECSCommand addCmd = ECSCommand::AddComponent(entity, newLightning);
                            if (!m_AppContext->ECSCommandRing.Push(addCmd)) {
                                SM_WARN("ECS command queue full! Add component command dropped.");
                            }
                        }
                    }

                    if (!worldSnapshot->HasComponent<MeshComponent>(entity)) {
                        if (ImGui::MenuItem("Add Mesh Component")) {
                            MeshComponent newMesh{};
                            newMesh.MeshId = 0;
                            newMesh.Visible = false;

                            ECSCommand addCmd = ECSCommand::AddComponent(entity, newMesh);
                            if (!m_AppContext->ECSCommandRing.Push(addCmd)) {
                                SM_WARN("ECS command queue full! Add component command dropped.");
                            }
                        }
                    }

                    if (!worldSnapshot->HasComponent<MaterialComponent>(entity)) {
                        if (ImGui::MenuItem("Add Material Component")) {
                            MaterialComponent newMaterial{};
                            newMaterial.MaterialId = 0;
                            newMaterial.BaseColor = glm::vec4(1.0f);
                            newMaterial.Flags = 0;

                            ECSCommand addCmd = ECSCommand::AddComponent(entity, newMaterial);
                            if (!m_AppContext->ECSCommandRing.Push(addCmd)) {
                                SM_WARN("ECS command queue full! Add component command dropped.");
                            }
                        }
                    }

                    if (!worldSnapshot->HasComponent<TextComponent>(entity)) {
                        if (ImGui::MenuItem("Add Text Component")) {
                            TextComponent newText{};
                            newText.Text = "Sample text";
                            ECSCommand addCmd = ECSCommand::AddComponent(entity, newText);
                            if (!m_AppContext->ECSCommandRing.Push(addCmd)) {
                                SM_WARN("ECS command queue full! Add component command dropped.");
                            }
                        }
                    }

                    if (!worldSnapshot->HasComponent<SunMarker>(entity)) {
                        if (ImGui::MenuItem("Add Sun Marker")) {
                            ECSCommand addCmd = ECSCommand::AddComponent(entity, SunMarker{});
                            if (!m_AppContext->ECSCommandRing.Push(addCmd)) {
                                SM_WARN("ECS command queue full! Add component command dropped.");
                            }
                        }
                    }

                    ImGui::Separator();

                    // Remove component options
                    if (worldSnapshot->HasComponent<TransformComponent>(entity)) {
                        if (ImGui::MenuItem("Remove Transform Component")) {
                            ECSCommand removeCmd = ECSCommand::RemoveComponent<TransformComponent>(entity);
                            if (!m_AppContext->ECSCommandRing.Push(removeCmd)) {
                                SM_WARN("ECS command queue full! Remove component command dropped.");
                            }
                        }
                    }

                    if (worldSnapshot->HasComponent<LightningComponent>(entity)) {
                        if (ImGui::MenuItem("Remove Lightning Component")) {
                            ECSCommand removeCmd = ECSCommand::RemoveComponent<LightningComponent>(entity);
                            if (!m_AppContext->ECSCommandRing.Push(removeCmd)) {
                                SM_WARN("ECS command queue full! Remove component command dropped.");
                            }
                        }
                    }

                    if (worldSnapshot->HasComponent<MeshComponent>(entity)) {
                        if (ImGui::MenuItem("Remove Mesh Component")) {
                            ECSCommand removeCmd = ECSCommand::RemoveComponent<MeshComponent>(entity);
                            if (!m_AppContext->ECSCommandRing.Push(removeCmd)) {
                                SM_WARN("ECS command queue full! Remove component command dropped.");
                            }
                        }
                    }

                    if (worldSnapshot->HasComponent<MaterialComponent>(entity)) {
                        if (ImGui::MenuItem("Remove Material Component")) {
                            ECSCommand removeCmd = ECSCommand::RemoveComponent<MaterialComponent>(entity);
                            if (!m_AppContext->ECSCommandRing.Push(removeCmd)) {
                                SM_WARN("ECS command queue full! Remove component command dropped.");
                            }
                        }
                    }

                    if (worldSnapshot->HasComponent<TextComponent>(entity)) {
                        if (ImGui::MenuItem("Remove Text Component")) {
                            ECSCommand removeCmd = ECSCommand::RemoveComponent<TextComponent>(entity);
                            if (!m_AppContext->ECSCommandRing.Push(removeCmd)) {
                                SM_WARN("ECS command queue full! Remove component command dropped.");
                            }
                        }
                    }

                    if (worldSnapshot->HasComponent<SunMarker>(entity)) {
                        if (ImGui::MenuItem("Remove Sun Marker")) {
                            ECSCommand removeCmd = ECSCommand::RemoveComponent<SunMarker>(entity);
                            if (!m_AppContext->ECSCommandRing.Push(removeCmd)) {
                                SM_WARN("ECS command queue full! Remove component command dropped.");
                            }
                        }
                    }

                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }

            ImGui::Separator();

            // === COMPONENT EDITOR FOR SELECTED ENTITY ===
            if (selectedEntity != INVALID_ENTITY && worldSnapshot->IsValidEntity(selectedEntity)) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Editing Entity %llu", selectedEntity);
                ImGui::Separator();

                // Edit Transform Component
                if (worldSnapshot->HasComponent<TransformComponent>(selectedEntity)) {
                    if (ImGui::CollapsingHeader("Transform Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                        auto* transform = worldSnapshot->GetComponent<TransformComponent>(selectedEntity);
                        if (transform) {
                            // Create mutable copy for editing
                            static TransformComponent editTransform{};
                            static EntityId lastEditedEntity = INVALID_ENTITY;
                            static bool modified = false;

                            // Reset when switching entities
                            if (lastEditedEntity != selectedEntity) {
                                editTransform = *transform;
                                lastEditedEntity = selectedEntity;
                                modified = false;
                            }
                            // Live-refresh from snapshot every frame while not editing,
                            // so game-driven mutations (e.g. day/night) show up in inspector.
                            if (!modified) {
                                editTransform = *transform;
                            }

                            // Position editor
                            ImGui::Text("Position:");
                            if (ImGui::InputFloat3("##Position", &editTransform.Position.x)) {
                                modified = true;
                            }

                            // Rotation editor (in degrees for user-friendliness)
                            ImGui::Text("Rotation:");
                            glm::vec3 rotationDegrees = glm::degrees(editTransform.Rotation);
                            if (ImGui::InputFloat3("##Rotation", &rotationDegrees.x)) {
                                editTransform.Rotation = glm::radians(rotationDegrees);
                                modified = true;
                            }

                            // Scale editor
                            ImGui::Text("Scale:");
                            if (ImGui::InputFloat3("##Scale", &editTransform.Scale.x)) {
                                modified = true;
                            }

                            const auto hasTextTransform = worldSnapshot->HasComponent<TextComponent>(selectedEntity);
                            // ImGuizmo integration: manipulate this entity's transform using camera
                            if (!hasTextTransform) {
                                glm::mat4 cameraView(1.0f), cameraProjection(1.0f);
                                if (const auto* cam = world ? world->GetSingleton<WorldCameraComponent>() : nullptr) {
                                    cameraView = cam->View;
                                    cameraProjection = cam->Projection;
                                }

                                // Small inline gizmo controls specific to Transform component
                                ImGui::Separator();
                                ImGui::Text("Gizmo:");
                                ImGui::SameLine();
                                if (ImGui::RadioButton("Translate##GZ", m_GizmoOperation == ImGuizmo::TRANSLATE)) m_GizmoOperation = ImGuizmo::TRANSLATE;
                                ImGui::SameLine();
                                if (ImGui::RadioButton("Rotate##GZ", m_GizmoOperation == ImGuizmo::ROTATE)) m_GizmoOperation = ImGuizmo::ROTATE;
                                ImGui::SameLine();
                                if (ImGui::RadioButton("Scale##GZ", m_GizmoOperation == ImGuizmo::SCALE)) m_GizmoOperation = ImGuizmo::SCALE;

                                if (m_GizmoOperation != ImGuizmo::SCALE)
                                {
                                    if (ImGui::RadioButton("Local##GZ", m_GizmoMode == ImGuizmo::LOCAL)) m_GizmoMode = ImGuizmo::LOCAL;
                                    ImGui::SameLine();
                                    if (ImGui::RadioButton("World##GZ", m_GizmoMode == ImGuizmo::WORLD)) m_GizmoMode = ImGuizmo::WORLD;
                                }

                                // Keyboard shortcuts for convenience
                                if (ImGui::IsKeyPressed(ImGuiKey_F1)) m_GizmoOperation = ImGuizmo::TRANSLATE;
                                if (ImGui::IsKeyPressed(ImGuiKey_F2)) m_GizmoOperation = ImGuizmo::ROTATE;
                                if (ImGui::IsKeyPressed(ImGuiKey_F3)) m_GizmoOperation = ImGuizmo::SCALE;
                                if (ImGui::IsKeyPressed(ImGuiKey_F4)) m_GizmoUseSnap = !m_GizmoUseSnap;

                                ImGui::Checkbox("Snap##GZ", &m_GizmoUseSnap);
                                ImGui::SameLine();
                                if (m_GizmoOperation == ImGuizmo::TRANSLATE)
                                {
                                    ImGui::InputFloat3("Step##GZ", &m_GizmoSnap[0]);
                                }
                                else if (m_GizmoOperation == ImGuizmo::ROTATE)
                                {
                                    ImGui::InputFloat("Angle##GZ", &m_GizmoSnap[0]);
                                }
                                else // SCALE
                                {
                                    ImGui::InputFloat("Scale##GZ", &m_GizmoSnap[0]);
                                }

                                // Build model matrix from current editable transform
                                glm::mat4 T = glm::translate(glm::mat4(1.0f), editTransform.Position);
                                glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), editTransform.Rotation.x, glm::vec3(1.f, 0.f, 0.f));
                                glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), editTransform.Rotation.y, glm::vec3(0.f, 1.f, 0.f));
                                glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), editTransform.Rotation.z, glm::vec3(0.f, 0.f, 1.f));
                                glm::mat4 S  = glm::scale(glm::mat4(1.0f), editTransform.Scale);
                                glm::mat4 M  = T * Rz * Ry * Rx * S;

                                // Manipulate matrix within this inspector window
                                EditTransform(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection), glm::value_ptr(M));

                                // If user manipulated the gizmo, decompose back into component fields
                                if (ImGuizmo::IsUsing())
                                {
                                    float tr[3], rtDeg[3], sc[3];
                                    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(M), tr, rtDeg, sc);
                                    editTransform.Position = glm::vec3(tr[0], tr[1], tr[2]);
                                    editTransform.Rotation = glm::radians(glm::vec3(rtDeg[0], rtDeg[1], rtDeg[2]));
                                    editTransform.Scale    = glm::vec3(sc[0], sc[1], sc[2]);
                                    modified = true;
                                }
                            }

                            // Buttons to apply or revert changes
                            ImGui::Spacing();

                            if (modified) {
                                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
                            }

                            if (modified) {
                                modified = false;
                                ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editTransform);
                                if (!m_AppContext->ECSCommandRing.Push(modifyCmd)) {
                                    SM_WARN("ECS command queue full! Modify command dropped.");
                                }
                            }
                        }
                    }
                }

                // Edit Lightning Component
                if (worldSnapshot->HasComponent<LightningComponent>(selectedEntity)) {
                    if (ImGui::CollapsingHeader("Lightning Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                        auto* lightning = worldSnapshot->GetComponent<LightningComponent>(selectedEntity);
                        if (lightning) {
                            // Create mutable copy for editing
                            static LightningComponent editLightning{};
                            static EntityId lastEditedLightningEntity = INVALID_ENTITY;
                            static bool modified = false;
                            // Reset when switching entities
                            if (lastEditedLightningEntity != selectedEntity) {
                                editLightning = *lightning;
                                lastEditedLightningEntity = selectedEntity;
                                modified = false;
                            }
                            // Live-refresh while not editing — exposes day/night cycle changes.
                            if (!modified) {
                                editLightning = *lightning;
                            }
                            // Type editor
                            const char* types[] = { "Directional", "Point", "Spot" };
                            int currentType = static_cast<int>(editLightning.Type);
                            if (ImGui::Combo("Type", &currentType, types, IM_ARRAYSIZE(types))) {
                                editLightning.Type = static_cast<LightningType>(currentType);
                                modified = true;
                            }
                            // Direction editor
                            ImGui::Text("Direction:");
                            if (ImGui::DragFloat3("##Direction", &editLightning.Direction.x, 0.1f, -1.0f, 1.0f)) {
                                modified = true;
                            }
                            // Color editor
                            ImGui::Text("Color:");
                            if (ImGui::ColorEdit4("##Color", &editLightning.Color.r)) {
                                modified = true;
                            }
                            // Intensity editor
                            if (ImGui::DragFloat("Intensity", &editLightning.Intensity, 0.1f, 0.0f, 100.0f)) {
                                modified = true;
                            }
                            // Range editor (for Point and Spot lights)
                            if (editLightning.Type != LightningType::Directional) {
                                if (ImGui::DragFloat("Range", &editLightning.Range, 0.1f, 0.0f, 1000.0f)) {
                                    modified = true;
                                }
                            }
                            ImGui::Spacing();

                            if (modified) {
                                ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editLightning);
                                if (!m_AppContext->ECSCommandRing.Push(modifyCmd)) {
                                    SM_WARN("ECS command queue full! Modify command dropped.");
                                }
                                modified = false;
                            }
                        }
                    }
                }

                // Edit Mesh Component
                if (worldSnapshot->HasComponent<MeshComponent>(selectedEntity)) {
                    if (ImGui::CollapsingHeader("Mesh Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                        auto* mesh = worldSnapshot->GetComponent<MeshComponent>(selectedEntity);
                        if (mesh) {
                            // Create mutable copy for editing
                            static MeshComponent editMesh{};
                            static EntityId lastEditedMeshEntity = INVALID_ENTITY;
                            static bool modified = false;

                            // Reset when switching entities
                            if (lastEditedMeshEntity != selectedEntity) {
                                editMesh = *mesh;
                                lastEditedMeshEntity = selectedEntity;
                                modified = false;
                            }
                            if (!modified) {
                                editMesh = *mesh;
                            }

                            // Mesh ID editor with dropdown
                            if (m_MeshSystem) {
                                const uint32_t meshCount = m_MeshSystem->GetMeshCount();
                                if (meshCount > 0) {
                                    // Build combo items
                                    std::vector<std::string> meshItems;
                                    meshItems.reserve(meshCount);
                                    for (uint32_t i = 0; i < meshCount; ++i) {
                                        meshItems.push_back("Mesh " + std::to_string(i));
                                    }

                                    // Current selection
                                    int currentMeshIdx = static_cast<int>(editMesh.MeshId);
                                    if (currentMeshIdx >= static_cast<int>(meshCount)) {
                                        currentMeshIdx = 0; // Default to first mesh if invalid
                                    }

                                    // Combo dropdown
                                    if (ImGui::BeginCombo("Mesh ID", meshItems[currentMeshIdx].c_str())) {
                                        for (uint32_t i = 0; i < meshCount; ++i) {
                                            const bool isSelected = (currentMeshIdx == static_cast<int>(i));
                                            if (ImGui::Selectable(meshItems[i].c_str(), isSelected)) {
                                                editMesh.MeshId = i;
                                                modified = true;
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
                                if (ImGui::InputScalar("Mesh ID", ImGuiDataType_U32, &editMesh.MeshId)) {
                                    modified = true;
                                }
                            }

                            // Visibility toggle
                            if (ImGui::Checkbox("Visible", &editMesh.Visible)) {
                                modified = true;
                            }


                            ImGui::Spacing();

                            if (modified) {
                                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
                            }

                            if (ImGui::Button("Apply Changes##Mesh", ImVec2(150, 0))) {
                                ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editMesh);
                                if (!m_AppContext->ECSCommandRing.Push(modifyCmd)) {
                                    SM_WARN("ECS command queue full! Modify command dropped.");
                                }
                                modified = false;
                            }
                        }
                    }
                }

                // Edit Material Component
                if (worldSnapshot->HasComponent<MaterialComponent>(selectedEntity)) {
                    if (ImGui::CollapsingHeader("Material Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                        auto* material = worldSnapshot->GetComponent<MaterialComponent>(selectedEntity);
                        if (material) {
                            // Create mutable copy for editing
                            static MaterialComponent editMaterial{};
                            static EntityId lastEditedMaterialEntity = INVALID_ENTITY;
                            static bool modified = false;
                            // Reset when switching entities
                            if (lastEditedMaterialEntity != selectedEntity) {
                                editMaterial = *material;
                                lastEditedMaterialEntity = selectedEntity;
                                modified = false;
                            }
                            if (!modified) {
                                editMaterial = *material;
                            }
                            // Material ID editor — dropdown over MaterialSystem entries.
                            if (m_MaterialSystem) {
                                const uint32_t materialCount = m_MaterialSystem->GetMaterialCount();
                                if (materialCount > 0) {
                                    std::vector<std::string> materialItems;
                                    materialItems.reserve(materialCount);
                                    for (uint32_t i = 0; i < materialCount; ++i) {
                                        materialItems.push_back("Material " + std::to_string(i));
                                    }

                                    int currentMaterialIdx = static_cast<int>(editMaterial.MaterialId);
                                    if (currentMaterialIdx >= static_cast<int>(materialCount)) {
                                        currentMaterialIdx = 0;
                                    }

                                    if (ImGui::BeginCombo("Material ID", materialItems[currentMaterialIdx].c_str())) {
                                        for (uint32_t i = 0; i < materialCount; ++i) {
                                            const bool isSelected = (currentMaterialIdx == static_cast<int>(i));
                                            if (ImGui::Selectable(materialItems[i].c_str(), isSelected)) {
                                                editMaterial.MaterialId = i;
                                                modified = true;
                                            }
                                            if (isSelected) {
                                                ImGui::SetItemDefaultFocus();
                                            }
                                        }
                                        ImGui::EndCombo();
                                    }
                                } else {
                                    ImGui::TextDisabled("No materials loaded");
                                }
                            } else {
                                if (ImGui::InputScalar("Material ID", ImGuiDataType_U32, &editMaterial.MaterialId)) {
                                    modified = true;
                                }
                            }
                            // Base color editor
                            ImGui::Text("Base Color:");
                            if (ImGui::ColorEdit4("##BaseColor", &editMaterial.BaseColor.r)) {
                                modified = true;
                            }
                            ImGui::Spacing();

                            // Flags editor - Use Texture checkbox (bit 0)
                            bool useTexture = (editMaterial.Flags & 1u) != 0;
                            if (ImGui::Checkbox("Use Texture", &useTexture)) {
                                if (useTexture) {
                                    editMaterial.Flags |= 1u;  // Set bit 0
                                } else {
                                    editMaterial.Flags &= ~1u; // Clear bit 0
                                }
                                modified = true;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Enable texture sampling in shader (requires valid Material ID with texture)");
                            }
                            ImGui::Spacing();

                            if (modified) {
                                ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editMaterial);
                                if (!m_AppContext->ECSCommandRing.Push(modifyCmd)) {
                                    SM_WARN("ECS command queue full! Modify command dropped.");
                                }
                                modified = false;
                            }
                        }
                    }
                }

                // Edit Text Component
                if (worldSnapshot->HasComponent<TextComponent>(selectedEntity)) {
                    if (ImGui::CollapsingHeader("Text Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                        auto* textComp = worldSnapshot->GetComponent<TextComponent>(selectedEntity);
                        if (textComp) {
                            // Create mutable copy for editing
                            static TextComponent editTextComp{};
                            static EntityId lastEditedTextEntity = INVALID_ENTITY;
                            static bool modified = false;
                            // Reset when switching entities
                            if (lastEditedTextEntity != selectedEntity) {
                                editTextComp = *textComp;
                                lastEditedTextEntity = selectedEntity;
                                modified = false;
                            }
                            if (!modified) {
                                editTextComp = *textComp;
                            }
                            // Text editor
                            char buffer[256];
                            strncpy_s(buffer, editTextComp.Text.c_str(), sizeof(buffer));
                            if (ImGui::InputTextMultiline("Text", buffer, sizeof(buffer))) {
                                editTextComp.Text = std::string(buffer);
                                modified = true;
                            }
                            ImGui::Spacing();
                            // Text color editor
                            ImGui::Text("Text Color:");
                            if (ImGui::ColorEdit4("##TextColor", &editTextComp.Color.r)) {
                                modified = true;
                            }
                            ImGui::Spacing();
                            // Font size editor
                            ImGui::Text("Font Size:");
                            int fontSizeInt = static_cast<int>(editTextComp.FontSize);
                            if (ImGui::SliderInt("##FontSize", &fontSizeInt, 6, 72, "%d px")) {
                                editTextComp.FontSize = static_cast<size_t>(fontSizeInt);
                                modified = true;
                            }
                            ImGui::Spacing();
                            if (modified) {
                                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
                            }
                            if (ImGui::Button("Apply Changes##Text", ImVec2(150, 0))) {
                                ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editTextComp);
                                if (!m_AppContext->ECSCommandRing.Push(modifyCmd)) {
                                    SM_WARN("ECS command queue full! Modify command dropped.");
                                }
                                modified = false;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Revert##Text", ImVec2(150, 0))) {
                                editTextComp = *textComp;
                            }
                            ImGui::Separator();
                            // Show original values from snapshot (read-only)
                            ImGui::TextDisabled("Original values from snapshot:");
                            ImGui::TextDisabled("Text: %s", textComp->Text.c_str());
                        }
                    }
                }

                // Sun Marker — zero-size tag; display as read-only badge.
                if (worldSnapshot->HasComponent<SunMarker>(selectedEntity)) {
                    if (ImGui::CollapsingHeader("Sun Marker", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Tagged as Sun");
                        ImGui::TextDisabled("Day/night cycle drives this entity.");
                    }
                }

                // Show if entity has no components
                if (!worldSnapshot->HasComponents<TransformComponent, LightningComponent, MeshComponent, MaterialComponent, TextComponent>(selectedEntity)
                    && !worldSnapshot->HasComponent<SunMarker>(selectedEntity)) {
                    ImGui::TextDisabled("Entity has no components.");
                    ImGui::TextDisabled("Right-click the entity to add components.");
                }
            } else if (selectedEntity != INVALID_ENTITY) {
                ImGui::TextDisabled("Selected entity no longer exists.");
                if (ImGui::Button("Clear Selection")) {
                    selectedEntity = INVALID_ENTITY;
                }
            } else {
                ImGui::TextDisabled("Select an entity to edit its components.");
            }

        } else {
            ImGui::TextDisabled("No ECS snapshot available");
        }

        ImGui::End();

        // Mesh Manager Window
        ImGui::Begin("Mesh Manager");

        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Mesh System");
        ImGui::Separator();

        if (m_MeshSystem) {
            const uint32_t meshCount = m_MeshSystem->GetMeshCount();
            ImGui::Text("Loaded Meshes: %u", meshCount);

            ImGui::Spacing();

            // Track selected mesh
            static int selectedMeshId = -1;

            // Display list of loaded meshes with selection
            if (meshCount > 0) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Available Meshes:");
                for (uint32_t i = 0; i < meshCount; ++i) {
                    char label[64];
                    snprintf(label, sizeof(label), "Mesh %u", i);
                    const bool isSelected = (selectedMeshId == static_cast<int>(i));
                    if (ImGui::Selectable(label, isSelected)) {
                        selectedMeshId = static_cast<int>(i);
                    }
                }
                ImGui::Separator();

                // Show preview of selected mesh
                if (selectedMeshId >= 0 && selectedMeshId < static_cast<int>(meshCount)) {
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Mesh Preview:");
                    ImGui::Spacing();

                    auto meshBounds = m_MeshSystem->GetMeshBounds(static_cast<uint32_t>(selectedMeshId));
                    auto meshResources = m_MeshSystem->GetMeshResources(static_cast<uint32_t>(selectedMeshId));

                    if (meshBounds.valid && meshResources.valid && m_MeshPreviewRenderer) {
                        // 3D mesh preview with camera controls
                        constexpr float previewSize = 256.0f;

                        // Calculate default camera distance based on mesh size
                        const glm::vec3 meshSize = meshBounds.max - meshBounds.min;
                        const float maxExtent = glm::max(glm::max(meshSize.x, meshSize.y), meshSize.z);
                        const float defaultDistance = maxExtent * 2.0f;

                        // Reset camera to default on first view of this mesh
                        static int lastViewedMesh = -1;
                        if (lastViewedMesh != selectedMeshId)
                        {
                            lastViewedMesh = selectedMeshId;
                            m_MeshPreviewState.cameraDistance = defaultDistance;
                            m_MeshPreviewState.cameraYaw = 0.0f;
                            m_MeshPreviewState.cameraPitch = 0.3f;
                        }

                        // Render mesh to offscreen texture
                        nvrhi::ITexture* previewTexture = m_MeshPreviewRenderer->RenderMeshPreview(
                            m_MeshSystem,
                            static_cast<uint32_t>(selectedMeshId),
                            m_MeshPreviewState.cameraDistance,
                            m_MeshPreviewState.cameraYaw,
                            m_MeshPreviewState.cameraPitch
                        );

                        if (previewTexture)
                        {
                            const ImVec2 previewPos = ImGui::GetCursorScreenPos();

                            // Display rendered preview
                            ImGui::Image(
                                reinterpret_cast<ImTextureID>(previewTexture),
                                ImVec2(previewSize, previewSize),
                                ImVec2(0, 0), ImVec2(1, 1),
                                ImVec4(1, 1, 1, 1),
                                ImVec4(0.3f, 0.3f, 0.3f, 1.0f) // Border
                            );

                            // Camera controls (drag to rotate, wheel to zoom)
                            if (ImGui::IsItemHovered())
                            {
                                // Mouse wheel zoom
                                if (io.MouseWheel != 0.0f)
                                {
                                    m_MeshPreviewState.cameraDistance *= (1.0f - io.MouseWheel * 0.1f);
                                    m_MeshPreviewState.cameraDistance = glm::clamp(
                                        m_MeshPreviewState.cameraDistance,
                                        maxExtent * 0.5f,
                                        maxExtent * 10.0f
                                    );
                                }

                                // Mouse drag to rotate
                                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                                {
                                    if (!m_MeshPreviewState.isDragging)
                                    {
                                        m_MeshPreviewState.isDragging = true;
                                        m_MeshPreviewState.lastMouseX = io.MousePos.x;
                                        m_MeshPreviewState.lastMouseY = io.MousePos.y;
                                    }
                                    else
                                    {
                                        const float deltaX = io.MousePos.x - m_MeshPreviewState.lastMouseX;
                                        const float deltaY = io.MousePos.y - m_MeshPreviewState.lastMouseY;

                                        m_MeshPreviewState.cameraYaw += deltaX * 0.01f;
                                        m_MeshPreviewState.cameraPitch -= deltaY * 0.01f;

                                        // Clamp pitch to avoid gimbal lock
                                        m_MeshPreviewState.cameraPitch = glm::clamp(
                                            m_MeshPreviewState.cameraPitch,
                                            -1.5f, 1.5f
                                        );

                                        m_MeshPreviewState.lastMouseX = io.MousePos.x;
                                        m_MeshPreviewState.lastMouseY = io.MousePos.y;
                                    }
                                }
                                else
                                {
                                    m_MeshPreviewState.isDragging = false;
                                }
                            }
                            else
                            {
                                m_MeshPreviewState.isDragging = false;
                            }

                            // Display controls hint
                            ImGui::TextDisabled("(Drag to rotate, scroll to zoom)");
                        }
                        else
                        {
                            ImGui::TextDisabled("(Preview rendering failed)");
                        }

                        ImGui::Spacing();

                        // Display mesh statistics below visualization
                        ImGui::Text("Mesh ID: %d", selectedMeshId);
                        ImGui::Text("Vertices: %u", meshResources.vertexCount);
                        ImGui::Text("Indices: %u", meshResources.indexCount);
                        ImGui::Text("Triangles: %u", meshResources.indexCount / 3);
                        ImGui::Text("Bounds: (%.2f, %.2f, %.2f) - (%.2f, %.2f, %.2f)",
                                   meshBounds.min.x, meshBounds.min.y, meshBounds.min.z,
                                   meshBounds.max.x, meshBounds.max.y, meshBounds.max.z);
                    } else {
                        ImGui::TextDisabled("(No mesh data available)");
                    }
                } else {
                    ImGui::TextDisabled("Select a mesh to see preview");
                }

                ImGui::Separator();
            }

            // Load mesh from file button
            static char statusMessage[512] = "";
            static ImVec4 statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

            if (ImGui::Button("Load Mesh from File", ImVec2(200, 0))) {
                char filePath[512] = "";
                const char* filter = "3D Model Files\0*.obj;*.fbx;*.gltf;*.glb;*.dae\0"
                                     "OBJ Files (*.obj)\0*.obj\0"
                                     "FBX Files (*.fbx)\0*.fbx\0"
                                     "GLTF Files (*.gltf;*.glb)\0*.gltf;*.glb\0"
                                     "All Files (*.*)\0*.*\0\0";

                if (EditorFileDialog::Open(filePath, sizeof(filePath), filter)) {
                    // File selected, load it using MeshLoader
                    std::vector<MeshVertex> vertices;
                    std::vector<uint32_t> indices;
                    std::vector<MeshLoader::MeshMaterial> materials;
                    std::vector<SubMesh> subMeshes;
                    std::string error;

                    if (MeshLoader::LoadMeshFromFile(filePath, vertices, indices, subMeshes, materials, error)) {
                        bool hasSubMeshes = !subMeshes.empty();
                        // Successfully loaded, upload to GPU via MeshSystem
                        MeshHandle meshHandle = m_MeshSystem->AddMesh(
                            vertices.data(), static_cast<uint32_t>(vertices.size()),
                            indices.data(), static_cast<uint32_t>(indices.size()),
                            hasSubMeshes ? subMeshes.data() : nullptr,
                            hasSubMeshes ? static_cast<uint32_t>(subMeshes.size()) : 0
                        );

                        for (const auto& mat : materials) {
                            MaterialHandle materialHandle = m_MaterialSystem->AddMaterial(
                                mat.TextureData.data(), mat.Width, mat.Height
                            );
                            m_MeshSystem->AssociateMeshMaterial(meshHandle, materialHandle, mat.MaterialIndex);
                        }

                        if (meshHandle.Index != UINT32_MAX) {
                            snprintf(statusMessage, sizeof(statusMessage),
                                    "Success! Loaded mesh %u (%zu vertices, %zu indices)",
                                    meshHandle.Index, vertices.size(), indices.size());
                            statusColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // Green
                        } else {
                            snprintf(statusMessage, sizeof(statusMessage),
                                    "Failed to upload mesh to GPU");
                            statusColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red
                        }
                    } else {
                        // Failed to load
                        snprintf(statusMessage, sizeof(statusMessage),
                                "Failed to load mesh: %s", error.c_str());
                        statusColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red
                    }
                }
            }

            // Display status message if any
            if (statusMessage[0] != '\0') {
                ImGui::Spacing();
                ImGui::TextColored(statusColor, "%s", statusMessage);
            }

        } else {
            ImGui::TextDisabled("MeshSystem not available");
        }

        ImGui::End();

        m_MaterialManager.Draw(ctx);

        ImGui::PopFont();
    }

    {
        ImGui::Render();
        ZoneScopedN("ImGui:Render");
    }


    {
        ZoneScopedN("ImGuiNvrhi:Render");
        m_ImGuiNvrhi->render(framebuffer);
    }

    // Update and Render additional Platform Windows
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ZoneScopedN("UpdatePlatformWindows");
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

ImGuiRenderer::ImGuiRenderer() = default;

ImGuiRenderer::~ImGuiRenderer() {
    Shutdown();
}

nvrhi::IFramebuffer* ImGuiRenderer::GetSceneFramebuffer(nvrhi::IFramebuffer* swapChainFb)
{
    if (!swapChainFb)
        return nullptr;

    const auto& info = swapChainFb->getFramebufferInfo();
    const nvrhi::Format colorFormat = info.colorFormats[0];
    const uint32_t      sampleCount = info.sampleCount;

    // Frame 1 (before the Viewport panel has reported a size): default to the swapchain size.
    const uint32_t w = m_LastViewportW ? m_LastViewportW : info.width;
    const uint32_t h = m_LastViewportH ? m_LastViewportH : info.height;

    return m_SceneViewport.EnsureTargets(w, h, colorFormat, sampleCount);
}

void ImGuiRenderer::Shutdown() {
    m_ImGuiNvrhi.reset();
    m_ImGuiNvrhi = nullptr;

    if (m_MeshPreviewRenderer)
    {
        m_MeshPreviewRenderer->Shutdown();
        m_MeshPreviewRenderer.reset();
    }

    m_SceneViewport.Release();

    m_MeshSystem = nullptr;
    m_MaterialSystem = nullptr;
    ImGui::DestroyContext();
}

void ImGuiRenderer::ShutdownNvrhiOnly() {
    // Device-bound resources only. ImGui context + fonts stay alive.
    if (m_MeshPreviewRenderer) {
        m_MeshPreviewRenderer.reset();
    }
    m_SceneViewport.Release();
    if (m_ImGuiNvrhi) {
        m_ImGuiNvrhi.reset();
    }
}

bool ImGuiRenderer::InitNvrhiForDevice(nvrhi::IDevice* device) {
    if (!device) {
        SM_ERROR("ImGuiRenderer::InitNvrhiForDevice: null device");
        return false;
    }

    m_ImGuiNvrhi = std::make_unique<ImGui_NVRHI>();
    if (!m_ImGuiNvrhi->init(device)) {
        SM_ERROR("ImGuiRenderer::InitNvrhiForDevice: ImGui_NVRHI init failed");
        return false;
    }

    m_MeshPreviewRenderer = std::make_unique<MeshPreviewRenderer>();
    if (!m_MeshPreviewRenderer->Initialize(device, m_Renderer, 256, 256)) {
        SM_ERROR("ImGuiRenderer::InitNvrhiForDevice: MeshPreviewRenderer init failed");
        m_MeshPreviewRenderer.reset();
        // Non-fatal: preview just won't render. Continue.
    }

    m_SceneViewport.Init(device);

    // Re-upload the font atlas against the new device.
    m_ImGuiNvrhi->updateFontTexture();

    return true;
}

std::shared_ptr<RegisteredFont> ImGuiRenderer::CreateFontFromFile(const char *fontFile, float fontSize) {
    auto fontData = std::make_shared<Blob>();

    std::ifstream fileStream(fontFile, std::ios::binary | std::ios::ate);
    if (!fileStream.is_open())
    {
        return nullptr;
    }

    void* fileBuffer = nullptr;
    std::streamsize const fileSize = fileStream.tellg();
    fileStream.seekg(0, std::ios::beg);
    fileBuffer = malloc(static_cast<size_t>(fileSize));
    if (!fileStream.read(static_cast<char*>(fileBuffer), fileSize))
    {
        free(fileBuffer);
        return nullptr;
    }
    fontData->size = static_cast<size_t>(fileSize);
    fontData->data = fileBuffer;

    auto font = std::make_shared<RegisteredFont>(fontData, false, fontSize);
    m_fonts.push_back(font);

    return std::move(font);
}

void ImGuiRenderer::ProcessInputEvents() {
    if (!m_AppContext) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    // Process all pending input events from the ImGui ring buffer
    InputEvent event;
    while (m_AppContext->ImGuiInputRing.Pop(event)) {
        switch (event.Type) {
            case InputEventType::MouseMove: {
                io.AddMousePosEvent(static_cast<float>(event.MouseMoveEvent.X),
                                   static_cast<float>(event.MouseMoveEvent.Y));
                break;
            }

            case InputEventType::MouseButton: {
                const int button = static_cast<int>(event.MouseButtonEvent.Button);
                if (button >= 0 && button < ImGuiMouseButton_COUNT) {
                    const bool isDown = (event.MouseButtonEvent.Action == InputAction::PRESS);
                    io.AddMouseButtonEvent(button, isDown);
                }
                break;
            }

            case InputEventType::MouseWheel: {
                io.AddMouseWheelEvent(static_cast<float>(event.MouseScrollEvent.OffsetX),
                                     static_cast<float>(event.MouseScrollEvent.OffsetY));
                break;
            }

            case InputEventType::Key: {
                // Map GLFW keys to ImGuiKey
                ImGuiKey imguiKey = GlfwKeyToImGuiKey(event.KeyEvent.Key);
                const bool isDown = (event.KeyEvent.Action == InputAction::PRESS ||
                                    event.KeyEvent.Action == InputAction::REPEAT);

                io.AddKeyEvent(imguiKey, isDown);

                // Update modifier keys
                const KeyModifier mods = event.KeyEvent.Modifier;
                io.AddKeyEvent(ImGuiMod_Ctrl, (mods & KeyModifier::CONTROL) != 0);
                io.AddKeyEvent(ImGuiMod_Shift, (mods & KeyModifier::SHIFT) != 0);
                io.AddKeyEvent(ImGuiMod_Alt, (mods & KeyModifier::ALT) != 0);
                io.AddKeyEvent(ImGuiMod_Super, (mods & KeyModifier::SUPER) != 0);
                break;
            }

            case InputEventType::TextInput: {
                if (event.TextEvent.Key > 0 && event.TextEvent.Key < 0x10000) {
                    io.AddInputCharacter(event.TextEvent.Key);
                }
                break;
            }
        }
    }
}
