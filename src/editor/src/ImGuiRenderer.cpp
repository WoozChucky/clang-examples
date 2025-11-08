#include "ImGuiRenderer.h"

#include <fstream>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

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

void ImGuiRenderer::Render(nvrhi::IFramebuffer* framebuffer, double deltaTime) {
    ZoneScopedN("ImGui");
    {
        ZoneScopedN("ImGui_ProcessInput");
        ProcessInputEvents();

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(
            framebuffer->getFramebufferInfo().getViewport().width(),
            framebuffer->getFramebufferInfo().getViewport().height()
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

        ImGui::Begin("Hello, world!");
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::Text("Hello from the dynamically loaded UI Overlay!");
        ImGui::Separator();
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
    std::streamsize fileSize = fileStream.tellg();
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


