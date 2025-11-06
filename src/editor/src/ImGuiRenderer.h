#pragma once

#include <memory>
#include <nvrhi/nvrhi.h>
#include <GLFW/glfw3.h>

#include "imgui_nvrhi.h"

class RegisteredFont;

class ImGuiRenderer final {
public:
    ImGuiRenderer() = default;
    ~ImGuiRenderer() {
        Shutdown();
    }

    bool Init(GLFWwindow* window, nvrhi::IDevice* device);
    void Render(nvrhi::IFramebuffer* framebuffer, double deltaTime);
    void Shutdown();
private:
    std::shared_ptr<RegisteredFont> CreateFontFromFile(const char* fontFile, float fontSize);

private:
    std::vector<std::shared_ptr<RegisteredFont>> m_fonts;
    std::unique_ptr<ImGui_NVRHI> m_ImGuiNvrhi;
};