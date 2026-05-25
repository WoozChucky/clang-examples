#pragma once

#include <nvrhi/nvrhi.h>
#include <GLFW/glfw3.h>
#include <vector>
#include <utility>

#include "Engine.h"
#include "ApplicationContext.h"
#include "RendererBackend.h"
#include "IRenderPass.h"
#include "Fog.h"

#include "FrameAllocator.h"
#include "IOverlay.h"

// Systems for managing GPU resources
#include "MeshSystem.h"
#include "MaterialSystem.h"

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

class ENGINE_API Renderer final {
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

    float Render(double deltaTime, float red, float green, float blue,
                 SimulationSnapshot& snapshot, const ECS* world);
    void Resize(uint32_t width, uint32_t height);
    void ToggleVSync();

    // Phase B hot-swap. Runs synchronously on the RenderThread. Returns false
    // on a fatal, unrecoverable failure (caller should MessageBox + exit).
    bool SwapBackend(RendererAPI newApi);
    RendererAPI CurrentApi() const { return m_Backend ? m_Backend->GetAPI() : RendererAPI::Invalid; }

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

    // Inject an optional overlay BEFORE Init(); the renderer Inits/Renders/tears it down.
    void SetOverlay(std::unique_ptr<IOverlay> overlay) { m_Overlay = std::move(overlay); }

    // Resource upload APIs
    MeshHandle AddMesh(const MeshVertex* vertices, uint32_t vertexCount,
                       const uint32_t* indices, uint32_t indexCount, SubMesh* subMeshes = nullptr, uint32_t subMeshCount = 0);
    MaterialHandle AddMaterial(const uint32_t* textureRgba8, uint32_t texWidth, uint32_t texHeight);

    // Access to resource systems
    MeshSystem* GetMeshSystem() { return &m_MeshSystem; }
    MaterialSystem* GetMaterialSystem() { return &m_MaterialSystem; }
    ApplicationContext* GetAppContext() const { return m_AppContext; }

    // The camera the world passes render with this frame: editor override when active, else the
    // game's WorldCameraComponent. Resolved once at the top of Render(), before the pass loop.
    const CameraView& GetActiveCamera() const { return m_ActiveCamera; }

    // Directional-shadow GPU resources + shared view. Created/destroyed with the backend.
    // The shadow pass (Task 3) renders into m_ShadowFb and publishes LightVP into m_ShadowView;
    // the mesh pass (Task 4) samples m_ShadowDepth via m_ShadowSampler.
    struct ShadowView { glm::mat4 LightVP{1.0f}; int Enabled = 0; };

    nvrhi::ITexture*     GetShadowDepthTexture() const { return m_ShadowDepth; }
    nvrhi::ISampler*     GetShadowSampler()      const { return m_ShadowSampler; }
    nvrhi::IFramebuffer* GetShadowFramebuffer()  const { return m_ShadowFb; }
    ShadowView&          GetShadowView()               { return m_ShadowView; }

    nvrhi::ITexture*     GetGBufferAlbedo()      const { return m_GBuffer.Albedo;   }
    nvrhi::ITexture*     GetGBufferNormal()      const { return m_GBuffer.Normal;   }
    nvrhi::ITexture*     GetGBufferWorldPos()    const { return m_GBuffer.WorldPos; }
    nvrhi::IFramebuffer* GetGBufferFramebuffer() const { return m_GBuffer.Fb;       }

    // Per-frame fog resolved at the top of Render() (like GetActiveCamera): drives
    // both the scene clear color and the mesh pass's distance fog.
    const FogFrame&      GetFrameFog() const           { return m_FrameFog; }
    static constexpr uint32_t kShadowMapSize = 2048;

private:

    void TeardownForSwap();
    bool InitForSwap(RendererAPI newApi);
    // Creates the default magenta missing texture + default sampler.
    void CreateDefaultMaterialResources(nvrhi::TextureHandle& outMissing,
                                        nvrhi::SamplerHandle& outSampler);
    // Creates the directional-shadow D32 depth map, depth-only framebuffer, and comparison sampler.
    void CreateShadowResources();

    constexpr static uint32_t   SHUTDOWN_TIMEOUT = 5000;

    std::unique_ptr<IOverlay> m_Overlay;  // optional; editor injects ImGui, runtime injects none

    nvrhi::DeviceHandle         m_Device = nullptr;
    nvrhi::CommandListHandle    m_CommandList = nullptr;
    uint32_t                    m_FrameIndex = 0;

    GpuTimer                    m_GpuTimer {};
    FrameAllocator              m_FrameAllocator;

    GLFWwindow*                 m_Window;
    ApplicationContext*         m_AppContext;

    CameraView m_ActiveCamera{};

    // Directional-shadow resources (created in Init/InitForSwap, released in Shutdown/TeardownForSwap).
    nvrhi::TextureHandle     m_ShadowDepth;
    nvrhi::FramebufferHandle m_ShadowFb;
    nvrhi::SamplerHandle     m_ShadowSampler;
    ShadowView               m_ShadowView{};
    FogFrame                 m_FrameFog{}; // resolved each frame; drives clear + mesh-pass fog

    // Deferred G-buffer (Renderer-owned intermediates; recreated on size change,
    // released with the backend). RT0 albedo(linear) / RT1 world-normal / RT2 world-pos.
    struct GBuffer {
        nvrhi::TextureHandle     Albedo;   // RGBA8_UNORM linear
        nvrhi::TextureHandle     Normal;   // RGBA16_FLOAT
        nvrhi::TextureHandle     WorldPos; // RGBA16_FLOAT
        nvrhi::FramebufferHandle Fb;       // current frame's framebuffer (one of the cached ones)
        // One framebuffer per distinct shared-depth texture (swapchain rotates depths
        // across back-buffers; editor uses a single stable depth). Avoids per-frame churn.
        std::vector<std::pair<nvrhi::ITexture*, nvrhi::FramebufferHandle>> FbCache;
        uint32_t Width  = 0;
        uint32_t Height = 0;
    };
    GBuffer m_GBuffer{};

    // Builds m_GBuffer at the given size sharing the supplied depth texture.
    // No-op if already matching width/height/depth.
    void EnsureGBuffer(uint32_t width, uint32_t height, nvrhi::ITexture* sharedDepth);
    void ReleaseGBuffer();

    RendererBackend*            m_Backend = nullptr;
    RendererBackendSettings     m_BackendSettings{};

    std::vector<std::unique_ptr<IRenderPass>>   m_RenderPasses;

    // Resource management systems
    MeshSystem                  m_MeshSystem;
    MaterialSystem              m_MaterialSystem;
};
