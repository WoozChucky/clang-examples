#include "ImGuiRenderer.h"

#include <fstream>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include "MeshSystem.h"
#include "MaterialSystem.h"
#include "MeshPreviewRenderer.h"
#include "MemoryPanel.h"
#include "RenderStatsPanel.h"
#include "RenderStats.h"
#include "EditorPreferences.h"
#include "utilities/SettingsManager.h"
#include "EditorContext.h"
#include "PerformancePanel.h"
#include "SimulationPanel.h"
#include "MaterialManagerPanel.h"
#include "MeshManagerPanel.h"
#include "EcsInspectorPanel.h"
#include "DayNightPanel.h"
#include "NavigationPanel.h"
#include "ViewportPicker.h"
#include "MainMenuBar.h"
#include "TransformMath.h"   // ModelMatrix
#include "Frustum.h"         // TransformAABB

#include "ApplicationContext.h"
#include "registered_font.h"
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
    ImGui::DockBuilderDockWindow("Performance",            leftBottom);
    ImGui::DockBuilderDockWindow("Simulation",             leftBottom);
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

    // Ground grid is an editor authoring aid — on by default in the editor, never in
    // the runtime (separate process, no overlay, leaves the flag at its default false).
    GetDebugDrawSettings().ShowGrid = true;

    // Restore persisted RenderStats toggles + editor camera pose. Runs after the editor
    // defaults above so a saved file wins; first run (no file) keeps the defaults.
    EditorCameraState cs = m_EditorCamera.GetState();
    EditorPreferences::Load(EditorPreferences::DEFAULT_PREFERENCES_PATH, cs);
    m_EditorCamera.SetState(cs);

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

        const MainMenuBarResult menuResult = m_MenuBar.Draw(ctx, m_EditMode);
        if (menuResult.resetLayout) s_ResetLayout = true;
        if (menuResult.sceneChanged) m_EcsInspector.SetSelectedEntity(INVALID_ENTITY); // ids invalid after New/Reload
        if (ImGui::IsKeyPressed(ImGuiKey_F6) && !io.WantTextInput)
            m_EditMode = !m_EditMode;

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
        if (DrawRenderStatsPanel(&s_ShowRenderStatsPanel)) {
            // Debug-draw/shadow toggles persist in editor_preferences.json.
            EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH, m_EditorCamera.GetState());
            // FXAA persists in engine_settings.json (engine tier). Only write when it
            // actually changed so unrelated panel edits don't rewrite engine settings.
            const int aaNow = static_cast<int>(GetAntiAliasingSettings().Mode);
            if (m_AppContext && aaNow != m_AppContext->Settings.aaMode) {
                m_AppContext->Settings.aaMode = aaNow;
                if (!SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH, m_AppContext->Settings)) {
                    SM_WARN("Failed to persist AA mode to %s", SettingsManager::DEFAULT_SETTINGS_PATH);
                }
            }
        }

        // Scene viewport: shows the offscreen scene RT. Zero padding so the image fills the panel.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        // NoMove so a click on the image body doesn't start a window-move (which sets an active
        // id and blocks ImGuizmo::CanActivate -> the gizmo wouldn't be draggable). The tab can
        // still be dragged to redock.
        m_ViewportDrawList = nullptr;
        m_ViewportHovered = false;
        m_ViewportFocused = false;
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
            m_ViewportHovered = ImGui::IsWindowHovered();
            m_ViewportFocused = ImGui::IsWindowFocused();
            if (nvrhi::ITexture* sceneTex = m_SceneViewport.ColorTexture())
                ImGui::Image(reinterpret_cast<ImTextureID>(sceneTex), avail);
        }
        ImGui::End();
        ImGui::PopStyleVar();

        ctx.ViewportDrawList = m_ViewportDrawList;
        ctx.ViewportMinX = m_ViewportImageMinX;
        ctx.ViewportMinY = m_ViewportImageMinY;
        ctx.ViewportW = m_LastViewportW;
        ctx.ViewportH = m_LastViewportH;

        // ---- Editor camera (Edit mode) ----
        const float vpAspect = m_LastViewportH ? float(m_LastViewportW) / float(m_LastViewportH)
                                               : 16.0f / 9.0f;
        if (m_EditMode && (m_ViewportHovered || m_ViewportFocused)) {
            EditorCameraInput cin{};
            cin.MouseDX = io.MouseDelta.x;
            cin.MouseDY = io.MouseDelta.y;
            cin.Wheel   = io.MouseWheel;
            cin.Look    = ImGui::IsMouseDown(ImGuiMouseButton_Right);
            cin.Pan     = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
            cin.Orbit   = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            cin.W = ImGui::IsKeyDown(ImGuiKey_W); cin.S = ImGui::IsKeyDown(ImGuiKey_S);
            cin.A = ImGui::IsKeyDown(ImGuiKey_A); cin.D = ImGui::IsKeyDown(ImGuiKey_D);
            cin.Q = ImGui::IsKeyDown(ImGuiKey_Q); cin.E = ImGui::IsKeyDown(ImGuiKey_E);
            cin.Frame = ImGui::IsKeyPressed(ImGuiKey_F);
            cin.DeltaSeconds = io.DeltaTime;

            // Pivot for orbit/frame = the selected entity's world AABB center + radius.
            const EntityId sel = m_EcsInspector.GetSelectedEntity();
            if (sel != INVALID_ENTITY && world) {
                const auto* tc = world->GetComponent<TransformComponent>(sel);
                const auto* mc = world->GetComponent<MeshComponent>(sel);
                if (tc) {
                    if (mc && m_MeshSystem) {
                        const auto b = m_MeshSystem->GetMeshBounds(mc->MeshId);
                        if (b.valid) {
                            glm::vec3 wMin, wMax;
                            TransformAABB(ModelMatrix(*tc), b.min, b.max, wMin, wMax);
                            cin.PivotCenter = 0.5f * (wMin + wMax);
                            cin.PivotRadius = 0.5f * glm::length(wMax - wMin);
                            cin.HasPivot = true;
                        }
                    }
                    if (!cin.HasPivot) { // no mesh bounds -> pivot on the transform position
                        cin.PivotCenter = tc->Position;
                        cin.PivotRadius = 1.0f;
                        cin.HasPivot = true;
                    }
                }
            }
            m_EditorCamera.Update(cin);
        }

        // Publish the override channel for the Renderer (1-frame lag, like SelectedEntity).
        const CameraView editorCam = m_EditorCamera.ToCameraView(vpAspect);
        if (m_AppContext) {
            m_AppContext->EditorCameraActive.store(m_EditMode, std::memory_order_relaxed);
            m_AppContext->EditorCamera.store(editorCam);
        }
        // Hand the editor camera to picking (consumed in Task 5).
        ctx.EditorCameraActive = m_EditMode;
        ctx.EditorCamView = editorCam.View;
        ctx.EditorCamProj = editorCam.Projection;

        // Viewport pick: left-click selects the entity under the cursor (edit mode). Skip when
        // over/using a gizmo (that click manipulates the gizmo). Empty space -> deselect.
        if (m_ViewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing() && !io.KeyAlt && ctx.EditorCameraActive) {
            m_EcsInspector.SetSelectedEntity(PickEntity(ctx, io.MousePos.x, io.MousePos.y));
        }

        // Publish the panel size for the GameThread (camera aspect + UI ortho).
        if (m_AppContext) {
            m_AppContext->SceneViewportSize.store(
                (static_cast<uint64_t>(m_LastViewportW) << 32) | static_cast<uint64_t>(m_LastViewportH),
                std::memory_order_relaxed);
            // Scene-viewport image top-left (screen-space, same value the picker uses) so the
            // GameThread can offset the window-space mouse into UI space.
            // Clamp to >= 0 before the unsigned cast: a detached Viewport dragged off the
            // top-left can yield a negative GetCursorScreenPos(), and float->uint32 of a
            // negative value is UB.
            const uint32_t originX = static_cast<uint32_t>(m_ViewportImageMinX > 0.0f ? m_ViewportImageMinX : 0.0f);
            const uint32_t originY = static_cast<uint32_t>(m_ViewportImageMinY > 0.0f ? m_ViewportImageMinY : 0.0f);
            m_AppContext->SceneViewportOrigin.store(
                (static_cast<uint64_t>(originX) << 32) | static_cast<uint64_t>(originY),
                std::memory_order_relaxed);
        }

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::BeginFrame();

        m_Performance.Draw(ctx);
        m_Simulation.Draw(ctx);

        m_EcsInspector.Draw(ctx);

        static bool s_ShowDayNightPanel = true;
        DrawDayNightPanel(ctx, &s_ShowDayNightPanel);

        static bool s_ShowNavigationPanel = true;
        DrawNavigationPanel(ctx, &s_ShowNavigationPanel);

        if (m_AppContext)
            m_AppContext->SelectedEntity.store(m_EcsInspector.GetSelectedEntity(), std::memory_order_relaxed);

        m_MeshManager.Draw(ctx);

        m_MaterialManager.Draw(ctx);

        // Publish input-routing flags for the PlatformThread: the game gets mouse only when the
        // Viewport is hovered (and no gizmo drag), keyboard only when it's focused (and no text
        // field is active). Computed after the panels so ImGui's WantTextInput is up to date.
        if (m_AppContext) {
            // In Edit mode the editor camera consumes viewport input, so the game gets none (its
            // free-look would otherwise fight the editor camera). In Play mode keep the prior rule.
            const bool toGameMouse    = !m_EditMode && m_ViewportHovered && !ImGuizmo::IsUsing();
            const bool toGameKeyboard = !m_EditMode && m_ViewportFocused && !io.WantTextInput;
            m_AppContext->GameAcceptsMouse.store(toGameMouse, std::memory_order_relaxed);
            m_AppContext->GameAcceptsKeyboard.store(toGameKeyboard, std::memory_order_relaxed);
        }

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
    // Persist final editor preferences (RenderStats toggles + camera pose) on exit.
    EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH, m_EditorCamera.GetState());

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
