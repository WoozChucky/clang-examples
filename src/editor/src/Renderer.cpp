#include "Renderer.h"

#include <iostream>


#include <nvrhi/utils.h>

#include "RendererBackendDX12.h"
#include "RendererBackendVulkan.h"

#include <tracy/Tracy.hpp>

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

    m_ImGuiRenderer = std::make_unique<ImGuiRenderer>();
    if (!m_ImGuiRenderer->Init(m_Window, m_Device)) {
        SM_ERROR("Failed to initialize ImGuiRenderer");
        return false;
    }

    SM_TRACE("Renderer initialized with API: %d", static_cast<int>(m_Backend->GetAPI()));

    PreparePrimitivePass();

    return true;
}

void Renderer::Shutdown(const uint32_t timeoutMs) {
    if (m_ImGuiRenderer) {
        m_ImGuiRenderer.reset();
        m_ImGuiRenderer = nullptr;
    }

    m_GpuTimer.Cleanup();

    {
        // Clear all pipeline objects
        m_PrimitivePass.m_Pipeline = nullptr;
        m_PrimitivePass.m_BindingSet = nullptr;
        m_PrimitivePass.m_BindingLayout = nullptr;
        m_PrimitivePass.m_InputLayout = nullptr;
        m_PrimitivePass.m_IndexCount = 0;
        m_PrimitivePass.m_PerFrameConstantBuffer = nullptr;
        m_PrimitivePass.m_PerDrawCB = nullptr;
        m_PrimitivePass.m_VertexBuffer = nullptr;
        m_PrimitivePass.m_IndexBuffer = nullptr;
        m_PrimitivePass.m_VS = nullptr;
        m_PrimitivePass.m_PS = nullptr;
    }

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

float Renderer::Render(double deltaTime, float red, float green, float blue, OrthographicCamera2D& uiCamera, PerspectiveCamera3D& gameCamera) {

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

            {
                ZoneScopedN("BeginRecording");
                m_CommandList->open();
                m_GpuTimer.Begin(m_CommandList);
            }


            {
                ZoneScopedN("RenderPasses");
                static glm::vec4 ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                //nvrhi::Color(ClearColor.r, ClearColor.g, ClearColor.b, ClearColor.a);
                const auto clearColor = nvrhi::Color(red, green, blue, ClearColor.a);

                nvrhi::utils::ClearColorAttachment(m_CommandList, frameBuffer, 0, clearColor);

                RenderSomethingTemporarily(frameBuffer, gameCamera);
            }

            {
                ZoneScopedN("SubmitRecording");
                m_GpuTimer.End(m_CommandList);
                m_CommandList->close();

                m_Device->executeCommandList(m_CommandList, nvrhi::CommandQueue::Graphics);

                m_GpuTimer.Advance();
            }

            m_ImGuiRenderer->Render(frameBuffer, deltaTime);

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
    // TODO: nullptr pipelines/render passes
    m_PrimitivePass.m_Pipeline = nullptr;
    if (m_Backend) {
        m_Backend->ResizeSwapChain(width, height);
    }
}

void Renderer::ToggleVSync() {
    m_BackendSettings.vsyncEnabled = !m_BackendSettings.vsyncEnabled;
}

inline auto PRIM_VS_HLSL = R"(
cbuffer PerFrame : register(b0, space0) // set = 0, binding = 0
{
    float4x4 uModel;
    float4x4 uVP;
    float4 uCameraPos; // xyz = camera world pos
};

struct VSIn { float3 Pos : POSITION; };
struct VSOut {
    float4 PosH  : SV_POSITION;
    float3 World : TEXCOORD0;
};

VSOut main_vs(VSIn vin)
{
    VSOut o;
    float4 wpos = mul(float4(vin.Pos, 1.0f), uModel);
    o.World = wpos.xyz;
    o.PosH = mul(uVP, wpos);
    return o;
}
)";

inline auto PRIM_PS_HLSL = R"(
cbuffer PerFrame : register(b0, space0) // set = 0, binding = 0
{
    float4x4 uModel;
    float4x4 uVP;
    float4 uCameraPos; // xyz = camera world pos
};

