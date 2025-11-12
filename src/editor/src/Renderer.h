#pragma once

#include <nvrhi/nvrhi.h>
#include <GLFW/glfw3.h>
#include <vector>

#include "ApplicationContext.h"
#include "RendererBackend.h"
#include "IRenderPass.h"

#include "Camera.h"
#include "ImGuiRenderer.h"

struct GpuTimer
{
    std::vector<nvrhi::TimerQueryHandle> m_Queries; // size = FramesInFlight (or >)
    uint32_t m_Index = 0;                           // rotating index

    void Init(nvrhi::IDevice* device, uint32_t capacity)
    {
        m_Queries.resize(capacity);
        for (auto& q : m_Queries) q = device->createTimerQuery();
    }

    void Begin(nvrhi::ICommandList* cmd) const {
        cmd->beginTimerQuery(m_Queries[m_Index].Get());
    }

    void End(nvrhi::ICommandList* cmd) const {
        cmd->endTimerQuery(m_Queries[m_Index].Get());
    }

    // Call once per frame after submission; try to read a result from a previous frame
    // Returns true if a result was available and written to outSeconds
    bool TryRead(nvrhi::IDevice* device, float& outSeconds) const {
        // Probe an older query (e.g., previous frame index)
        const uint32_t readIdx = (m_Index + 1) % static_cast<uint32_t>(m_Queries.size());
        auto* q = m_Queries[readIdx].Get();

        if (device->pollTimerQuery(q))
        {
            outSeconds = device->getTimerQueryTime(q); // non‑blocking now
            device->resetTimerQuery(q);
            return true;
        }
        return false;
    }

    void Advance()
    {
        // Reset the just‑used query so it’s ready next time we come back around
        m_Index = (m_Index + 1) % static_cast<uint32_t>(m_Queries.size());
    }

    void Cleanup() {
        m_Queries.clear();
        m_Index = 0;
    }
};

class Renderer final {
public:
    explicit Renderer(GLFWwindow* window, ApplicationContext* appContext) : m_Window(window), m_AppContext(appContext) {
        m_BackendSettings.backBufferWidth = appContext->Settings.windowWidth;
        m_BackendSettings.backBufferHeight = appContext->Settings.windowHeight;
        m_BackendSettings.vsyncEnabled = appContext->Settings.vsyncEnabled;
    }
    ~Renderer() {
        Shutdown();
    }

    bool Init(RendererAPI api);
    void Shutdown(uint32_t timeoutMs = SHUTDOWN_TIMEOUT);

    float Render(double deltaTime, float red, float green, float blue, OrthographicCamera2D& uiCamera, PerspectiveCamera3D& gameCamera, double targetTPS, double actualTPS);
    void Resize(uint32_t width, uint32_t height);
    void ToggleVSync();

    // Shader creation (wraps backend, keeps backend isolated)
    nvrhi::ShaderHandle CreateShader(
        nvrhi::ShaderType shaderType,
        const char* content,
        size_t contentSize,
        const char* entryPoint,
        const char* targetName);

    // Render pass management
    void AddRenderPass(std::unique_ptr<IRenderPass> pass);
    void RemoveRenderPass(IRenderPass* pass);

private:

    constexpr static uint32_t   SHUTDOWN_TIMEOUT = 5000;

    std::unique_ptr<ImGuiRenderer> m_ImGuiRenderer;

    nvrhi::DeviceHandle         m_Device = nullptr;
    nvrhi::CommandListHandle    m_CommandList = nullptr;

    GpuTimer                    m_GpuTimer {};

    GLFWwindow*                 m_Window;
    ApplicationContext*         m_AppContext;

    RendererBackend*            m_Backend = nullptr;
    RendererBackendSettings     m_BackendSettings{};

    std::vector<std::unique_ptr<IRenderPass>>   m_RenderPasses;
};
