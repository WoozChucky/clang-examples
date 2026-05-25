#include "LightingRenderPass.h"

#include "Renderer.h"
#include "Fog.h"
#include "RenderStats.h"
#include <nvrhi/utils.h>
#include "lib.h"
#include <glm/vec3.hpp>

// Full-screen deferred lighting. The PS math is a 1:1 port of MeshRenderPass's
// pixel shader (ambient/diffuse/point loop/ShadowFactor 3x3 PCF/fog) but reads
// its surface inputs (albedo/world-normal/world-pos) from the G-buffer instead
// of interpolated vertex attributes. Registers mirror the mesh-pass scheme so
// the Vulkan flat-binding offsets line up (CB b0, t0/t1/t2 gbuffer, s0 sampler,
// t4 point lights, t6 shadow map, s7 shadow comparison sampler).
static const char* LIGHT_VS_HLSL = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o;
    o.UV = float2((vid << 1) & 2, vid & 2);
    o.PosH = float4(o.UV * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}
)";

static const char* LIGHT_PS_HLSL = R"(
struct DirectionalLight { float4 Direction; float4 Color; };
struct PointLight { float4 Position; float4 Color; float Intensity; float Range; float2 _pad; };
cbuffer PerFrame : register(b0) {
    float4x4 uLightVP;
    DirectionalLight uDir;
    float4 uCameraPos;
    float4 uFog;
    uint  uPointLightCount; float uAmbient; int uShadowEnabled; float uShadowBias;
    int   uFogEnabled; int3 _padL;
};
Texture2D uAlbedo   : register(t0);
Texture2D uNormal   : register(t1);
Texture2D uWorldPos : register(t2);
SamplerState uSamp  : register(s0);
StructuredBuffer<PointLight> gPointLights : register(t4);
Texture2D              uShadowMap  : register(t6);
SamplerComparisonState uShadowSamp : register(s7);
struct PSIn { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };

float ShadowFactor(float3 worldPos, float ndl){
    if (uShadowEnabled == 0) return 1.0;
    float4 lp = mul(uLightVP, float4(worldPos,1.0));
    float3 p = lp.xyz / lp.w;
    float2 uv = p.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    if (uv.x<0.0||uv.x>1.0||uv.y<0.0||uv.y>1.0) return 1.0;
    float bias = uShadowBias * (1.0 + (1.0-ndl)*2.0);
    float d = p.z - bias; float sum = 0.0; const float texel = 1.0/2048.0;
    [unroll] for(int y=-1;y<=1;++y)[unroll] for(int x=-1;x<=1;++x)
        sum += uShadowMap.SampleCmpLevelZero(uShadowSamp, uv+float2(x,y)*texel, d);
    return sum/9.0;
}

float4 main_ps(PSIn i) : SV_Target {
    float3 N  = uNormal.Sample(uSamp, i.UV).xyz;
    float3 wp = uWorldPos.Sample(uSamp, i.UV).xyz;
    float3 albedo = uAlbedo.Sample(uSamp, i.UV).rgb;
    if (dot(N,N) < 0.5) { return float4(uFog.rgb, 1.0); }   // sky / no geometry -> fog color
    N = normalize(N);
    float3 lightDir = normalize(uDir.Direction.xyz);
    float diffuse = max(dot(N, -lightDir), 0.0);
    float ambient = uAmbient;
    float3 lighting = ambient * uDir.Color.rgb;
    lighting += diffuse * uDir.Color.rgb * ShadowFactor(wp, diffuse);
    [loop] for (uint idx=0; idx<uPointLightCount; ++idx){
        PointLight pl = gPointLights[idx];
        float3 L = pl.Position.xyz - wp; float dist = length(L);
        if (pl.Range > 0.0001){
            float3 Ldir = L / max(dist,1e-5);
            float NdotL = max(dot(N,Ldir),0.0);
            float falloff = saturate(1.0 - (dist/pl.Range)*(dist/pl.Range));
            float contrib = pl.Intensity * NdotL * falloff;
            lighting += pl.Color.rgb * contrib;
        }
    }
    float3 col = albedo * lighting;
    if (uFogEnabled != 0){
        float fogDist = length(wp - uCameraPos.xyz);
        float fogF = 1.0 - exp(-uFog.w * fogDist);
        col = lerp(col, uFog.rgb, fogF);
    }
    return float4(col, 1.0);
}
)";

bool LightingRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer)
        return false;

    // Compile shaders.
    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, LIGHT_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel,  LIGHT_PS_HLSL, 0, "main_ps", "ps_6_1");
    if (!m_VS || !m_PS)
        return false;

    // G-buffer sampler: point/clamp is exact for a 1:1 full-screen sample.
    {
        nvrhi::SamplerDesc sd;
        sd.setAllFilters(false); // point
        sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        m_GBufSampler = m_Device->createSampler(sd);
    }

    // Binding layout: b0 (PerFrame), t0/t1/t2 (G-buffer albedo/normal/worldpos),
    // s0 (G-buffer sampler), t4 (point lights), t6 (shadow map), s7 (shadow sampler).
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Sampler(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),
        nvrhi::BindingLayoutItem::Texture_SRV(6),
        nvrhi::BindingLayoutItem::Sampler(7)
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
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(LightFrameCB), "LightingRenderPass FrameCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    // Point-light structured buffer (SRV), uploaded each frame.
    {
        nvrhi::BufferDesc bd;
        bd.debugName = "LightingRenderPass PointLights";
        bd.byteSize = m_MaxPointLights * sizeof(PointLightCPU);
        bd.structStride = sizeof(PointLightCPU);
        bd.initialState = nvrhi::ResourceStates::CopyDest;
        bd.isIndexBuffer = false;
        bd.isVertexBuffer = false;
        bd.isConstantBuffer = false;
        bd.canHaveUAVs = false;
        bd.keepInitialState = true;
        m_PointLightBuffer = m_Device->createBuffer(bd);
    }

    return true;
}

