#pragma once

#include <memory>
#include <nvrhi/nvrhi.h>
#include <GLFW/glfw3.h>

#include "imgui_nvrhi.h"

class RegisteredFont;
struct ApplicationContext;
struct SimulationSnapshot;

class ImGuiRenderer final {
public:
    ImGuiRenderer() = default;
    ~ImGuiRenderer() {
        Shutdown();
    }

    bool Init(GLFWwindow* window, nvrhi::IDevice* device, ApplicationContext* appContext);
    void Render(nvrhi::IFramebuffer* framebuffer, double deltaTime, const SimulationSnapshot& snapshot);
    void Shutdown();
private:
    std::shared_ptr<RegisteredFont> CreateFontFromFile(const char* fontFile, float fontSize);
    void ProcessInputEvents();

private:
    std::vector<std::shared_ptr<RegisteredFont>> m_fonts;
    std::unique_ptr<ImGui_NVRHI> m_ImGuiNvrhi;
    ApplicationContext* m_AppContext = nullptr;
};