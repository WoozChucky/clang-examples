#include "Renderer.h"

#include <algorithm>
#include <iostream>

#include <nvrhi/utils.h>

#include "backends/RendererBackendDX12.h"
#include "backends/RendererBackendVulkan.h"

#include "passes/PrimitiveRenderPass.h"
#include "passes/MeshRenderPass.h"

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

    m_CommandList = m_Device->createCommandList();
    if (!m_CommandList) {
        SM_ERROR("Failed to create CommandList");
        delete m_Backend;
        m_Backend = nullptr;
        m_Device = nullptr;
        return false;
    }

    m_GpuTimer.Init(m_Device, 256);

    // Initialize resource systems with default resources
    {
        // Create default magenta checkerboard texture (16x16 RGBA8) for missing textures
        nvrhi::TextureHandle defaultWhite;
        {
            constexpr uint32_t texSize = 16;
            constexpr uint32_t magenta = 0xFFFF00FFu; // RGBA8: magenta (R=255, G=0, B=255, A=255)
            constexpr uint32_t black = 0xFF000000u;   // RGBA8: black (R=0, G=0, B=0, A=255)

            nvrhi::TextureDesc td;
            td.debugName = "Renderer DefaultMissingTexture";
            td.width = texSize; td.height = texSize; td.depth = 1;
            td.arraySize = 1; td.mipLevels = 1;
            td.sampleCount = 1;
            td.dimension = nvrhi::TextureDimension::Texture2D;
            td.format = nvrhi::Format::RGBA8_UNORM;
            td.initialState = nvrhi::ResourceStates::CopyDest;
            td.isShaderResource = true;
            defaultWhite = m_Device->createTexture(td);

            // Generate magenta checkerboard pattern (2x2 checker cells = 8x8 pixels per cell)
            uint32_t pixels[texSize * texSize];
            constexpr uint32_t checkerSize = 8; // Size of each checker square in pixels
            for (uint32_t y = 0; y < texSize; ++y) {
                for (uint32_t x = 0; x < texSize; ++x) {
                    const bool checkerX = (x / checkerSize) % 2 == 0;
                    const bool checkerY = (y / checkerSize) % 2 == 0;
                    const bool isMagenta = checkerX == checkerY; // XOR pattern
                    pixels[y * texSize + x] = isMagenta ? magenta : black;
                }
            }

            const auto cl = m_Device->createCommandList();
            cl->open();
            cl->beginTrackingTextureState(defaultWhite, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
            cl->writeTexture(defaultWhite, 0, 0, pixels, texSize * sizeof(uint32_t));
            cl->setPermanentTextureState(defaultWhite, nvrhi::ResourceStates::ShaderResource);
            cl->close();
            m_Device->executeCommandList(cl);
        }

        // Create default sampler
        nvrhi::SamplerHandle defaultSampler;
        {
            nvrhi::SamplerDesc sd;
            sd.setAllFilters(true);
            sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
            defaultSampler = m_Device->createSampler(sd);
        }

        // Initialize systems
        m_MeshSystem.Initialize(m_Device);
        m_MaterialSystem.Initialize(m_Device, defaultWhite, defaultSampler);
    }

    m_ImGuiRenderer = std::make_unique<ImGuiRenderer>();
    if (!m_ImGuiRenderer->Init(m_Device, m_AppContext, &m_MeshSystem, &m_MaterialSystem)) {
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

    auto meshPass = std::make_unique<MeshRenderPass>();
    if (!meshPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize MeshRenderPass");
        return false;
    }
    AddRenderPass(std::move(meshPass));

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

    // Cleanup resource systems
    m_MaterialSystem.Shutdown();
    m_MeshSystem.Shutdown();

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

    if (m_FrameIndex > 0) {
        if (m_Backend->BeginFrame()) {

            nvrhi::IFramebuffer* frameBuffer = m_Backend->GetCurrentFrameBuffer();

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
                    ZoneScopedN("RenderPass Rec N");
                    if (pass) {
                        pass->Render(m_CommandList, frameBuffer, snapshot, deltaTime, &m_FrameAllocator);
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

    // Reset frame allocator for next frame
    m_FrameAllocator.Reset();

    m_Device->runGarbageCollection();

    ++m_FrameIndex;

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

    auto it = std::ranges::find_if(m_RenderPasses,
                                   [pass](const std::unique_ptr<IRenderPass>& ptr) { return ptr.get() == pass; });
    if (it != m_RenderPasses.end()) {
        m_RenderPasses.erase(it);
    }
}

MeshHandle Renderer::AddMesh(const MeshVertex* vertices, uint32_t vertexCount,
                              const uint32_t* indices, uint32_t indexCount) {
    return m_MeshSystem.AddMesh(vertices, vertexCount, indices, indexCount);
}

MaterialHandle Renderer::AddMaterial(const uint32_t* textureRgba8, uint32_t texWidth, uint32_t texHeight) {
    return m_MaterialSystem.AddMaterial(textureRgba8, texWidth, texHeight);
}
