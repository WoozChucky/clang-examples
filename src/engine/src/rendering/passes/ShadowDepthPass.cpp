#include "ShadowDepthPass.h"

#include <nvrhi/utils.h>
#include <glm/glm.hpp>
#include <limits>

#include "Renderer.h"
#include "MeshSystem.h"
#include "RenderStats.h"
#include "ShadowMath.h"
#include "ECS.h"
#include "TransformMath.h"
#include "Frustum.h"      // TransformAABB
#include "lib.h"

namespace {
const char* SHADOW_VS_HLSL = R"(
cbuffer ShadowCB : register(b0) { float4x4 LightVP; float4x4 Model; };
struct VSIn  { float3 Position : POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD; };
float4 main_vs(VSIn vin) : SV_Position { return mul(LightVP, mul(Model, float4(vin.Position, 1.0))); }
)";
} // namespace

bool ShadowDepthPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer || !renderer->GetMeshSystem())
        return false;

    m_CB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(ShadowCB), "ShadowDepthPass CB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, SHADOW_VS_HLSL, 0, "main_vs", "vs_6_1");
    if (!m_VS)
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

void ShadowDepthPass::Render(nvrhi::ICommandList* commandList,
                             nvrhi::IFramebuffer* /*frameBuffer*/,
                             SimulationSnapshot& /*snapshot*/,
                             const ECS* world,
                             double /*deltaTime*/,
                             FrameAllocator* /*frameAllocator*/)
{
    Renderer::ShadowView& sv = m_Renderer->GetShadowView();
    sv.Enabled = 0;
    if (!world || !GetShadowSettings().Enabled)
        return;

    // Find the directional (sun) light.
    glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
    bool haveSun = false;
    world->Each<TransformComponent, LightningComponent>(
        [&](EntityId, const TransformComponent&, const LightningComponent& l)
        {
            if (l.Type == LightningType::Directional)
            {
                sunDir = glm::vec3(l.Direction);
                haveSun = true;
            }
        });
    if (!haveSun || !IsSunUp(sunDir))
        return;

    // Fit the light frustum to the world-space AABB of all visible meshes.
    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(-std::numeric_limits<float>::max());
    bool any = false;
    MeshSystem* ms = m_Renderer->GetMeshSystem();
    world->Each<TransformComponent, MeshComponent>(
        [&](EntityId, const TransformComponent& t, const MeshComponent& m)
        {
            if (!m.Visible)
                return;
            const auto b = ms->GetMeshBounds(m.MeshId);
            if (!b.valid)
                return;
            glm::vec3 wMin, wMax;
            TransformAABB(ModelMatrix(t), b.min, b.max, wMin, wMax);
            mn = glm::min(mn, wMin);
            mx = glm::max(mx, wMax);
            any = true;
        });
    if (!any)
        return;

    const glm::vec3 center = 0.5f * (mn + mx);
    const float radius = 0.5f * glm::length(mx - mn);
    const glm::mat4 lightVP = ComputeLightViewProj(center, radius, sunDir);
    sv.LightVP = lightVP;
    sv.Enabled = 1;

    nvrhi::IFramebuffer* shadowFb = m_Renderer->GetShadowFramebuffer();
    if (!shadowFb)
        return;

    if (!m_Pipeline)
    {
        const auto fbi = shadowFb->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = nullptr;                                   // depth-only
        pso.inputLayout = m_InputLayout;
        pso.bindingLayouts = { m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        pso.renderState.depthStencilState.depthTestEnable = true;
        pso.renderState.depthStencilState.depthWriteEnable = true;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Front;
        pso.renderState.rasterState.setFrontCounterClockwise(true);
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    commandList->beginMarker("ShadowDepthPass");

    commandList->clearDepthStencilTexture(shadowFb->getDesc().depthAttachment.texture,
                                          nvrhi::AllSubresources, true, 1.0f, false, 0);

    world->Each<TransformComponent, MeshComponent>(
        [&](EntityId, const TransformComponent& t, const MeshComponent& m)
        {
            if (!m.Visible)
                return;
            if (!ms->IsValidMeshId(m.MeshId))
                return;
            const auto res = ms->GetMeshResources(m.MeshId);
            if (!res.valid)
                return;

            ShadowCB cb{};
            cb.LightVP = lightVP;
            cb.Model = ModelMatrix(t);
            commandList->writeBuffer(m_CB, &cb, sizeof(cb));

            nvrhi::GraphicsState state;
            state.pipeline = m_Pipeline;
            state.framebuffer = shadowFb;
            state.viewport.addViewportAndScissorRect(shadowFb->getFramebufferInfo().getViewport());
            state.bindings = { m_BindingSet };
            state.vertexBuffers = { nvrhi::VertexBufferBinding(res.vertexBuffer, 0, 0) };
            state.indexBuffer = nvrhi::IndexBufferBinding(res.indexBuffer, nvrhi::Format::R32_UINT, 0);
            commandList->setGraphicsState(state);

            if (res.subMeshes.size() > 0)
            {
                for (const auto& sm : res.subMeshes)
                {
                    nvrhi::DrawArguments a{};
                    a.vertexCount = sm.IndexCount;
                    a.instanceCount = 1;
                    a.startIndexLocation = sm.IndexStart;
                    commandList->drawIndexed(a);
                }
            }
            else
            {
                nvrhi::DrawArguments a{};
                a.vertexCount = res.indexCount;
                a.instanceCount = 1;
                commandList->drawIndexed(a);
            }
        });

    commandList->endMarker();
}

void ShadowDepthPass::OnResize(uint32_t /*width*/, uint32_t /*height*/) {}   // shadow map is fixed-size

void ShadowDepthPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_BindingSet = nullptr;
    m_CB = nullptr;
    m_InputLayout = nullptr;
    m_BindingLayout = nullptr;
    m_VS = nullptr;
}
