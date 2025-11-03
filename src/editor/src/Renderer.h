#pragma once

#include <nvrhi/nvrhi.h>
#include <GLFW/glfw3.h>

#include "RendererBackend.h"

struct GpuTimer
{
    std::vector<nvrhi::TimerQueryHandle> queries; // size = FramesInFlight (or >)
    uint32_t index = 0;                           // rotating index

    void Init(nvrhi::IDevice* device, uint32_t capacity)
    {
        queries.resize(capacity);
        for (auto& q : queries) q = device->createTimerQuery();
    }

    void Begin(nvrhi::ICommandList* cmd) const {
        cmd->beginTimerQuery(queries[index].Get());
    }

    void End(nvrhi::ICommandList* cmd) const {
        cmd->endTimerQuery(queries[index].Get());
    }

    // Call once per frame after submission; try to read a result from a previous frame
    // Returns true if a result was available and written to outSeconds
    bool TryRead(nvrhi::IDevice* device, float& outSeconds) const {
        // Probe an older query (e.g., previous frame index)
        const uint32_t readIdx = (index + 1) % static_cast<uint32_t>(queries.size());
        auto* q = queries[readIdx].Get();

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
        index = (index + 1) % static_cast<uint32_t>(queries.size());
    }

    void Cleanup() {
        queries.clear();
        index = 0;
    }
};

class Renderer {
public:
    explicit Renderer(GLFWwindow* window) : m_Window(window) {}
    ~Renderer() {
        Shutdown();
    }

    bool Init(RendererAPI api);
    void Shutdown(uint32_t timeoutMs = SHUTDOWN_TIMEOUT);
    float Render(double deltaTime, float red, float green, float blue);
    void Resize(uint32_t width, uint32_t height) const;
    void ToggleVSync();

private:
    constexpr static uint32_t   SHUTDOWN_TIMEOUT = 5000;

    nvrhi::DeviceHandle         m_Device = nullptr;
    nvrhi::CommandListHandle    m_CommandList = nullptr;

    GpuTimer                    m_GpuTimer {};

    GLFWwindow*                 m_Window;

    RendererBackend*            m_Backend = nullptr;
    RendererBackendSettings     m_BackendSettings{};
};
