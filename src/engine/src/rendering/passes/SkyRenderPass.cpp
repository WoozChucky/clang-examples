#include "SkyRenderPass.h"

#include "Renderer.h"
#include "Sky.h"
#include "CameraView.h"
#include "lib.h"
#include <nvrhi/utils.h>
#include <glm/vec3.hpp>
#include <glm/matrix.hpp>       // glm::inverse
#include <glm/trigonometric.hpp> // glm::radians
#include <cmath>                 // cosf

// Full-screen procedural sky. Draws into sky pixels only: the VS emits the
// full-screen triangle at z = 1.0 (far plane) and the pipeline uses a
// LessOrEqual depth test with depth writes disabled, so only pixels the
// deferred passes left at the far plane (no geometry) are painted. The PS
// reconstructs a world-space ray from the inverse view-projection and shades
// a day/night gradient plus sun/moon discs from GetSkySettings().
static const char* SKY_VS_HLSL = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o;
    o.UV = float2((vid << 1) & 2, vid & 2);
    // z = 1.0 (far plane) so the LessOrEqual depth test only passes on sky pixels.
    o.PosH = float4(o.UV * float2(2,-2) + float2(-1,1), 1.0, 1.0);
    return o;
}
)";

static const char* SKY_PS_HLSL = R"(
cbuffer SkyCB : register(b0) {
    float4x4 uInvViewProj;
    float4 uCameraPos;
    float4 uSunDir;
    float4 uDayZenith;
    float4 uDayHorizon;
    float4 uNightZenith;
    float4 uNightHorizon;
    float4 uSunColor;   // w = glow exponent
    float4 uMoonColor;  // w = glow exponent
    float4 uDisc;       // x sunCosOuter, y sunCosInner, z moonCosOuter, w moonCosInner
};
struct PSIn { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };

float4 main_ps(PSIn i) : SV_Target {
    float2 ndc = float2(i.UV.x * 2.0 - 1.0, 1.0 - i.UV.y * 2.0);
    float4 far = mul(uInvViewProj, float4(ndc, 1.0, 1.0));
    float3 rayDir = normalize(far.xyz / far.w - uCameraPos.xyz);

    float elev = saturate(-uSunDir.y);          // 1 noon, 0 horizon/night
    float t    = saturate(rayDir.y);            // horizon -> zenith
    float3 dayCol   = lerp(uDayHorizon.rgb,   uDayZenith.rgb,   t);
    float3 nightCol = lerp(uNightHorizon.rgb, uNightZenith.rgb, t);
    float3 sky = lerp(nightCol, dayCol, elev);

    float sdot = dot(rayDir, -uSunDir.xyz);
    float sunDisc = smoothstep(uDisc.x, uDisc.y, sdot);
    float sunHalo = pow(saturate(sdot), uSunColor.w) * 0.5;
    sky += uSunColor.rgb * (sunDisc + sunHalo);

    float mdot = dot(rayDir, uSunDir.xyz);
    float moonDisc = smoothstep(uDisc.z, uDisc.w, mdot);
    float moonHalo = pow(saturate(mdot), uMoonColor.w) * 0.3;
    sky += uMoonColor.rgb * (moonDisc + moonHalo);

    return float4(sky, 1.0);
}
)";

bool SkyRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer)
        return false;

    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, SKY_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel,  SKY_PS_HLSL, 0, "main_ps", "ps_6_1");
    if (!m_VS || !m_PS)
        return false;

    // Binding layout: only the per-frame constant buffer at b0. No textures,
    // no samplers, no input layout. Same Vulkan flat-binding offsets the other
    // passes use.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0)
    };
    nvrhi::VulkanBindingOffsets& offsets =
        nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0);

    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        layoutDesc.setBindingOffsets(offsets);
    }

    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    // Per-frame constant buffer.
    m_FrameCB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(SkyFrameCB), "SkyRenderPass FrameCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    return true;
}

void SkyRenderPass::Render(nvrhi::ICommandList* commandList,
                           nvrhi::IFramebuffer* frameBuffer,
                           SimulationSnapshot& /*snapshot*/,
                           const ECS* world,
                           double /*deltaTime*/,
                           FrameAllocator* /*frameAllocator*/)
{
    if (!GetSkySettings().Enabled)
        return;

    if (!m_Pipeline)
    {
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = m_PS;
        // No input layout: full-screen triangle generated from SV_VertexID.
        pso.bindingLayouts = { m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        // Far-plane depth test: paint only sky pixels (no geometry), never write depth.
        pso.renderState.depthStencilState.depthTestEnable = true;
        pso.renderState.depthStencilState.depthWriteEnable = false;
        pso.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        nvrhi::BlendState::RenderTarget rt;
        rt.setBlendEnable(false)
          .setColorWriteMask(nvrhi::ColorMask::All);
        pso.renderState.blendState.setRenderTarget(0, rt);
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    commandList->beginMarker("SkyRenderPass");

    // Gather the directional (sun) light from the ECS, like LightingRenderPass.
    glm::vec4 sunDir(0.0f, -1.0f, 0.0f, 0.0f); // default: straight down
    if (world)
    {
        world->Each<TransformComponent, LightningComponent>(
            [&](EntityId, const TransformComponent&, const LightningComponent& lightning)
        {
            if (lightning.Type == LightningType::Directional)
                sunDir = lightning.Direction;
        });
    }

    const CameraView& cam = m_Renderer->GetActiveCamera();
    const SkySettings& s = GetSkySettings();
    SkyFrameCB cb{};
    cb.InvViewProj  = glm::inverse(cam.Projection * cam.View);
    cb.CameraPos    = glm::vec4(cam.Position, 1.0f);
    cb.SunDir       = sunDir;
    cb.DayZenith    = glm::vec4(s.DayZenith, 0.0f);
    cb.DayHorizon   = glm::vec4(s.DayHorizon, 0.0f);
    cb.NightZenith  = glm::vec4(s.NightZenith, 0.0f);
    cb.NightHorizon = glm::vec4(s.NightHorizon, 0.0f);
    cb.SunColor     = glm::vec4(s.SunColor,  s.SunGlow);
    cb.MoonColor    = glm::vec4(s.MoonColor, s.MoonGlow);
    const float soft  = glm::radians(0.5f);
    const float sunR  = glm::radians(s.SunRadiusDeg);
    const float moonR = glm::radians(s.MoonRadiusDeg);
    cb.Disc = glm::vec4(cosf(sunR + soft), cosf(sunR), cosf(moonR + soft), cosf(moonR));
    commandList->writeBuffer(m_FrameCB, &cb, sizeof(cb));

    nvrhi::BindingSetDesc bindingDesc;
    bindingDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB)
    };
    nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = frameBuffer;
    state.bindings = { bindingSet };
    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
    // No vertex/index buffers: full-screen triangle from SV_VertexID.
    commandList->setGraphicsState(state);

    nvrhi::DrawArguments a;
    a.vertexCount = 3;
    commandList->draw(a);

    commandList->endMarker();
}

void SkyRenderPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_BindingLayout = nullptr;
    m_FrameCB = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;
    m_Device = nullptr;
    m_Renderer = nullptr;
}

void SkyRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    m_Pipeline = nullptr;
}
