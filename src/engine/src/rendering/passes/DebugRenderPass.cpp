#include "DebugRenderPass.h"

#include <nvrhi/utils.h>
#include <glm/glm.hpp> // glm::inverse, glm::normalize, glm::length, glm::cross

#include "Renderer.h"
#include "MeshSystem.h"
#include "RenderStats.h"
#include "ApplicationContext.h"
#include "ECS.h"
#include "TransformMath.h"
#include "Frustum.h"     // TransformAABB
#include "lib.h"

namespace {
const char* DEBUG_VS_HLSL = R"(
cbuffer DebugCB : register(b0) { float4x4 VP; };
struct VSIn  { float3 Position : POSITION; float4 Color : COLOR; };
struct VSOut { float4 PosH : SV_Position; float4 Color : COLOR; };
VSOut main_vs(VSIn i) { VSOut o; o.PosH = mul(VP, float4(i.Position, 1.0)); o.Color = i.Color; return o; }
)";
const char* DEBUG_PS_HLSL = R"(
struct VSOut { float4 PosH : SV_Position; float4 Color : COLOR; };
float4 main_ps(VSOut i) : SV_Target { return i.Color; }
)";
} // namespace

bool DebugRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer || !renderer->GetMeshSystem())
        return false;

    m_CB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(DebugCB), "DebugRenderPass CB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));

    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, DEBUG_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel,  DEBUG_PS_HLSL, 0, "main_ps", "ps_6_1");
    if (!m_VS || !m_PS)
        return false;

    nvrhi::VertexAttributeDesc attrs[2];
    attrs[0].setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(DebugVertex, Position)).setBufferIndex(0).setElementStride(sizeof(DebugVertex));
    attrs[1].setName("COLOR").setFormat(nvrhi::Format::RGBA32_FLOAT)
        .setOffset(offsetof(DebugVertex, Color)).setBufferIndex(0).setElementStride(sizeof(DebugVertex));
    m_InputLayout = m_Device->createInputLayout(attrs, 2, m_VS);

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

void DebugRenderPass::EnsureVertexCapacity(size_t vertexCount)
{
    if (vertexCount <= m_VertexCapacity && m_VertexBuffer)
        return;
    size_t cap = m_VertexCapacity ? m_VertexCapacity : 4096;
    while (cap < vertexCount) cap *= 2;
    nvrhi::BufferDesc desc;
    desc.byteSize = cap * sizeof(DebugVertex);
    desc.isVertexBuffer = true;
    desc.initialState = nvrhi::ResourceStates::VertexBuffer;
    desc.keepInitialState = true;
    desc.debugName = "DebugRenderPass VB";
    m_VertexBuffer = m_Device->createBuffer(desc);
    m_VertexCapacity = cap;
}

