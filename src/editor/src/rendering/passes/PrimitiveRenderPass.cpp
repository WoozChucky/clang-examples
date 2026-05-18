#include "PrimitiveRenderPass.h"
#include "Renderer.h"
#include "lib.h"
#include <nvrhi/utils.h>

#include "glm/mat4x4.hpp"
#include "glm/vec2.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"
#include "tracy/Tracy.hpp"

// Helper structs for primitive pass
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

// Shader source code
static const char* PRIM_VS_HLSL = R"(
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

static const char* PRIM_PS_HLSL = R"(
cbuffer PerFrame : register(b0, space0) // set = 0, binding = 0
{
    float4x4 uModel;
    float4x4 uVP;
    float4 uCameraPos; // xyz = camera world pos
};

cbuffer PrimPerDraw : register(b1, space0) // set = 0, binding = 1
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

bool PrimitiveRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer) {
    m_Device = device;
    m_Renderer = renderer;

    if (!m_Device || !m_Renderer) {
        SM_ERROR("PrimitiveRenderPass::Initialize - Invalid device or renderer");
        return false;
    }

    // Create constant buffers
    m_PerFrameConstantBuffer = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(PerFrameCBData), "PrimitiveRenderPass PerFrameConstantBuffer")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
    );

    m_PerDrawCB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(PrimPerDrawCB), "PrimitiveRenderPass PerDrawConstantBuffer")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
    );

    // Compile primitive shaders (procedural grid)
    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, PRIM_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel, PRIM_PS_HLSL, 0, "main_ps", "ps_6_1");

    if (!m_VS || !m_PS) {
        SM_ERROR("Failed to create primitive shaders");
        return false;
    }

    // Create big plane geometry on Y=0 (two triangles)
    struct PrimVertex { float x, y, z; };
    const float S = 2000.0f;
    PrimVertex verts[4] = { {-S,0,-S}, {+S,0,-S}, {+S,0,+S}, {-S,0,+S} };
    uint16_t inds[6] = { 0,1,2, 0,2,3 };

    auto cl = m_Device->createCommandList();
    cl->open();

    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = sizeof(verts);
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = "PrimitiveRenderPass VertexBuffer";
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    m_VertexBuffer = m_Device->createBuffer(vbDesc);

    cl->beginTrackingBufferState(m_VertexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(m_VertexBuffer, verts, sizeof(verts));
    cl->setPermanentBufferState(m_VertexBuffer, nvrhi::ResourceStates::VertexBuffer);

    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = sizeof(inds);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "PrimitiveRenderPass IndexBuffer";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    m_IndexBuffer = m_Device->createBuffer(ibDesc);

    cl->beginTrackingBufferState(m_IndexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(m_IndexBuffer, inds, sizeof(inds));
    cl->setPermanentBufferState(m_IndexBuffer, nvrhi::ResourceStates::IndexBuffer);

    cl->close();

    m_IndexCount = 6;

    m_Device->executeCommandList(cl);

    // Create input layout (POSITION only)
    nvrhi::VertexAttributeDesc attr;
    attr.setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0).setBufferIndex(0).setElementStride(sizeof(PrimVertex));
    m_InputLayout = m_Device->createInputLayout(&attr, 1, m_VS);


    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = false;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),
        nvrhi::BindingLayoutItem::ConstantBuffer(1),
    };
    nvrhi::VulkanBindingOffsets& offsets =
        nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0);

    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        layoutDesc.setBindingOffsets(offsets);
    }

    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    nvrhi::BindingSetDesc desc;
    desc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_PerFrameConstantBuffer),
        nvrhi::BindingSetItem::ConstantBuffer(1, m_PerDrawCB),
    };

    nvrhi::BindingSetHandle binding;
    m_BindingSet = m_Device->createBindingSet(desc, m_BindingLayout);

    if (!m_BindingLayout || !m_BindingSet) {
        SM_ERROR("Failed to create primitive binding layout or set");
        return false;
    }

    return true;
}

