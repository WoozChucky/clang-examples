#include "Renderer.h"

#include <algorithm>
#include <iostream>

#include <nvrhi/utils.h>

#include "backends/RendererBackendDX12.h"
#include "backends/RendererBackendVulkan.h"

#include "passes/PrimitiveRenderPass.h"
#include "passes/MeshRenderPass.h"

#include <tracy/Tracy.hpp>

#include <memory/AllocatorRegistry.h>

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
        nvrhi::TextureHandle missingMaterialTexture;
        nvrhi::SamplerHandle defaultSampler;
        CreateDefaultMaterialResources(missingMaterialTexture, defaultSampler);

        m_MeshSystem.Initialize(m_Device);
        m_MaterialSystem.Initialize(m_Device, missingMaterialTexture, defaultSampler);
    }

    if (m_Overlay && !m_Overlay->Init(m_Device, m_AppContext, &m_MeshSystem, &m_MaterialSystem, this)) {
        SM_ERROR("Failed to initialize renderer overlay");
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

    Engine::Registry().Register(&m_FrameAllocator);

    return true;
}

void Renderer::Shutdown(const uint32_t timeoutMs) {
    Engine::Registry().Unregister(&m_FrameAllocator);

    if (m_Overlay) {
        // reset() destroys the overlay, whose destructor performs teardown (e.g.
        // ~ImGuiRenderer calls Shutdown()). Do NOT also call Shutdown() explicitly:
        // ImGuiRenderer::Shutdown() unconditionally DestroyContext()s, so a double
        // call null-derefs GImGui. RAII via reset() = single teardown, matching the
        // pre-refactor behavior.
        m_Overlay.reset();
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

float Renderer::Render(double deltaTime, float red, float green, float blue, SimulationSnapshot& snapshot, const ECS* world) {

    if (!m_Backend || !m_Device || !m_CommandList) {
        SM_ERROR("Failed to render: renderer not fully initialized (backend=%p, device=%p, cmdlist=%p)",
                 static_cast<void *>(m_Backend), static_cast<void *>(m_Device.Get()), static_cast<void *>(m_CommandList.Get()));
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
                        pass->Render(m_CommandList, frameBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
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

            if (m_Overlay) m_Overlay->Render(frameBuffer, deltaTime, snapshot, world, secs);

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
                              const uint32_t* indices, uint32_t indexCount, SubMesh* subMeshes, uint32_t subMeshCount) {
    return m_MeshSystem.AddMesh(vertices, vertexCount, indices, indexCount, subMeshes, subMeshCount);
}

MaterialHandle Renderer::AddMaterial(const uint32_t* textureRgba8, uint32_t texWidth, uint32_t texHeight) {
    return m_MaterialSystem.AddMaterial(textureRgba8, texWidth, texHeight);
}

void Renderer::CreateDefaultMaterialResources(nvrhi::TextureHandle& outMissing,
                                              nvrhi::SamplerHandle& outSampler)
{
    constexpr uint32_t texSize = 256;
    constexpr uint32_t magenta = 0xFFFF00FFu;
    constexpr uint32_t black   = 0xFF000000u;

    nvrhi::TextureDesc td;
    td.debugName = "Renderer DefaultMissingTexture";
    td.width = texSize;
    td.height = texSize;
    td.depth = 1;
    td.arraySize = 1;
    td.mipLevels = 1;
    td.sampleCount = 1;
    td.dimension = nvrhi::TextureDimension::Texture2D;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.isShaderResource = true;
    outMissing = m_Device->createTexture(td);

    static uint32_t pixels[texSize * texSize];
    constexpr uint32_t checkerSize = 16;
    for (uint32_t y = 0; y < texSize; ++y) {
        for (uint32_t x = 0; x < texSize; ++x) {
            const bool checkerX = (x / checkerSize) % 2 == 0;
            const bool checkerY = (y / checkerSize) % 2 == 0;
            pixels[y * texSize + x] = (checkerX == checkerY) ? magenta : black;
        }
    }

    const auto cl = m_Device->createCommandList();
    cl->open();
    cl->beginTrackingTextureState(outMissing, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    cl->writeTexture(outMissing, 0, 0, pixels, texSize * sizeof(uint32_t));
    cl->setPermanentTextureState(outMissing, nvrhi::ResourceStates::ShaderResource);
    cl->commitBarriers();
    cl->close();
    m_Device->executeCommandList(cl);

    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true);
    sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    outSampler = m_Device->createSampler(sd);
}

void Renderer::TeardownForSwap()
{
    // ImGui: drop only the NVRHI backend + device-bound preview resources;
    // keep the ImGui context (dock layout, fonts loaded from disk).
    if (m_Overlay) {
        m_Overlay->OnDeviceLost();
    }

    m_GpuTimer.Cleanup();

    for (auto& pass : m_RenderPasses) {
        if (pass) pass->Shutdown();
    }
    m_RenderPasses.clear();

    // Release GPU resources but keep CPU caches + entry slots.
    m_MaterialSystem.DestroyGpuResources();
    m_MeshSystem.DestroyGpuResources();

    // Flush the deferred-release queue now, while the device is still alive,
    // so the editor's released handles don't leak when the device is dropped.
    if (m_Device)
    {
        m_Device->runGarbageCollection();
    }

    m_CommandList = nullptr;
    m_Device = nullptr;

    if (m_Backend) {
        m_Backend->Shutdown(SHUTDOWN_TIMEOUT);
        delete m_Backend;
        m_Backend = nullptr;
    }
}

bool Renderer::InitForSwap(RendererAPI newApi)
{
    switch (newApi) {
        case RendererAPI::DirectX12:
            m_Backend = new RendererBackendDX12(m_BackendSettings, m_Window);
            break;
        case RendererAPI::Vulkan:
            m_Backend = new RendererBackendVulkan(m_BackendSettings, m_Window);
            break;
        default:
            SM_ERROR("InitForSwap: unsupported API %d", static_cast<int>(newApi));
            return false;
    }

    if (!m_Backend->Init()) { SM_ERROR("InitForSwap: backend Init failed"); return false; }

    m_Device = m_Backend->CreateDevice();
    if (!m_Device) { SM_ERROR("InitForSwap: CreateDevice failed"); return false; }

    m_Backend->CreateSwapChain(m_BackendSettings.backBufferWidth, m_BackendSettings.backBufferHeight);

    m_CommandList = m_Device->createCommandList();
    if (!m_CommandList) { SM_ERROR("InitForSwap: createCommandList failed"); return false; }

    m_GpuTimer.Init(m_Device, 256);

    // Rebuild default material resources + replay caches.
    nvrhi::TextureHandle missingTex;
    nvrhi::SamplerHandle defaultSampler;
    CreateDefaultMaterialResources(missingTex, defaultSampler);

    m_MeshSystem.SetDevice(m_Device);
    m_MeshSystem.RecreateGpuResources();          // per-mesh failures are non-fatal
    m_MaterialSystem.SetDevice(m_Device);
    m_MaterialSystem.RecreateGpuResources(missingTex, defaultSampler);

    // ImGui NVRHI backend against the new device.
    if (m_Overlay && !m_Overlay->OnDeviceReset(m_Device)) {
        SM_ERROR("InitForSwap: overlay device reset failed");
        return false;
    }

    // Recreate render passes.
    auto primitivePass = std::make_unique<PrimitiveRenderPass>();
    if (!primitivePass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: PrimitivePass failed"); return false; }
    AddRenderPass(std::move(primitivePass));

    auto meshPass = std::make_unique<MeshRenderPass>();
    if (!meshPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: MeshPass failed"); return false; }
    AddRenderPass(std::move(meshPass));

    auto uiPass = std::make_unique<UiRenderPass>();
    if (!uiPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: UiPass failed"); return false; }
    AddRenderPass(std::move(uiPass));

    // Frame index reset so the first post-swap frame is treated like a warm-up
    // (Render() skips frame 0; see m_FrameIndex guard).
    m_FrameIndex = 0;

    SM_TRACE("InitForSwap: now running API %d", static_cast<int>(m_Backend->GetAPI()));
    return true;
}

bool Renderer::SwapBackend(RendererAPI newApi)
{
    if (m_Backend && m_Backend->GetAPI() == newApi) {
        SM_TRACE("SwapBackend: already running API %d; no-op", static_cast<int>(newApi));
        return true;
    }

    SM_TRACE("SwapBackend: %d -> %d begin",
             static_cast<int>(m_Backend ? m_Backend->GetAPI() : RendererAPI::Invalid),
             static_cast<int>(newApi));

    if (m_Device) {
        m_Device->waitForIdle();
    }

    TeardownForSwap();

    if (!InitForSwap(newApi)) {
        SM_ERROR("SwapBackend: InitForSwap failed (fatal)");
        return false;
    }

    SM_TRACE("SwapBackend: complete");
    return true;
}
