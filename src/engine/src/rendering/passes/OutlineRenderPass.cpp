#include "OutlineRenderPass.h"

#include <nvrhi/utils.h>

#include "Renderer.h"
#include "MeshSystem.h"
#include "ApplicationContext.h"
#include "ECS.h"
#include "TransformMath.h"
#include "lib.h"

namespace {
const char* OUTLINE_VS_HLSL = R"(
cbuffer OutlineCB : register(b0)
{
    float4x4 VP;
    float4x4 Model;
    float     OutlineWidth;
    float3    _pad0;
    float4    OutlineColor;
};
struct VSIn  { float3 Position : POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD; };
struct VSOut { float4 PosH : SV_Position; };
VSOut main_vs(VSIn vin)
{
    VSOut o;
    float3 lp = vin.Position + normalize(vin.Normal) * OutlineWidth;
    float4 wp = mul(Model, float4(lp, 1.0));
    o.PosH = mul(VP, wp);
    return o;
}
)";

const char* OUTLINE_PS_HLSL = R"(
cbuffer OutlineCB : register(b0)
{
    float4x4 VP;
    float4x4 Model;
    float     OutlineWidth;
    float3    _pad0;
    float4    OutlineColor;
};
struct VSOut { float4 PosH : SV_Position; };
float4 main_ps(VSOut pin) : SV_Target { return OutlineColor; }
)";
} // namespace

bool OutlineRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer || !renderer->GetMeshSystem())
        return false;

    m_CB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(OutlineCB), "OutlineRenderPass CB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, OUTLINE_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel,  OUTLINE_PS_HLSL, 0, "main_ps", "ps_6_1");
    if (!m_VS || !m_PS)
        return false;

    nvrhi::VertexAttributeDesc attrs[3];
    attrs[0].setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(MeshVertex, px)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    attrs[1].setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(MeshVertex, nx)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    attrs[2].setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT)
        .setOffset(offsetof(MeshVertex, u)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    m_InputLayout = m_Device->createInputLayout(attrs, 3, m_VS);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0) };
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        layoutDesc.setBindingOffsets(nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0));
    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    nvrhi::BindingSetDesc bsd;
    bsd.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, m_CB) };
    m_BindingSet = m_Device->createBindingSet(bsd, m_BindingLayout);

    return true;
}

void OutlineRenderPass::Render(nvrhi::ICommandList* commandList,
                               nvrhi::IFramebuffer* frameBuffer,
                               SimulationSnapshot& /*snapshot*/,
                               const ECS* world,
                               double /*deltaTime*/,
                               FrameAllocator* /*frameAllocator*/)
{
    if (!world)
        return;

    const EntityId sel = m_Renderer->GetAppContext()->SelectedEntity.load(std::memory_order_relaxed);
    if (sel == INVALID_ENTITY)
        return;

    const auto* mc = world->GetComponent<MeshComponent>(sel);
    const auto* tc = world->GetComponent<TransformComponent>(sel);
    if (!mc || !tc || !mc->Visible)
        return;

    auto res = m_Renderer->GetMeshSystem()->GetMeshResources(mc->MeshId);
    if (!res.valid)
        return;

    glm::mat4 V(1.0f), P(1.0f);
    if (const auto* cam = world->GetSingleton<WorldCameraComponent>())
    {
        V = cam->View;
        P = cam->Projection;
    }

    if (!m_Pipeline)
    {
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = m_PS;
        pso.inputLayout = m_InputLayout;
        pso.bindingLayouts = { m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        pso.renderState.depthStencilState.depthTestEnable = true;
        pso.renderState.depthStencilState.depthWriteEnable = false; // overlay rim; don't pollute depth (outline runs after the mesh)
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Front;
        pso.renderState.rasterState.setFrontCounterClockwise(true);
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    OutlineCB cb{};
    cb.VP = P * V;
    cb.Model = ModelMatrix(*tc);
    cb.OutlineWidth = 0.03f;
    cb.OutlineColor = glm::vec4(1.0f, 0.6f, 0.1f, 1.0f);
    commandList->writeBuffer(m_CB, &cb, sizeof(cb));

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = frameBuffer;
    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
    state.bindings = { m_BindingSet };
    state.vertexBuffers = { nvrhi::VertexBufferBinding(res.vertexBuffer, 0, 0) };
    state.indexBuffer = nvrhi::IndexBufferBinding(res.indexBuffer, nvrhi::Format::R32_UINT, 0);
    commandList->setGraphicsState(state);

    if (res.subMeshes.size() > 0)
    {
        for (const auto& sm : res.subMeshes)
        {
            nvrhi::DrawArguments args{};
            args.vertexCount = sm.IndexCount;
            args.instanceCount = 1;
            args.startIndexLocation = sm.IndexStart;
            args.startVertexLocation = 0;
            commandList->drawIndexed(args);
        }
    }
    else
    {
        nvrhi::DrawArguments args{};
        args.vertexCount = res.indexCount;
        args.instanceCount = 1;
        commandList->drawIndexed(args);
    }
}

void OutlineRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    m_Pipeline = nullptr;
}

void OutlineRenderPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_BindingSet = nullptr;
    m_CB = nullptr;
    m_InputLayout = nullptr;
    m_BindingLayout = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;
}