void PrimitiveRenderPass::Render(
    nvrhi::ICommandList* commandList,
    nvrhi::IFramebuffer* frameBuffer,
    SimulationSnapshot& snapshot,
    const ECS* /*world*/,
    double deltaTime,
    FrameAllocator* frameAllocator)
{
    ZoneScopedN("PrimitiveRenderPass");
    if (!m_Pipeline)
    {
        ZoneScopedN("Pipeline Creation");
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = m_PS;
        pso.inputLayout = m_InputLayout;
        pso.bindingLayouts = { m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        pso.renderState.depthStencilState.depthTestEnable = false; // no depth buffer yet
        pso.renderState.depthStencilState.depthWriteEnable = false;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    commandList->beginMarker("PrimiviteRenderPass");

    // Common VP for this frame
    glm::mat4 V = snapshot.GameCamera.get_view_matrix();
    glm::mat4 P = snapshot.GameCamera.get_projection_matrix();

    {
        ZoneScopedN("Upload ContantBuffer 1");
        // Defaults for the grid
        PrimPerDrawCB prim{};
        prim.GridColor  = {0.35f, 0.35f, 0.35f, 1.0f};
        prim.AxisXColor = {1.0f, 0.2f, 0.2f, 1.0f};
        prim.AxisZColor = {0.2f, 0.6f, 1.0f, 1.0f};
        prim.GridParams = {1.f, 2.0f};   // 1 unit per cell, 2px thickness
        prim.FadeParams = {100.0f, 150.0f};
        commandList->writeBuffer(m_PerDrawCB, &prim, sizeof(prim));
    }

    // State
    nvrhi::GraphicsState primState;
    {
        ZoneScopedN("Create Graphics State");
        primState.pipeline = m_Pipeline;
        primState.framebuffer = frameBuffer;
        primState.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
        primState.bindings = { m_BindingSet };
        primState.vertexBuffers = { nvrhi::VertexBufferBinding(m_VertexBuffer, 0, 0) };
        primState.indexBuffer = nvrhi::IndexBufferBinding(m_IndexBuffer, nvrhi::Format::R16_UINT, 0);
    }

    {
        ZoneScopedN("Upload ContantBuffer 2");
        PrimitiveInstance inst{};
        inst.type = PrimitiveType::Plane;
        inst.transform = glm::mat4(1.0f); // Typically identity for Y=0 grid
        inst.color = {1,1,1,1};
        inst.params = {0,0,0,0};

        PerFrameCBData pf{};
        pf.Model = inst.transform;
        pf.VP = P * V;
        pf.CameraPos = glm::vec4(snapshot.GameCamera.position, 0.0f);

        commandList->writeBuffer(m_PerFrameConstantBuffer, &pf, sizeof(pf));
    }

    {
        ZoneScopedN("Set Graphics State");
        commandList->setGraphicsState(primState);
    }

    {
        ZoneScopedN("Draw");
        nvrhi::DrawArguments args{};
        args.vertexCount = m_IndexCount; // vertexCount acts as index count for indexed draw per NVRHI documentation
        args.instanceCount = 1;
        args.startIndexLocation = 0;
        args.startVertexLocation = 0;

        commandList->drawIndexed(args);
    }

    commandList->endMarker();
}

void PrimitiveRenderPass::Shutdown() {
    // Clear all pipeline objects
    m_Pipeline = nullptr;
    m_BindingSet = nullptr;
    m_BindingLayout = nullptr;
    m_InputLayout = nullptr;
    m_IndexCount = 0;
    m_PerFrameConstantBuffer = nullptr;
    m_PerDrawCB = nullptr;
    m_VertexBuffer = nullptr;
    m_IndexBuffer = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;

    m_Device = nullptr;
}

void PrimitiveRenderPass::OnResize(uint32_t width, uint32_t height) {
    // Invalidate pipeline on resize so it gets recreated with new framebuffer info
    m_Pipeline = nullptr;
}