void DebugRenderPass::Render(nvrhi::ICommandList* commandList,
                             nvrhi::IFramebuffer* frameBuffer,
                             SimulationSnapshot& /*snapshot*/,
                             const ECS* world,
                             double /*deltaTime*/,
                             FrameAllocator* /*frameAllocator*/)
{
    if (!world)
        return;

    const DebugDrawSettings& s = GetDebugDrawSettings();
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid && !s.ShowColliders)
        return;

    m_Verts.clear();

    if (s.ShowGrid) {
        const glm::vec3 camPos = m_Renderer->GetActiveCamera().Position;
        DebugAppendGrid(m_Verts, camPos, 50.0f, 1.0f,
                        glm::vec4(0.35f, 0.35f, 0.35f, 1.0f),  // grid grey
                        glm::vec4(1.0f, 0.2f, 0.2f, 1.0f),     // X axis red
                        glm::vec4(0.2f, 0.6f, 1.0f, 1.0f));    // Z axis blue
    }

    if (s.ShowLightGizmos) {
        world->Each<TransformComponent, LightningComponent>(
            [&](EntityId, const TransformComponent& t, const LightningComponent& l) {
                const glm::vec4 col = l.Color;
                if (l.Type == LightningType::Point) {
                    const float r = (l.Range > 0.001f) ? l.Range : 1.0f;
                    DebugAppendSphere(m_Verts, t.Position, r, col);
                } else if (l.Type == LightningType::Directional) {
                    glm::vec3 d = glm::vec3(l.Direction);
                    if (glm::length(d) < 1e-5f) d = glm::vec3(0, -1, 0);
                    d = glm::normalize(d);
                    DebugAppendArrow(m_Verts, glm::vec3(0.0f), d * 2.0f, col);
                }
            });
    }

    if (s.ShowCameraFrustum) {
        if (const auto* cam = world->GetSingleton<WorldCameraComponent>()) {
            const glm::mat4 invVP = glm::inverse(cam->Projection * cam->View);
            DebugAppendFrustum(m_Verts, invVP, glm::vec4(0.4f, 0.8f, 1.0f, 1.0f));
        }
    }

    if (s.ShowSelectedAABB) {
        const EntityId sel = m_Renderer->GetAppContext()->SelectedEntity.load(std::memory_order_relaxed);
        if (sel != INVALID_ENTITY) {
            if (const auto* tc = world->GetComponent<TransformComponent>(sel)) {
                const glm::vec4 col(1.0f, 0.9f, 0.2f, 1.0f);
                bool drew = false;
                if (const auto* mc = world->GetComponent<MeshComponent>(sel)) {
                    const auto b = m_Renderer->GetMeshSystem()->GetMeshBounds(mc->MeshId);
                    if (b.valid) {
                        glm::vec3 wMin, wMax;
                        TransformAABB(ModelMatrix(*tc), b.min, b.max, wMin, wMax);
                        DebugAppendBox(m_Verts, wMin, wMax, col);
                        drew = true;
                    }
                }
                if (!drew) {
                    const glm::vec3 c = tc->Position, e(0.25f);
                    DebugAppendBox(m_Verts, c - e, c + e, col);
                }
            }
        }
    }

    if (s.ShowColliders) {
        // Mirrors src/game/src/Collision.h math (center = Position + Offset*Scale;
        // extents per shape). Inlined here so the engine debug pass doesn't depend on the
        // game header.
        world->Each<TransformComponent, ColliderComponent>(
            [&](EntityId, const TransformComponent& t, const ColliderComponent& c) {
                const glm::vec3 absScale = glm::abs(t.Scale);
                const glm::vec3 center   = t.Position + c.Offset * t.Scale;

                const glm::vec4 color =
                      c.IsTrigger ? glm::vec4(0.20f, 0.85f, 1.00f, 1.0f)   // cyan: trigger
                    : !c.IsStatic ? glm::vec4(1.00f, 0.55f, 0.10f, 1.0f)   // orange: dynamic
                                  : glm::vec4(1.00f, 0.85f, 0.10f, 1.0f);  // yellow: static blocker

                switch (c.Shape) {
                    case ColliderShape::Box: {
                        const glm::vec3 ext = glm::max(glm::abs(c.Size) * absScale, glm::vec3(0.0001f));
                        DebugAppendBox(m_Verts, center - ext, center + ext, color);
                        break;
                    }
                    case ColliderShape::Sphere: {
                        const float r = std::max({ c.Size.x * absScale.x,
                                                   c.Size.x * absScale.y,
                                                   c.Size.x * absScale.z, 0.0f });
                        DebugAppendSphere(m_Verts, center, r, color);
                        break;
                    }
                    case ColliderShape::Capsule: {
                        const float r  = std::max({ c.Size.x * absScale.x,
                                                    c.Size.x * absScale.z, 0.0f });
                        const float hh = std::max(c.Size.y * absScale.y, 0.0f);
                        DebugAppendCapsule(m_Verts, center, r, hh, color);
                        break;
                    }
                }
            });
    }

    if (m_Verts.empty())
        return;

    if (!m_Pipeline) {
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = m_PS;
        pso.inputLayout = m_InputLayout;
        pso.bindingLayouts = { m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::LineList;
        pso.renderState.depthStencilState.depthTestEnable = true;
        pso.renderState.depthStencilState.depthWriteEnable = false;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    EnsureVertexCapacity(m_Verts.size());

    const CameraView& cam = m_Renderer->GetActiveCamera();
    DebugCB cb{}; cb.VP = cam.Projection * cam.View;
    commandList->writeBuffer(m_CB, &cb, sizeof(cb));
    commandList->writeBuffer(m_VertexBuffer, m_Verts.data(), m_Verts.size() * sizeof(DebugVertex));

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = frameBuffer;
    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
    state.bindings = { m_BindingSet };
    state.vertexBuffers = { nvrhi::VertexBufferBinding(m_VertexBuffer, 0, 0) };
    commandList->setGraphicsState(state);

    nvrhi::DrawArguments args{};
    args.vertexCount = static_cast<uint32_t>(m_Verts.size());
    args.instanceCount = 1;
    commandList->draw(args);
}

void DebugRenderPass::OnResize(uint32_t, uint32_t) { m_Pipeline = nullptr; }

void DebugRenderPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_BindingSet = nullptr;
    m_CB = nullptr;
    m_VertexBuffer = nullptr;
    m_VertexCapacity = 0;
    m_InputLayout = nullptr;
    m_BindingLayout = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;
}
