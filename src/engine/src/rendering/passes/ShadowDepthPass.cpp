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

    // Volatile CB: written per-mesh inside the command list. NVRHI versions volatile CB writes per
    // draw (a static CB would clobber -> all meshes drawn with the last Model). maxVersions must
    // cover (meshes per frame) * (frames in flight); 1024 is generous for the current scene scale.
    m_CB = m_Device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ShadowCB), "ShadowDepthPass CB",
                                                       /*maxVersions=*/1024));

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
    layoutDesc.bindings = { nvrhi::BindingLayoutItem::VolatileConstantBuffer(0) };
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

    MeshSystem* ms = m_Renderer->GetMeshSystem();

    const ShadowSettings& shadow = GetShadowSettings();

    // Prefer fitting the light ortho to the camera frustum slice (concentrates the fixed-size
    // shadow map's texels where the camera looks). Fall back to the all-visible-meshes AABB if
    // the camera is unavailable/degenerate.
    glm::vec3 center(0.0f);
    float radius = 0.0f;
    bool haveFit = false;
    {
        const CameraView& cam = m_Renderer->GetActiveCamera();
        const glm::vec3 fwd = CameraForward(cam.View);
        if (glm::dot(fwd, fwd) > 0.5f) {                              // valid camera basis
            // Fit the ortho box to the camera frustum slice [near, ShadowDistance], so the fixed
            // 4096^2 texels concentrate exactly on what the camera sees. Smaller ShadowDistance
            // -> smaller box -> smaller texels -> sharper shadows.
            const float dist = glm::max(shadow.ShadowDistance, 1.0f);
            const ShadowSphere s = FrustumSliceSphere(cam.View, cam.Projection, dist);
            radius = glm::max(s.radius, 1.0f);                        // ortho half-extent
            center = SnapToTexelGrid(s.center, radius, sunDir, Renderer::kShadowMapSize);
            haveFit = true;
        }
    }
    if (!haveFit) {
        // Fallback: fit to the world-space AABB of all visible meshes (the original behavior).
        glm::vec3 mn(std::numeric_limits<float>::max());
        glm::vec3 mx(-std::numeric_limits<float>::max());
        bool any = false;
        world->Each<TransformComponent, MeshComponent>(
            [&](EntityId, const TransformComponent& t, const MeshComponent& m)
            {
                if (!m.Visible) return;
                const auto b = ms->GetMeshBounds(m.MeshId);
                if (!b.valid) return;
                glm::vec3 wMin, wMax;
                TransformAABB(ModelMatrix(t), b.min, b.max, wMin, wMax);
                mn = glm::min(mn, wMin); mx = glm::max(mx, wMax); any = true;
            });
        if (!any) return;
        center = 0.5f * (mn + mx);
        radius = 0.5f * glm::length(mx - mn);
        static bool s_warnedShadowFit = false;
        if (!s_warnedShadowFit) { SM_WARN("ShadowDepthPass: camera frustum fit unavailable; using scene-AABB fallback"); s_warnedShadowFit = true; }
    }

    const glm::mat4 lightVP = ComputeLightViewProj(center, radius, sunDir, shadow.NearExtend);
    sv.LightVP = lightVP;
    sv.Enabled = 1;
    sv.Radius  = radius;

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

    // Skinned entities cast deformed shadows: bind the per-frame compute-skinned VB (posed
    // mesh-local vertices) at the entity's offset instead of the static bind-pose VB. The
    // mesh's index buffer is unchanged; baseVertex (startVertexLocation) shifts each index
    // into the entity's range within the shared skinned VB. The Model CB is still applied.
    SkinningComputePass* skinningPass = m_Renderer->GetSkinningPass();

    world->Each<TransformComponent, MeshComponent>(
        [&](EntityId e, const TransformComponent& t, const MeshComponent& m)
        {
            if (!m.Visible)
                return;
            if (!ms->IsValidMeshId(m.MeshId))
                return;
            const auto res = ms->GetMeshResources(m.MeshId);
            if (!res.valid)
                return;

            const int64_t skinnedOff = skinningPass ? skinningPass->GetSkinnedVertexOffset(e) : -1;
            const bool useSkinned = (skinnedOff >= 0) && skinningPass->GetSkinnedVertexBuffer();
            nvrhi::IBuffer* vb = useSkinned ? skinningPass->GetSkinnedVertexBuffer() : res.vertexBuffer.Get();
            const uint32_t baseVertex = useSkinned ? (uint32_t)skinnedOff : 0;

            ShadowCB cb{};
            cb.LightVP = lightVP;
            cb.Model = ModelMatrix(t);
            commandList->writeBuffer(m_CB, &cb, sizeof(cb));

            nvrhi::GraphicsState state;
            state.pipeline = m_Pipeline;
            state.framebuffer = shadowFb;
            state.viewport.addViewportAndScissorRect(shadowFb->getFramebufferInfo().getViewport());
            state.bindings = { m_BindingSet };
            state.vertexBuffers = { nvrhi::VertexBufferBinding(vb, 0, 0) };
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
                    a.startVertexLocation = baseVertex;
                    commandList->drawIndexed(a);
                }
            }
            else
            {
                nvrhi::DrawArguments a{};
                a.vertexCount = res.indexCount;
                a.instanceCount = 1;
                a.startVertexLocation = baseVertex;
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