cbuffer PrimPerDraw : register(b2, space0) // set = 0, binding = 2
{
    float4 GridColor;
    float4 AxisXColor;
    float4 AxisZColor;
    float2 GridParams;  // (gridScale, lineThickness)
    float2 FadeParams;  // (fadeStart, fadeEnd)
};

struct PSIn {
    float4 PosH   : SV_POSITION;
    float3 World  : TEXCOORD0;
};

float4 main_ps(PSIn i) : SV_TARGET
{
    float3 P = i.World;

    // Fade by camera distance
    float dist = length(P - uCameraPos.xyz);
    float fade = 1.0;
    if (FadeParams.x < FadeParams.y) {
        fade = saturate(1.0 - smoothstep(FadeParams.x, FadeParams.y, dist));
    }

    // Grid coordinates in world units scaled by gridScale
    float gridScale = max(GridParams.x, 1e-4);
    float2 g = P.xz * gridScale;          // grid coordinates (world * scale)
    float2 fracg = abs(frac(g) - 0.5);    // distance inside cell

    // Use derivatives of the grid coordinate across the screen for AA
    float2 fw_g = max(abs(fwidth(g)), 1e-6);

    float thickness = max(GridParams.y, 0.5); // user thickness (approx pixels)
    // Anti-aliased grid lines (smoothstep over screen-space derivative scale)
    float gx = 1.0 - smoothstep(0.0, 0.5 * fw_g.x * thickness, fracg.x);
    float gz = 1.0 - smoothstep(0.0, 0.5 * fw_g.y * thickness, fracg.y);
    float gridLine = max(gx, gz);

    // Axis lines at world X==0 and Z==0 (use g directly; axis at g==0)
    float axisWidth = 1.5 * thickness;
    float a_fw_x = max(abs(fwidth(g.x)), 1e-6);
    float a_fw_y = max(abs(fwidth(g.y)), 1e-6);
    float ax = 1.0 - smoothstep(0.0, 0.5 * a_fw_x * axisWidth, abs(g.x));
    float az = 1.0 - smoothstep(0.0, 0.5 * a_fw_y * axisWidth, abs(g.y));

    float3 color = GridColor.rgb * gridLine;
    color = lerp(color, AxisXColor.rgb, ax);
    color = lerp(color, AxisZColor.rgb, az);

    return float4(color * fade, 1.0);
}
)";

void Renderer::PreparePrimitivePass() {
    m_PrimitivePass.m_PerFrameConstantBuffer = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(PerFrameCBData), "PerFrameCBData")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
    );

    m_PrimitivePass.m_PerDrawCB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(PrimPerDrawCB), "PrimPerDrawCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
    );

    // Compile primitive shaders (procedural grid)
    m_PrimitivePass.m_VS = m_Backend->CreateShaderFromMemory(nvrhi::ShaderType::Vertex, PRIM_VS_HLSL, 0, "main_vs", "vs_5_1");
    m_PrimitivePass.m_PS = m_Backend->CreateShaderFromMemory(nvrhi::ShaderType::Pixel, PRIM_PS_HLSL, 0, "main_ps", "ps_5_1");

    SM_ASSERT(m_PrimitivePass.m_VS && m_PrimitivePass.m_PS, "Failed to create primitive shaders");

    // Create big plane geometry on Y=0 (two triangles)
    struct PrimVertex { float x,y,z; };
    const float S = 2000.0f;
    PrimVertex verts[4] = { {-S,0,-S}, {+S,0,-S}, {+S,0,+S}, {-S,0,+S} };
    uint16_t inds[6] = { 0,1,2, 0,2,3 };

    auto cl = m_Device->createCommandList();
    cl->open();

    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = sizeof(verts);
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = "PrimPlaneVB";
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    m_PrimitivePass.m_VertexBuffer = m_Device->createBuffer(vbDesc);

    cl->beginTrackingBufferState(m_PrimitivePass.m_VertexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(m_PrimitivePass.m_VertexBuffer, verts, sizeof(verts));
    cl->setPermanentBufferState(m_PrimitivePass.m_VertexBuffer, nvrhi::ResourceStates::VertexBuffer);

    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = sizeof(inds);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "PrimPlaneIB";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    m_PrimitivePass.m_IndexBuffer = m_Device->createBuffer(ibDesc);

    cl->beginTrackingBufferState(m_PrimitivePass.m_IndexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(m_PrimitivePass.m_IndexBuffer, inds, sizeof(inds));
    cl->setPermanentBufferState(m_PrimitivePass.m_IndexBuffer, nvrhi::ResourceStates::IndexBuffer);

    cl->close();

    m_PrimitivePass.m_IndexCount = 6;

    m_Device->executeCommandList(cl);

    // Create input layout (POSITION only)
    nvrhi::VertexAttributeDesc attr;
    attr.setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0).setBufferIndex(0).setElementStride(sizeof(PrimVertex));
    m_PrimitivePass.m_InputLayout = m_Device->createInputLayout(&attr, 1, m_PrimitivePass.m_VS);

    // Create binding layout/set (b0 = PerFrame from main pass, b2 = PrimPerDraw)
    nvrhi::BindingSetDesc bs;
    bs.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_PrimitivePass.m_PerFrameConstantBuffer),
        nvrhi::BindingSetItem::ConstantBuffer(2, m_PrimitivePass.m_PerDrawCB)
    };

    if (!nvrhi::utils::CreateBindingSetAndLayout(
            m_Device,
            nvrhi::ShaderType::All,
            0,
            bs,
            m_PrimitivePass.m_BindingLayout,
            m_PrimitivePass.m_BindingSet, true))
    {
        SM_ASSERT(false, "Failed to create Primitive binding set/layout");
    }

}

