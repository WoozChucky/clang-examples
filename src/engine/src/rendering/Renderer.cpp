#include "Renderer.h"
#include "ECS.h" // world->Each, LightningComponent

#include <algorithm>
#include <iostream>

#include <nvrhi/utils.h>

#include "backends/RendererBackendDX12.h"
#include "backends/RendererBackendVulkan.h"

#include "passes/OutlineRenderPass.h"
#include "passes/DebugRenderPass.h"
#include "passes/GBufferFillPass.h"
#include "passes/LightingRenderPass.h"
#include "passes/SsaoRenderPass.h"
#include "passes/SkyRenderPass.h"
#include "passes/ShadowDepthPass.h"
#include "passes/SkinningComputePass.h"
#include "PaletteFrame.h"

#include <tracy/Tracy.hpp>

#include <memory/AllocatorRegistry.h>

#include "passes/UiRenderPass.h"

#include "RenderStats.h"

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

    CreateShadowResources();

    if (m_Overlay && !m_Overlay->Init(m_Device, m_AppContext, &m_MeshSystem, &m_MaterialSystem, this)) {
        SM_ERROR("Failed to initialize renderer overlay");
        return false;
    }

    SM_TRACE("Renderer initialized with API: %d", static_cast<int>(m_Backend->GetAPI()));

    // Initialize and add render passes (deferred order):
    // Shadow -> GBufferFill -> Lighting -> Sky -> Outline -> Debug -> UI.
    auto shadowPass = std::make_unique<ShadowDepthPass>();
    if (!shadowPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize ShadowDepthPass");
        return false;
    }
    AddRenderPass(std::move(shadowPass));

    auto gbufferPass = std::make_unique<GBufferFillPass>();
    if (!gbufferPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize GBufferFillPass");
        return false;
    }
    AddRenderPass(std::move(gbufferPass));

    auto ssaoPass = std::make_unique<SsaoRenderPass>();
    if (!ssaoPass->Initialize(m_Device, this)) { SM_ERROR("Failed to initialize SsaoRenderPass"); return false; }
    AddRenderPass(std::move(ssaoPass));

    auto lightingPass = std::make_unique<LightingRenderPass>();
    if (!lightingPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize LightingRenderPass");
        return false;
    }
    AddRenderPass(std::move(lightingPass));

    auto skyPass = std::make_unique<SkyRenderPass>();
    if (!skyPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize SkyRenderPass");
        return false;
    }
    AddRenderPass(std::move(skyPass));

    // OutlineRenderPass intentionally disabled (hardcoded). Code kept for easy re-enable.
#if 0
    auto outlinePass = std::make_unique<OutlineRenderPass>();
    if (!outlinePass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize OutlineRenderPass");
        return false;
    }
    AddRenderPass(std::move(outlinePass));
