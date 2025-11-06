#pragma once

#include <nvrhi/nvrhi.h>
#include <GLFW/glfw3.h>

#include "RendererBackend.h"
#include "glm/mat4x4.hpp"
#include "glm/vec2.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"

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

struct PrimitivePass {
    nvrhi::BufferHandle m_VertexBuffer;      // plane VB
    nvrhi::BufferHandle m_IndexBuffer;       // plane IB
    nvrhi::BufferHandle m_PerFrameConstantBuffer; // view/proj matrices
    nvrhi::BufferHandle m_PerDrawCB;         // grid params
    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSet;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    uint32_t m_IndexCount = 0;
};

struct PerFrameCBData {
    glm::mat4 Model;
    glm::mat4 VP;
    glm::vec4 CameraPos;
};
static_assert(sizeof(PerFrameCBData) % 16 == 0);

struct PrimPerDrawCB {
    glm::vec4 GridColor;
    glm::vec4 AxisXColor;
    glm::vec4 AxisZColor;
    glm::vec2 GridParams;
    glm::vec2 FadeParams;
};
static_assert(sizeof(PrimPerDrawCB) % 16 == 0);

enum class PrimitiveType : uint32_t { Plane = 0, Cube = 1, Sphere = 2, Cone = 3, Line = 4 };

struct PrimitiveInstance
{
    PrimitiveType type;
    glm::mat4     transform; // world transform
    glm::vec4     color;     // base color (not all primitives use it)
    glm::vec4     params;    // optional parameters per primitive
};

class Renderer final {
public:
    explicit Renderer(GLFWwindow* window) : m_Window(window) {}
    ~Renderer() {
        Shutdown();
    }

    bool Init(RendererAPI api);
    void Shutdown(uint32_t timeoutMs = SHUTDOWN_TIMEOUT);



    float Render(double deltaTime, float red, float green, float blue, OrthographicCamera2D& uiCamera, PerspectiveCamera3D& gameCamera);
    void Resize(uint32_t width, uint32_t height);
    void ToggleVSync();

private:
    void PreparePrimitivePass();
    void RenderSomethingTemporarily(nvrhi::IFramebuffer* frameBuffer, PerspectiveCamera3D& camera);

    constexpr static uint32_t   SHUTDOWN_TIMEOUT = 5000;

    std::unique_ptr<ImGuiRenderer> m_ImGuiRenderer;

    nvrhi::DeviceHandle         m_Device = nullptr;
    nvrhi::CommandListHandle    m_CommandList = nullptr;

    GpuTimer                    m_GpuTimer {};

    GLFWwindow*                 m_Window;

    RendererBackend*            m_Backend = nullptr;
    RendererBackendSettings     m_BackendSettings{};

    PrimitivePass               m_PrimitivePass{};
};
