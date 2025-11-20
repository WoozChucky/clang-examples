#include "ImGuiRenderer.h"

#include <fstream>
#include <cstdio>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>

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

bool ImGuiRenderer::Init(GLFWwindow* window, nvrhi::IDevice* device, ApplicationContext* appContext) {
    m_AppContext = appContext;

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
    ImGuiIO& io = ImGui::GetIO();
    const auto windowWidth = ImGui::GetWindowWidth();
    const auto windowHeight = ImGui::GetWindowHeight();
    if (!m_GizmoUseWindow)
    {
        ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    }
    else
    {
        ImGuizmo::SetRect(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, windowWidth, windowHeight);
    }
    ImGuizmo::Manipulate(cameraView, cameraProjection, m_GizmoOperation, m_GizmoMode, matrix, nullptr, m_GizmoUseSnap ? &m_GizmoSnap[0] : nullptr);
}

void ImGuiRenderer::Render(nvrhi::IFramebuffer* framebuffer, double deltaTime, SimulationSnapshot& snapshot) {
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

        ImGui::PushFont(m_fonts[0]->GetScaledFont(), 14.f);

        // Main Menu Bar (File / Edit / About) with placeholder items
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New", "Ctrl+N")) { /* no-op */ }
                if (ImGui::MenuItem("Open...", "Ctrl+O")) { /* no-op */ }
                ImGui::Separator();
                if (ImGui::MenuItem("Save", "Ctrl+S")) { /* no-op */ }
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

            ImGui::EndMainMenuBar();
        }

        // Ensure a DockSpace
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

        // Build layout once
        static bool s_dockBuilt = false;
        if (!s_dockBuilt)
        {
            ImGuiID dockspace_id = ImGui::GetMainViewport()->ID; // Dock node for main viewport

            // Rebuild the dockspace to a known layout
            ImGui::DockBuilderRemoveNode(dockspace_id);                      // clear any previous layout
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

            ImGuiID dock_id_right = 0;
            ImGuiID dock_id_main  = dockspace_id;

            // Split main dockspace: create a right dock at 28% width; remainder stays as main
            ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Right, 0.28f, &dock_id_right, &dock_id_main);

            // Dock your windows by exact title text
            ImGui::DockBuilderDockWindow("3D Engine Overlay", dock_id_right);
            ImGui::DockBuilderDockWindow("Hello, world!",      dock_id_main);

            ImGui::DockBuilderFinish(dockspace_id);
            s_dockBuilt = true;
        }

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::BeginFrame();

        ImGui::Begin("Hello, world!");

        ImGui::Text("Renderer average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::Text("Game TPS: %.2f/%.2f", snapshot.ActualTPS, snapshot.TargetTPS);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Game Thread Settings");

        // Read current settings
        GameThreadSettings settings = m_AppContext->GameThreadConfig.load();
        bool settingsChanged = false;

        // Target TPS slider with common presets
        ImGui::Text("Target TPS:");
        ImGui::SameLine();
        if (ImGui::Button("60")) { settings.TargetTPS = 60.0; settingsChanged = true; }
        ImGui::SameLine();
        if (ImGui::Button("120")) { settings.TargetTPS = 120.0; settingsChanged = true; }
        ImGui::SameLine();
        if (ImGui::Button("144")) { settings.TargetTPS = 144.0; settingsChanged = true; }
        ImGui::SameLine();
        if (ImGui::Button("165")) { settings.TargetTPS = 165.0; settingsChanged = true; }

        float targetTpsFloat = static_cast<float>(settings.TargetTPS);
        if (ImGui::SliderFloat("##TargetTPS", &targetTpsFloat, 60.0f, 240.0f, "%.1f Hz")) {
            settings.TargetTPS = static_cast<double>(targetTpsFloat);
            settingsChanged = true;
        }

        // Spin threshold in microseconds
        int spinThresholdInt = static_cast<int>(settings.SpinThresholdMicros);
        if (ImGui::SliderInt("Spin Threshold (μs)", &spinThresholdInt, 0, 2000, "%d μs")) {
            settings.SpinThresholdMicros = static_cast<uint32_t>(spinThresholdInt);
            settingsChanged = true;
        }
        ImGui::TextWrapped("Lower = more accurate timing, higher CPU usage during spin");

        // Frame time tracking toggle
        if (ImGui::Checkbox("Enable Frame Time Tracking", &settings.EnableFrameTimeTracking)) {
            settingsChanged = true;
        }

        // Write settings back if changed
        if (settingsChanged) {
            m_AppContext->GameThreadConfig.store(settings);
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Frame Time Statistics");

        // Use snapshot's frame stats (passed from RenderThread, which got it from GameThread)
        if (settings.EnableFrameTimeTracking && snapshot.FrameStats.SampleCount > 0) {
            ImGui::Text("Min: %.3f ms", snapshot.FrameStats.MinFrameTimeMs);
            ImGui::Text("Max: %.3f ms", snapshot.FrameStats.MaxFrameTimeMs);
            ImGui::Text("Avg: %.3f ms", snapshot.FrameStats.AvgFrameTimeMs);
            ImGui::Text("Samples: %llu", snapshot.FrameStats.SampleCount);

            // Reset button
            if (ImGui::Button("Reset Stats")) {
                // This will be picked up by game thread on next frame
                // (stats reset happens in game thread when sample count wraps or manually)
            }
        } else {
            ImGui::TextDisabled("(Frame time tracking disabled)");
        }

        ImGui::Separator();
        ImGui::End();

        // ECS Inspector Window - demonstrates reading from ECS snapshot AND modifying via commands
        ImGui::Begin("ECS Inspector & Editor");

        // Load the ECS world snapshot atomically from ApplicationContext
        std::shared_ptr<const ECS> worldSnapshot = std::atomic_load(&m_AppContext->LatestWorldSnapshot);

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
                            newMaterial.TextureId = 0;
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

                            // Reset when switching entities
                            if (lastEditedEntity != selectedEntity) {
                                editTransform = *transform;
                                lastEditedEntity = selectedEntity;
                            }

                            static bool modified = false;

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
                                auto cameraView = snapshot.GameCamera.get_view_matrix();
                                auto cameraProjection = snapshot.GameCamera.get_projection_matrix();

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
                            // Reset when switching entities
                            if (lastEditedLightningEntity != selectedEntity) {
                                editLightning = *lightning;
                                lastEditedLightningEntity = selectedEntity;
                            }
                            static bool modified = false;
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

                            // Reset when switching entities
                            if (lastEditedMeshEntity != selectedEntity) {
                                editMesh = *mesh;
                                lastEditedMeshEntity = selectedEntity;
                            }

                            static bool modified = false;

                            // Mesh ID editor
                            if (ImGui::InputScalar("Mesh ID", ImGuiDataType_U32, &editMesh.MeshId)) {
                                modified = true;
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
                            // Reset when switching entities
                            if (lastEditedMaterialEntity != selectedEntity) {
                                editMaterial = *material;
                                lastEditedMaterialEntity = selectedEntity;
                            }
                            static bool modified = false;
                            // Material ID editor
                            if (ImGui::InputScalar("Material ID", ImGuiDataType_U32, &editMaterial.MaterialId)) {
                                modified = true;
                            }
                            // Texture ID editor
                            if (ImGui::InputScalar("Texture ID", ImGuiDataType_U32, &editMaterial.TextureId)) {
                                modified = true;
                            }
                            // Base color editor
                            ImGui::Text("Base Color:");
                            if (ImGui::ColorEdit4("##BaseColor", &editMaterial.BaseColor.r)) {
                                modified = true;
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
                            // Reset when switching entities
                            if (lastEditedTextEntity != selectedEntity) {
                                editTextComp = *textComp;
                                lastEditedTextEntity = selectedEntity;
                            }
                            static bool modified = false;
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

                // Show if entity has no components
                if (!worldSnapshot->HasComponents<TransformComponent, LightningComponent, MeshComponent, MaterialComponent, TextComponent>(selectedEntity)) {
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

void ImGuiRenderer::Shutdown() {
    m_ImGuiNvrhi.reset();
    m_ImGuiNvrhi = nullptr;
    ImGui::DestroyContext();
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


