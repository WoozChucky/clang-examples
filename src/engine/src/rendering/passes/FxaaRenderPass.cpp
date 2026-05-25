#include "FxaaRenderPass.h"

#include "Renderer.h"
#include <nvrhi/utils.h>

// Full-screen FXAA 3.11 (luma-edge). VS emits a full-screen triangle from
// SV_VertexID; PS samples the scene color, detects high-contrast edges by luma
// range, and blends along the edge direction. Registers are globally unique
// across b/t/s (b0 CB, t1 scene, s2 sampler) so Vulkan flat-binding offsets
// don't collide, matching LightingRenderPass.
static const char* FXAA_VS_HLSL = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o;
    o.UV = float2((vid << 1) & 2, vid & 2);
    o.PosH = float4(o.UV * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}
)";

static const char* FXAA_PS_HLSL = R"(
cbuffer FxaaCB : register(b0) {
    float2 uRcpFrame;   // (1/width, 1/height)
    float2 _pad;
};
Texture2D    uScene : register(t1);
SamplerState uSamp  : register(s2);

struct PSIn { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };

float FxaaLuma(float3 c){ return dot(c, float3(0.299, 0.587, 0.114)); }

float4 main_ps(PSIn i) : SV_Target {
    float2 uv = i.UV;
    float3 rgbM  = uScene.Sample(uSamp, uv).rgb;
    float3 rgbNW = uScene.Sample(uSamp, uv + float2(-1.0,-1.0) * uRcpFrame).rgb;
    float3 rgbNE = uScene.Sample(uSamp, uv + float2( 1.0,-1.0) * uRcpFrame).rgb;
    float3 rgbSW = uScene.Sample(uSamp, uv + float2(-1.0, 1.0) * uRcpFrame).rgb;
    float3 rgbSE = uScene.Sample(uSamp, uv + float2( 1.0, 1.0) * uRcpFrame).rgb;

    float lM  = FxaaLuma(rgbM);
    float lNW = FxaaLuma(rgbNW);
    float lNE = FxaaLuma(rgbNE);
    float lSW = FxaaLuma(rgbSW);
    float lSE = FxaaLuma(rgbSE);

    float lMin = min(lM, min(min(lNW,lNE), min(lSW,lSE)));
    float lMax = max(lM, max(max(lNW,lNE), max(lSW,lSE)));

    const float EDGE_THRESHOLD_MIN = 0.0312;
    const float EDGE_THRESHOLD     = 0.125;
    float range = lMax - lMin;
    if (range < max(EDGE_THRESHOLD_MIN, lMax * EDGE_THRESHOLD)) {
        return float4(rgbM, 1.0);          // flat region: no AA
    }

    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));

    const float REDUCE_MUL = 1.0/8.0;
    const float REDUCE_MIN = 1.0/128.0;
    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * REDUCE_MUL, REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    const float SPAN_MAX = 8.0;
    dir = clamp(dir * rcpDirMin, -SPAN_MAX, SPAN_MAX) * uRcpFrame;

    float3 rgbA = 0.5 * (
        uScene.Sample(uSamp, uv + dir * (1.0/3.0 - 0.5)).rgb +
        uScene.Sample(uSamp, uv + dir * (2.0/3.0 - 0.5)).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        uScene.Sample(uSamp, uv + dir * -0.5).rgb +
        uScene.Sample(uSamp, uv + dir *  0.5).rgb);

    float lB = FxaaLuma(rgbB);
    if (lB < lMin || lB > lMax) return float4(rgbA, 1.0);
    return float4(rgbB, 1.0);
}
)";

bool FxaaRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer)
        return false;

    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, FXAA_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel,  FXAA_PS_HLSL, 0, "main_ps", "ps_6_1");
    if (!m_VS || !m_PS)
        return false;

    // Bilinear clamp sampler for the sub-pixel edge taps.
    {
        nvrhi::SamplerDesc sd;
        sd.setAllFilters(true); // linear
        sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        m_Sampler = m_Device->createSampler(sd);
    }

    // Binding layout: b0 (CB), t1 (scene color), s2 (sampler). Unique slots so the
    // Vulkan flat-binding offsets don't collide (same approach as the other passes).
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Sampler(2)
    };
    nvrhi::VulkanBindingOffsets& offsets =
        nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0);

    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        layoutDesc.setBindingOffsets(offsets);
    }

    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    m_FrameCB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(FxaaFrameCB), "FxaaRenderPass FrameCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    return true;
}

void FxaaRenderPass::Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* frameBuffer,
                            SimulationSnapshot& /*snapshot*/,
                            const ECS* /*world*/,
                            double /*deltaTime*/,
                            FrameAllocator* /*frameAllocator*/)
{
    nvrhi::ITexture* scene = m_Renderer->GetSceneColorTexture();
    if (!scene)
        return; // nothing to resolve (FXAA off path never calls this)

    if (!m_Pipeline)
    {
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = m_PS;
        // No input layout: full-screen triangle from SV_VertexID.
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

    commandList->beginMarker("FxaaRenderPass");

    const auto fbi = frameBuffer->getFramebufferInfo();
    FxaaFrameCB cb{};
    cb.RcpFrame = glm::vec2(1.0f / static_cast<float>(fbi.width),
                            1.0f / static_cast<float>(fbi.height));
    commandList->writeBuffer(m_FrameCB, &cb, sizeof(cb));

    nvrhi::BindingSetDesc bindingDesc;
    bindingDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
        nvrhi::BindingSetItem::Texture_SRV(1, scene),
        nvrhi::BindingSetItem::Sampler(2, m_Sampler)
    };
    nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = frameBuffer;
    state.bindings = { bindingSet };
    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
    commandList->setGraphicsState(state);

    nvrhi::DrawArguments a;
    a.vertexCount = 3;
    commandList->draw(a);

    commandList->endMarker();
}

void FxaaRenderPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_BindingLayout = nullptr;
    m_Sampler = nullptr;
    m_FrameCB = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;
    m_Device = nullptr;
    m_Renderer = nullptr;
}

void FxaaRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    m_Pipeline = nullptr;
}
