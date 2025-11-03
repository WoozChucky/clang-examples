#include "Renderer.h"

#include <iostream>

#include <glm/vec4.hpp>
#include <nvrhi/utils.h>

#include "RendererBackendDX12.h"
#include "RendererBackendVulkan.h"

bool Renderer::Init(const RendererAPI api) {
    switch (api) {
        case RendererAPI::DirectX11:
            break;
        case RendererAPI::DirectX12:
            m_Backend = new RendererBackendDX12(m_BackendSettings, m_Window);
            break;
        case RendererAPI::Vulkan:
            m_Backend = new RendererBackendVulkan(m_BackendSettings, m_Window);
            break;
        default:
            SM_ERROR("Unsupported rendering API: %d", static_cast<int>(api));
            return false;
    }

    if (!m_Backend) {
        SM_ERROR("Failed to create RendererBackend for API: %d", static_cast<int>(api));
        return false;
    }

    if (!m_Backend->Init()) {
        delete m_Backend;
        m_Backend = nullptr;
        return false;
    }

    m_Device = m_Backend->CreateDevice();

    m_Backend->CreateSwapChain(m_BackendSettings.backBufferWidth, m_BackendSettings.backBufferHeight);

    m_CommandList = m_Backend->CreateCommandList();

    m_GpuTimer.Init(m_Device, 256);

    SM_TRACE("Renderer initialized with API: %d", static_cast<int>(m_Backend->GetAPI()));

    return true;
}

void Renderer::Shutdown(const uint32_t timeoutMs) {
    m_GpuTimer.Cleanup();

    if (m_CommandList) {
        m_CommandList = nullptr;
    }

    if (m_Device) {
        m_Device = nullptr;
    }

    if (m_Backend) {
        m_Backend->Shutdown(timeoutMs);
        delete m_Backend;
        m_Backend = nullptr;
    }
}

float Renderer::Render(double deltaTime, float red, float green, float blue) {

    if (!m_Backend || !m_Device) {
        SM_ERROR("Failed to render API: not initialized");
        return 0.0f;
    }

    float secs = 0;
    {
        // Read GPU timer from last frame
        if (m_GpuTimer.TryRead(m_Device, secs)) {
            secs = secs * 1000.0f;
        }
    }

    uint32_t* frameIndex = m_Backend->GetFrameIndexPtr();

    if (*frameIndex > 0) {
        if (m_Backend->BeginFrame()) {

            nvrhi::IFramebuffer* frameBuffer = m_Backend->GetFrameBuffer(-1);

            m_CommandList->open();
            m_GpuTimer.Begin(m_CommandList);

            static glm::vec4 ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            //nvrhi::Color(ClearColor.r, ClearColor.g, ClearColor.b, ClearColor.a);
            const auto clearColor = nvrhi::Color(red, green, blue, ClearColor.a);

            nvrhi::utils::ClearColorAttachment(m_CommandList, frameBuffer, 0, clearColor);

            m_GpuTimer.End(m_CommandList);
            m_CommandList->close();
            m_Device->executeCommandList(m_CommandList, nvrhi::CommandQueue::Graphics);

            m_GpuTimer.Advance();

            const bool presentSuccess = m_Backend->Present();
            if (!presentSuccess) {
                SM_ERROR("[Renderer] Present() failed");
                return 0.0f;
            }
        }
    }

    m_Device->runGarbageCollection();

    ++*frameIndex;

    return secs;
}

void Renderer::Resize(uint32_t width, uint32_t height) const {
    // TODO: nullptr pipelines/render passes
    if (m_Backend) {
        m_Backend->ResizeSwapChain(width, height);
    }
}

void Renderer::ToggleVSync() {
    m_BackendSettings.vsyncEnabled = !m_BackendSettings.vsyncEnabled;
}