void Renderer::RenderSomethingTemporarily(nvrhi::IFramebuffer* frameBuffer, PerspectiveCamera3D& camera) {
    if (!m_PrimitivePass.m_Pipeline)
    {
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_PrimitivePass.m_VS;
        pso.PS = m_PrimitivePass.m_PS;
        pso.inputLayout = m_PrimitivePass.m_InputLayout;
        pso.bindingLayouts = { m_PrimitivePass.m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        pso.renderState.depthStencilState.depthTestEnable = false; // no depth buffer yet
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        m_PrimitivePass.m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    // Common VP for this frame
    glm::mat4 V = camera.get_view_matrix();
    glm::mat4 P = camera.get_projection_matrix();

    // Defaults for the grid
    PrimPerDrawCB prim{};
    prim.GridColor  = {0.35f, 0.35f, 0.35f, 1.0f};
    prim.AxisXColor = {1.0f, 0.2f, 0.2f, 1.0f};
    prim.AxisZColor = {0.2f, 0.6f, 1.0f, 1.0f};
    prim.GridParams = {1.f, 2.0f};   // 1 unit per cell, 1px thickness
    prim.FadeParams = {100.0f, 150.0f};
    m_CommandList->writeBuffer(m_PrimitivePass.m_PerDrawCB, &prim, sizeof(prim));

    // State
    nvrhi::GraphicsState primState;
    primState.pipeline = m_PrimitivePass.m_Pipeline.Get();
    primState.framebuffer = frameBuffer;
    primState.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
    primState.bindings = { m_PrimitivePass.m_BindingSet };
    primState.vertexBuffers = { nvrhi::VertexBufferBinding(m_PrimitivePass.m_VertexBuffer, 0, 0) };
    primState.indexBuffer = nvrhi::IndexBufferBinding(m_PrimitivePass.m_IndexBuffer, nvrhi::Format::R16_UINT, 0);

    PrimitiveInstance inst{};
    inst.type = PrimitiveType::Plane;
    inst.transform = glm::mat4(1.0f); // Typically identity for Y=0 grid
    inst.color = {1,1,1,1};
    inst.params = {0,0,0,0};

    PerFrameCBData pf{};
    pf.Model = inst.transform;
    pf.VP = P * V;
    pf.CameraPos = glm::vec4(camera.position, 0.0f);

    m_CommandList->writeBuffer(m_PrimitivePass.m_PerFrameConstantBuffer, &pf, sizeof(pf));

    m_CommandList->setGraphicsState(primState);

    nvrhi::DrawArguments args{};
    args.vertexCount = m_PrimitivePass.m_IndexCount; // vertexCount acts as index count for indexed draw per NVRHI documentation
    args.instanceCount = 1;
    args.startIndexLocation = 0;
    args.startVertexLocation = 0;

    m_CommandList->drawIndexed(args);
}
