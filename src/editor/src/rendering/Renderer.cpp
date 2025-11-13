#include "Renderer.h"

#include <algorithm>
#include <iostream>

#include <nvrhi/utils.h>

#include "backends/RendererBackendDX12.h"
#include "backends/RendererBackendVulkan.h"

#include "passes/PrimitiveRenderPass.h"

#include <tracy/Tracy.hpp>

#include "passes/UiRenderPass.h"

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
    if (!m_Device) {
        SM_ERROR("Failed to create Device");
        delete m_Backend;
        m_Backend = nullptr;
        return false;
    }

    m_Backend->CreateSwapChain(m_BackendSettings.backBufferWidth, m_BackendSettings.backBufferHeight);

    m_CommandList = m_Backend->CreateCommandList();
    if (!m_CommandList) {
        SM_ERROR("Failed to create CommandList");
        delete m_Backend;
        m_Backend = nullptr;
        m_Device = nullptr;
        return false;
    }

    m_GpuTimer.Init(m_Device, 256);

    m_ImGuiRenderer = std::make_unique<ImGuiRenderer>();
    if (!m_ImGuiRenderer->Init(m_Window, m_Device, m_AppContext)) {
        SM_ERROR("Failed to initialize ImGuiRenderer");
        return false;
    }

    SM_TRACE("Renderer initialized with API: %d", static_cast<int>(m_Backend->GetAPI()));

    // Initialize and add render passes
    auto primitivePass = std::make_unique<PrimitiveRenderPass>();
    if (!primitivePass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize PrimitiveRenderPass");
        return false;
    }
    AddRenderPass(std::move(primitivePass));

    auto uiPass = std::make_unique<UiRenderPass>();
    if (!uiPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize UiRenderPass");
        return false;
    }
    AddRenderPass(std::move(uiPass));

    return true;
}

void Renderer::Shutdown(const uint32_t timeoutMs) {
    if (m_ImGuiRenderer) {
        m_ImGuiRenderer.reset();
        m_ImGuiRenderer = nullptr;
    }

    m_GpuTimer.Cleanup();

    // Cleanup all render passes
    for (auto& pass : m_RenderPasses) {
        if (pass) {
            pass->Shutdown();
        }
    }
    m_RenderPasses.clear();

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

float Renderer::Render(double deltaTime, float red, float green, float blue, SimulationSnapshot& snapshot) {

if (!m_Backend || !m_Device || !m_CommandList) {
    SM_ERROR("Failed to render: renderer not fully initialized (backend=%p, device=%p, cmdlist=%p)",
             (void*)m_Backend, (void*)m_Device.Get(), (void*)m_CommandList.Get());
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

            {
                ZoneScopedN("BeginRecording");
                m_CommandList->open();
                m_GpuTimer.Begin(m_CommandList);
            }


            {
                ZoneScopedN("RenderPasses");
                static glm::vec4 ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                const auto clearColor = nvrhi::Color(red, green, blue, ClearColor.a);

                nvrhi::utils::ClearColorAttachment(m_CommandList, frameBuffer, 0, clearColor);

                // Render all passes
                for (auto& pass : m_RenderPasses) {
                    if (pass) {
                        pass->Render(m_CommandList, frameBuffer, snapshot, deltaTime);
                    }
                }
            }

            {
                ZoneScopedN("SubmitRecording");
                m_GpuTimer.End(m_CommandList);
                m_CommandList->close();

                m_Device->executeCommandList(m_CommandList, nvrhi::CommandQueue::Graphics);

                m_GpuTimer.Advance();
            }

            m_ImGuiRenderer->Render(frameBuffer, deltaTime, snapshot);

            {
                ZoneScopedN("Present");
                const bool presentSuccess = m_Backend->Present();
                if (!presentSuccess) {
                    SM_ERROR("[Renderer] Present() failed");
                    return 0.0f;
                }
            }
        }
    }

    m_Device->runGarbageCollection();

    ++*frameIndex;

    return secs;
}

void Renderer::Resize(const uint32_t width, const uint32_t height) {
    // Notify all render passes about the resize
    for (auto& pass : m_RenderPasses) {
        if (pass) {
            pass->OnResize(width, height);
        }
    }
    
    if (m_Backend) {
        m_Backend->ResizeSwapChain(width, height);
    }
}

void Renderer::ToggleVSync() {
    m_BackendSettings.vsyncEnabled = !m_BackendSettings.vsyncEnabled;
    if (m_Backend) {
        m_Backend->SetVSync(m_BackendSettings.vsyncEnabled);
    }
}

nvrhi::ShaderHandle Renderer::CreateShader(
    nvrhi::ShaderType shaderType,
    const char* content,
    size_t contentSize,
    const char* entryPoint,
    const char* targetName)
{
    if (!m_Backend) {
        SM_ERROR("Renderer::CreateShader - Backend not initialized");
        return nullptr;
    }
    return m_Backend->CreateShaderFromMemory(shaderType, content, contentSize, entryPoint, targetName);
}

void Renderer::AddRenderPass(std::unique_ptr<IRenderPass> pass) {
    if (pass) {
        m_RenderPasses.push_back(std::move(pass));
    }
}

void Renderer::RemoveRenderPass(IRenderPass* pass) {
    if (!pass) return;
    
    auto it = std::find_if(m_RenderPasses.begin(), m_RenderPasses.end(),
        [pass](const std::unique_ptr<IRenderPass>& ptr) { return ptr.get() == pass; });
    if (it != m_RenderPasses.end()) {
        m_RenderPasses.erase(it);
    }
}