#endif

    auto debugPass = std::make_unique<DebugRenderPass>();
    if (!debugPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize DebugRenderPass");
        return false;
    }
    AddRenderPass(std::move(debugPass));

    auto uiPass = std::make_unique<UiRenderPass>();
    if (!uiPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize UiRenderPass");
        return false;
    }
    AddRenderPass(std::move(uiPass));

    // FXAA resolve pass: Renderer-owned, not part of m_RenderPasses.
    m_FxaaPass = std::make_unique<FxaaRenderPass>();
    if (!m_FxaaPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize FxaaRenderPass");
        return false;
    }

    m_SmaaPass = std::make_unique<SmaaRenderPass>();
    if (!m_SmaaPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize SmaaRenderPass");
        m_SmaaPass.reset(); // SMAA unavailable; AA switch falls back with a one-shot warn
    }

    // First compute pass: per-frame GPU skinning. Renderer-owned; Execute()d before the pass loop.
    m_SkinningPass = std::make_unique<SkinningComputePass>();
    if (!m_SkinningPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize SkinningComputePass");
        return false;
    }

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
    if (m_FxaaPass) { m_FxaaPass->Shutdown(); m_FxaaPass.reset(); }
    if (m_SmaaPass) { m_SmaaPass->Shutdown(); m_SmaaPass.reset(); }
    if (m_SkinningPass) { m_SkinningPass->DestroyGpuResources(); m_SkinningPass.reset(); }

    // Cleanup resource systems
    m_MaterialSystem.Shutdown();
    m_MeshSystem.Shutdown();

    // Release shadow GPU resources (device still alive here).
    m_ShadowDepth = nullptr;
    m_ShadowFb = nullptr;
    m_ShadowSampler = nullptr;
    ReleaseGBuffer();
    ReleaseSceneColor();
    m_SsaoRaw = nullptr; m_SsaoBlur = nullptr;
    m_SsaoW = m_SsaoH = 0;

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

            // Editor renders the scene into an overlay-provided offscreen target; runtime (no
            // overlay, or overlay returns null) renders straight into the swapchain backbuffer.
            nvrhi::IFramebuffer* sceneBuffer = m_Overlay ? m_Overlay->GetSceneFramebuffer(frameBuffer) : nullptr;
            if (!sceneBuffer) sceneBuffer = frameBuffer;

            // Keep the G-buffer sized to the scene target + sharing its depth.
            {
                const auto& fbinfo = sceneBuffer->getFramebufferInfo();
                EnsureGBuffer(fbinfo.width, fbinfo.height,
                              sceneBuffer->getDesc().depthAttachment.texture);
                EnsureSsao(fbinfo.width, fbinfo.height);
            }

            {
                ZoneScopedN("RenderPasses");

                // Resolve sun-driven fog once per frame. The same color drives the
                // scene clear ("sky") and the geometry fog in LightingRenderPass, so the
                // horizon has no seam. elevation defaults to night if no sun exists.
                glm::vec3 sunDir(0.0f, 1.0f, 0.0f); // points up = below horizon = night default
                if (world) {
                    // Track the tagged sun specifically (SunMarker), not just any directional light.
                    world->Each<SunMarker, LightningComponent>(
                        [&](EntityId, const SunMarker&, const LightningComponent& l) {
                            if (l.Type == LightningType::Directional) sunDir = glm::vec3(l.Direction);
                        });
                }
                FogComponent fogComp{};
                if (world) {
                    if (const auto* f = world->GetSingleton<FogComponent>()) fogComp = *f;
                }
                // Always resolve fog: LightingRenderPass reads m_FrameFog regardless of Enabled.
                m_FrameFog = ComputeFog(sunDir, fogComp);

                // AA path: world passes render into an offscreen scene-color SRV, which the
                // selected AA pass (FXAA or SMAA) then resolves into sceneBuffer. UI draws on
                // top, un-AA'd. When disabled, worldTarget == sceneBuffer (today's path).
                const AAMode aaMode = GetAntiAliasingSettings().Mode;
                bool useFxaa = (aaMode == AAMode::FXAA) && m_FxaaPass != nullptr;
                bool useSmaa = (aaMode == AAMode::SMAA) && m_SmaaPass != nullptr;
                if (aaMode == AAMode::SMAA && m_SmaaPass == nullptr) {
                    static bool s_warnedSmaa = false;
                    if (!s_warnedSmaa) { SM_WARN("SMAA selected but pass unavailable; rendering without AA"); s_warnedSmaa = true; }
                }
                bool offscreen = useFxaa || useSmaa; // both resolve from an offscreen scene-color SRV
                nvrhi::IFramebuffer* worldTarget = sceneBuffer;
                if (offscreen) {
                    const auto& sfbi = sceneBuffer->getFramebufferInfo();
                    nvrhi::ITexture* sharedDepth = sceneBuffer->getDesc().depthAttachment.texture;
                    EnsureSceneColor(sfbi.width, sfbi.height, sharedDepth, sfbi.colorFormats[0]);
                    if (m_SceneColor.Fb) {
                        worldTarget = m_SceneColor.Fb;
                    } else {
                        // Degradation path (e.g. scene-color allocation failed): log once,
                        // then fall back to direct rendering so the frame still presents.
                        static bool s_warnedFxaaAlloc = false;
                        if (!s_warnedFxaaAlloc) {
                            SM_WARN("AA enabled but scene-color target unavailable; falling back to direct rendering (no AA)");
                            s_warnedFxaaAlloc = true;
                        }
                        offscreen = useFxaa = useSmaa = false;
                    }
                }

                const auto sceneClear = fogComp.Enabled
                    ? nvrhi::Color(m_FrameFog.Color.r, m_FrameFog.Color.g, m_FrameFog.Color.b, 1.0f)
                    : nvrhi::Color(red, green, blue, 1.0f);
                nvrhi::utils::ClearColorAttachment(m_CommandList, worldTarget, 0, sceneClear);
                if (sceneBuffer != frameBuffer) {
                    // Offscreen scene: clear the swapchain (dark) so the present surface is clean
                    // behind the ImGui dockspace. Depth of the scene target is cleared in GBufferFillPass.
                    nvrhi::utils::ClearColorAttachment(m_CommandList, frameBuffer, 0, nvrhi::Color(0.1f, 0.1f, 0.1f, 1.0f));
                }

                // Resolve the camera the world passes use this frame. Editor override (set by the
                // ImGui overlay last frame) wins when active; otherwise the game's WorldCameraComponent
                // from the snapshot. Runtime never sets EditorCameraActive -> always the game camera.
                {
                    CameraView active{}; // identity V/P, zero pos: matches the passes' old null fallback
                    if (m_AppContext && m_AppContext->EditorCameraActive.load(std::memory_order_relaxed)) {
                        active = m_AppContext->EditorCamera.load();
                    } else if (world) {
                        if (const auto* cam = world->GetSingleton<WorldCameraComponent>())
                            active = { cam->View, cam->Projection, cam->Position };
                    }
                    m_ActiveCamera = active;
                }

                // Run the skinning compute pass first: it skins each skinned entity into a per-frame
                // vertex buffer that the shadow + g-buffer passes then read via their static pipelines
                // (baseVertex per entity). Also uploads the palette consumed by the skin shader.
                if (m_SkinningPass) {
                    std::shared_ptr<const PaletteFrame> palette =
                        m_AppContext ? m_AppContext->LatestPaletteFrame.load(std::memory_order_acquire) : nullptr;
                    m_SkinningPass->Execute(m_CommandList, world, palette.get());
                }

                // World passes -> world target (offscreen scene color when AA on, else sceneBuffer).
                for (auto& pass : m_RenderPasses) {
                    ZoneScopedN("RenderPass Rec N");
                    if (pass && pass->Stage() == IRenderPass::RenderStage::World) {
                        pass->Render(m_CommandList, worldTarget, snapshot, world, deltaTime, &m_FrameAllocator);
                    }
                }

                // AA resolve: scene-color SRV -> sceneBuffer.
                if (useFxaa) {
                    m_FxaaPass->Render(m_CommandList, sceneBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
                } else if (useSmaa) {
                    m_SmaaPass->Render(m_CommandList, sceneBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
                }

                // Overlay passes (UI) on top of sceneBuffer, after the resolve.
                for (auto& pass : m_RenderPasses) {
                    if (pass && pass->Stage() == IRenderPass::RenderStage::Overlay) {
                        pass->Render(m_CommandList, sceneBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
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

    if (m_FxaaPass) {
        m_FxaaPass->OnResize(width, height);
    }
    if (m_SmaaPass) { m_SmaaPass->OnResize(width, height); }

    // Force the SSAO targets to rebuild at the new size (they are size-guarded by EnsureSsao).
    m_SsaoW = m_SsaoH = 0;
    m_SsaoRaw = nullptr;
    m_SsaoBlur = nullptr;

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

MeshHandle Renderer::AddMesh(std::string key, const MeshVertex* vertices, uint32_t vertexCount,
                              const uint32_t* indices, uint32_t indexCount, SubMesh* subMeshes, uint32_t subMeshCount,
                              const SkinnedVertex* boneData) {
    return m_MeshSystem.AddMesh(std::move(key), vertices, vertexCount, indices, indexCount, subMeshes, subMeshCount, boneData);
}

MaterialHandle Renderer::AddMaterial(std::string key, const uint32_t* textureRgba8, uint32_t texWidth, uint32_t texHeight) {
    return m_MaterialSystem.AddMaterial(std::move(key), textureRgba8, texWidth, texHeight);
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

void Renderer::CreateShadowResources()
{
    // D32 depth map: render target (depth) for the shadow pass + shader resource so the mesh
    // pass can sample it. keepInitialState lets NVRHI auto-transition between DepthWrite (shadow
    // pass) and ShaderResource (mesh pass) across command lists, like SceneViewport's targets.
    nvrhi::TextureDesc td;
    td.width  = kShadowMapSize;
    td.height = kShadowMapSize;
    td.format = nvrhi::Format::D32;
    td.dimension = nvrhi::TextureDimension::Texture2D;
    td.isRenderTarget = true;
    td.isShaderResource = true;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = "ShadowDepthMap";
    m_ShadowDepth = m_Device->createTexture(td);

    m_ShadowFb = m_Device->createFramebuffer(
        nvrhi::FramebufferDesc().setDepthAttachment(m_ShadowDepth));

    // Comparison sampler for hardware PCF: the reduction type drives the comparison; the actual
    // compare op is supplied in HLSL via SampleCmp (Task 4). NVRHI's SamplerDesc on this version
    // has no comparisonFunc field, so the func below is documentary intent only.
    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true);
    sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    sd.reductionType = nvrhi::SamplerReductionType::Comparison;
    m_ShadowSampler = m_Device->createSampler(sd);

    if (!m_ShadowDepth || !m_ShadowFb || !m_ShadowSampler) {
        SM_WARN("Renderer: failed to create shadow GPU resources");
    }
}

void Renderer::ReleaseGBuffer()
{
    m_GBuffer = GBuffer{};
}

void Renderer::EnsureGBuffer(uint32_t width, uint32_t height, nvrhi::ITexture* sharedDepth)
{
    if (!sharedDepth || width == 0 || height == 0) return;

    // Recreate the color targets only when the size changes; this also invalidates
    // every cached framebuffer (they reference the old color textures).
    if (!m_GBuffer.Albedo || m_GBuffer.Width != width || m_GBuffer.Height != height)
    {
        // Assumes a non-MSAA scene target (swapchain is 1x); G-buffer RTs are single-sampled.
        auto makeRT = [&](nvrhi::Format fmt, const char* name) {
            nvrhi::TextureDesc td;
            td.width = width; td.height = height;
            td.format = fmt;
            td.dimension = nvrhi::TextureDimension::Texture2D;
            td.isRenderTarget = true;
            td.isShaderResource = true;
            td.initialState = nvrhi::ResourceStates::ShaderResource;
            td.keepInitialState = true;
            td.debugName = name;
            td.clearValue = nvrhi::Color(0.f);
            td.useClearValue = true;
            return m_Device->createTexture(td);
        };
        m_GBuffer.Albedo   = makeRT(nvrhi::Format::RGBA8_UNORM,  "GBuffer.Albedo");
        m_GBuffer.Normal   = makeRT(nvrhi::Format::RGBA16_FLOAT, "GBuffer.Normal");
        m_GBuffer.WorldPos = makeRT(nvrhi::Format::RGBA16_FLOAT, "GBuffer.WorldPos");
        m_GBuffer.FbCache.clear();
        m_GBuffer.Fb = nullptr;
        m_GBuffer.Width = width;
        m_GBuffer.Height = height;
    }

    // Reuse a cached framebuffer for this depth texture, or build one and cache it.
    for (auto& [depth, fb] : m_GBuffer.FbCache) {
        if (depth == sharedDepth) { m_GBuffer.Fb = fb; return; }
    }
    nvrhi::FramebufferHandle fb = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
        .addColorAttachment(m_GBuffer.Albedo)
        .addColorAttachment(m_GBuffer.Normal)
        .addColorAttachment(m_GBuffer.WorldPos)
        .setDepthAttachment(sharedDepth));
    m_GBuffer.FbCache.emplace_back(sharedDepth, fb);
    m_GBuffer.Fb = fb;
}

void Renderer::EnsureSsao(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    if (m_SsaoRaw && m_SsaoW == width && m_SsaoH == height) return;
    m_SsaoW = width; m_SsaoH = height;
    auto mk = [&](const char* name) {
        nvrhi::TextureDesc td; td.width = width; td.height = height; td.format = nvrhi::Format::R8_UNORM;
        td.dimension = nvrhi::TextureDimension::Texture2D; td.isShaderResource = true;
        td.isUAV = true;                   // compute-only: written via RWTexture2D UAV, read as SRV
        td.debugName = name; td.initialState = nvrhi::ResourceStates::ShaderResource; td.keepInitialState = true;
        td.clearValue = nvrhi::Color(1.f); td.useClearValue = true;
        return m_Device->createTexture(td);
    };
    m_SsaoRaw  = mk("SSAO.Raw");
    m_SsaoBlur = mk("SSAO.Blur");
}

void Renderer::ReleaseSceneColor()
{
    m_SceneColor = SceneColorTarget{};
}

void Renderer::EnsureSceneColor(uint32_t width, uint32_t height,
                                nvrhi::ITexture* sharedDepth, nvrhi::Format colorFormat)
{
    if (!sharedDepth || width == 0 || height == 0 || colorFormat == nvrhi::Format::UNKNOWN) return;

    // Recreate the color target only on size/format change; this invalidates every
    // cached framebuffer (they reference the old color texture).
    if (!m_SceneColor.Color || m_SceneColor.Width != width ||
        m_SceneColor.Height != height || m_SceneColor.Format != colorFormat)
    {
        nvrhi::TextureDesc td;
        td.width = width; td.height = height;
        td.format = colorFormat;
        td.dimension = nvrhi::TextureDimension::Texture2D;
        td.isRenderTarget = true;
        td.isShaderResource = true;
        td.initialState = nvrhi::ResourceStates::ShaderResource;
        td.keepInitialState = true;
        td.debugName = "SceneColor";
        td.clearValue = nvrhi::Color(0.f);
        td.useClearValue = true;
        m_SceneColor.Color = m_Device->createTexture(td);
        m_SceneColor.FbCache.clear();
        m_SceneColor.Fb = nullptr;
        m_SceneColor.Width = width;
        m_SceneColor.Height = height;
        m_SceneColor.Format = colorFormat;
    }

    // Reuse a cached framebuffer for this depth texture, or build + cache one.
    for (auto& [depth, fb] : m_SceneColor.FbCache) {
        if (depth == sharedDepth) { m_SceneColor.Fb = fb; return; }
    }
    nvrhi::FramebufferHandle fb = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
        .addColorAttachment(m_SceneColor.Color)
        .setDepthAttachment(sharedDepth));
    m_SceneColor.FbCache.emplace_back(sharedDepth, fb);
    m_SceneColor.Fb = fb;
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
    if (m_FxaaPass) { m_FxaaPass->Shutdown(); m_FxaaPass.reset(); }
    if (m_SmaaPass) { m_SmaaPass->Shutdown(); m_SmaaPass.reset(); }
    if (m_SkinningPass) { m_SkinningPass->DestroyGpuResources(); m_SkinningPass.reset(); }

    // Release GPU resources but keep CPU caches + entry slots.
    m_MaterialSystem.DestroyGpuResources();
    m_MeshSystem.DestroyGpuResources();

    // Drop shadow GPU resources; recreated by InitForSwap against the new device.
    m_ShadowDepth = nullptr;
    m_ShadowFb = nullptr;
    m_ShadowSampler = nullptr;
    ReleaseGBuffer();
    ReleaseSceneColor();
    m_SsaoRaw = nullptr; m_SsaoBlur = nullptr;
    m_SsaoW = m_SsaoH = 0;

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

    CreateShadowResources();

    // ImGui NVRHI backend against the new device.
    if (m_Overlay && !m_Overlay->OnDeviceReset(m_Device)) {
        SM_ERROR("InitForSwap: overlay device reset failed");
        return false;
    }

    // Recreate render passes (deferred order):
    // Shadow -> GBufferFill -> Lighting -> Sky -> Outline -> Debug -> UI.
    auto shadowPass = std::make_unique<ShadowDepthPass>();
    if (!shadowPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: ShadowPass failed"); return false; }
    AddRenderPass(std::move(shadowPass));

    auto gbufferPass = std::make_unique<GBufferFillPass>();
    if (!gbufferPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: GBufferFillPass failed"); return false; }
    AddRenderPass(std::move(gbufferPass));

    auto ssaoPass = std::make_unique<SsaoRenderPass>();
    if (!ssaoPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: SsaoRenderPass failed"); return false; }
    AddRenderPass(std::move(ssaoPass));

    auto lightingPass = std::make_unique<LightingRenderPass>();
    if (!lightingPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: LightingPass failed"); return false; }
    AddRenderPass(std::move(lightingPass));

    auto skyPass = std::make_unique<SkyRenderPass>();
    if (!skyPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: SkyPass failed"); return false; }
    AddRenderPass(std::move(skyPass));

    // OutlineRenderPass intentionally disabled (hardcoded). Code kept for easy re-enable.
#if 0
    auto outlinePass = std::make_unique<OutlineRenderPass>();
    if (!outlinePass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: OutlinePass failed"); return false; }
    AddRenderPass(std::move(outlinePass));
#endif

    auto debugPass = std::make_unique<DebugRenderPass>();
    if (!debugPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: DebugPass failed"); return false; }
    AddRenderPass(std::move(debugPass));

    auto uiPass = std::make_unique<UiRenderPass>();
    if (!uiPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: UiPass failed"); return false; }
    AddRenderPass(std::move(uiPass));

    m_FxaaPass = std::make_unique<FxaaRenderPass>();
    if (!m_FxaaPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: FxaaPass failed"); return false; }

    m_SmaaPass = std::make_unique<SmaaRenderPass>();
    if (!m_SmaaPass->Initialize(m_Device, this)) {
        SM_ERROR("InitForSwap: SmaaPass failed");
        m_SmaaPass.reset(); // SMAA unavailable; AA switch falls back with a one-shot warn
    }

    m_SkinningPass = std::make_unique<SkinningComputePass>();
    if (!m_SkinningPass->RecreateGpuResources(m_Device, this)) { SM_ERROR("InitForSwap: SkinningComputePass failed"); return false; }

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