void LightingRenderPass::Render(nvrhi::ICommandList* commandList,
                                nvrhi::IFramebuffer* frameBuffer,
                                SimulationSnapshot& /*snapshot*/,
                                const ECS* world,
                                double /*deltaTime*/,
                                FrameAllocator* frameAllocator)
{
    if (!m_Renderer->GetGBufferFramebuffer())
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
        pso.renderState.depthStencilState.depthTestEnable = false;
        pso.renderState.depthStencilState.depthWriteEnable = false;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        nvrhi::BlendState::RenderTarget rt;
        rt.setBlendEnable(false)
          .setColorWriteMask(nvrhi::ColorMask::All);
        pso.renderState.blendState.setRenderTarget(0, rt);
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    commandList->beginMarker("LightingRenderPass");

    // Gather lights from the ECS, exactly like MeshRenderPass does.
    glm::vec4 lightningDirection(0.0f, -1.0f, 0.0f, 0.0f); // default arbitrary direction
    glm::vec4 lightningColor(0.0f);

    auto* pointLights = frameAllocator->AllocateArray<PointLightCPU>(m_MaxPointLights);
    uint32_t pointLightCount = 0;
    if (!pointLights)
        SM_WARN("LightingRenderPass: frame arena exhausted, point-light buffer not allocated");

    if (world)
    {
        world->Each<TransformComponent, LightningComponent>(
            [&](EntityId, const TransformComponent& transform, const LightningComponent& lightning)
        {
            if (lightning.Type == LightningType::Directional)
            {
                lightningDirection = lightning.Direction;
                lightningColor = lightning.Color;
            }
            else if (lightning.Type == LightningType::Point)
            {
                if (pointLights && pointLightCount < m_MaxPointLights)
                {
                    PointLightCPU pl{};
                    pl.Position = glm::vec4(transform.Position, 1.0f);
                    pl.Color = lightning.Color;
                    pl.Intensity = lightning.Intensity;
                    pl.Range = lightning.Range;
                    pointLights[pointLightCount++] = pl;
                }
            }
        });
    }

    // Fill the per-frame CB (matches MeshRenderPass's PerFrameCB values 1:1).
    LightFrameCB cb{};
    const CameraView& cam = m_Renderer->GetActiveCamera();
    cb.Dir.Direction = lightningDirection;
    cb.Dir.Color = lightningColor;
    cb.PointLightCount = pointLightCount;
    cb.Ambient = 0.1f; // hardcoded ambient for now (matches MeshRenderPass)
    const Renderer::ShadowView& sv = m_Renderer->GetShadowView();
    cb.LightVP = sv.LightVP;
    cb.ShadowEnabled = sv.Enabled;
    cb.ShadowBias = GetShadowSettings().Bias;
    const FogFrame& fog = m_Renderer->GetFrameFog();
    cb.CameraPos = glm::vec4(cam.Position, 1.0f);
    cb.Fog = glm::vec4(fog.Color, fog.Density);
    cb.FogEnabled = GetFogSettings().Enabled ? 1 : 0;
    commandList->writeBuffer(m_FrameCB, &cb, sizeof(cb));

    if (m_PointLightBuffer && pointLightCount > 0)
    {
        commandList->writeBuffer(m_PointLightBuffer, pointLights, pointLightCount * sizeof(PointLightCPU));
    }

    // Binding set: G-buffer SRVs + lights + shadow map (R32_FLOAT override matches MeshRenderPass).
    nvrhi::BindingSetDesc bindingDesc;
    bindingDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
        nvrhi::BindingSetItem::Texture_SRV(0, m_Renderer->GetGBufferAlbedo()),
        nvrhi::BindingSetItem::Texture_SRV(1, m_Renderer->GetGBufferNormal()),
        nvrhi::BindingSetItem::Texture_SRV(2, m_Renderer->GetGBufferWorldPos()),
        nvrhi::BindingSetItem::Sampler(0, m_GBufSampler),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_PointLightBuffer),
        nvrhi::BindingSetItem::Texture_SRV(6, m_Renderer->GetShadowDepthTexture(), nvrhi::Format::R32_FLOAT),
        nvrhi::BindingSetItem::Sampler(7, m_Renderer->GetShadowSampler())
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

void LightingRenderPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_BindingLayout = nullptr;
    m_GBufSampler = nullptr;
    m_FrameCB = nullptr;
    m_PointLightBuffer = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;
    m_Device = nullptr;
    m_Renderer = nullptr;
}

void LightingRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    m_Pipeline = nullptr;
}
