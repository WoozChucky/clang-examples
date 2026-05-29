#include "SsaoRenderPass.h"

#include "Renderer.h"
#include <nvrhi/utils.h>
#include <string>
#include <glm/glm.hpp>

#include "SsaoMath.h"
#include "RenderStats.h"
#include "lib.h"  // SM_ERROR

// CB shared by both sub-passes. 16-byte aligned; matches the cbuffer in the HLSL below.
struct SsaoCB {
    glm::mat4 ViewProj;   // world -> clip
    glm::mat4 View;       // world -> view (for view-space Z compare)
    glm::vec4 Kernel[16]; // xyz = tangent-space hemisphere sample
    glm::vec4 Params;     // x=Radius, y=Bias, z=Intensity, w=Power
    glm::vec4 RtSize;     // x=w, y=h, z=1/w, w=1/h
};
static_assert(sizeof(SsaoCB) % 16 == 0, "SsaoCB must be 16-byte aligned");

namespace {
// Full-screen-triangle VS (shared by both sub-passes): SV_VertexID -> UV + clip pos.
static const char* FULLSCREEN_VS = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 uv:TEXCOORD0; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o; o.uv = float2((vid<<1)&2, vid&2);
    o.PosH = float4(o.uv*float2(2,-2)+float2(-1,1),0,1);
    return o;
}
)";

// AO pixel shader: reads G-buffer world normal/pos, samples a tangent-space hemisphere kernel
// (rotated per pixel), accumulates range-weighted occlusion against the stored world positions.
static const char* AO_PS = R"(
Texture2D uNormal   : register(t1);
Texture2D uWorldPos : register(t2);
SamplerState uPt    : register(s3);
cbuffer SsaoCB : register(b0) {
    float4x4 uViewProj; float4x4 uView; float4 uKernel[16]; float4 uParams; float4 uRtSize;
};
struct PSIn { float4 PosH:SV_POSITION; float2 uv:TEXCOORD0; };
float4 main_ps(PSIn i):SV_Target {
    float2 uv = i.uv;
    float3 N = uNormal.Sample(uPt, uv).xyz;
    if (dot(N,N) < 0.5) return float4(1,0,0,0); // sky / unwritten -> no occlusion
    N = normalize(N);
    float3 P = uWorldPos.Sample(uPt, uv).xyz;

    // Per-pixel rotation angle (interleaved hash) to break up banding.
    float a = frac(sin(dot(uv*uRtSize.xy, float2(12.9898,78.233)))*43758.5453)*6.28318530718;

    float3 up = abs(N.y) < 0.99 ? float3(0,1,0) : float3(1,0,0);
    float3 T0 = normalize(cross(up, N));
    float3 T  = T0*cos(a) + cross(N,T0)*sin(a); // rotate T0 around N by angle a
    float3 B  = cross(N, T);

    float radius = uParams.x;
    float bias   = uParams.y;
    float occ = 0;
    [loop] for (int s = 0; s < 16; ++s) {
        float3 sp = P + (T*uKernel[s].x + B*uKernel[s].y + N*uKernel[s].z)*radius;
        float4 clip = mul(uViewProj, float4(sp,1));
        if (clip.w <= 1e-5) continue;
        float3 ndc = clip.xyz/clip.w;
        float2 uvs = ndc.xy*0.5+0.5; uvs.y = 1-uvs.y;
        if (any(uvs<0)||any(uvs>1)) continue;
        float3 Pocc = uWorldPos.Sample(uPt, uvs).xyz;
        float occZ = mul(uView, float4(Pocc,1)).z;
        float sZ   = mul(uView, float4(sp,1)).z;
        float range = saturate(1.0 - length(Pocc - P)/radius);
        if (occZ > sZ + bias) occ += range; // view Z negative (RH): closer = larger Z = occluded
    }
    occ = (occ/16.0)*uParams.z;
    float ao = pow(saturate(1.0 - occ), uParams.w);
    return float4(ao,0,0,0);
}
)";

// Blur pixel shader: 4x4 box over the raw AO. Reuses the SsaoCB; only uRtSize.zw (1/size) is used.
static const char* BLUR_PS = R"(
Texture2D uAo    : register(t1);
SamplerState uPt : register(s3);
cbuffer SsaoCB : register(b0) {
    float4x4 uViewProj; float4x4 uView; float4 uKernel[16]; float4 uParams; float4 uRtSize;
};
struct PSIn { float4 PosH:SV_POSITION; float2 uv:TEXCOORD0; };
float4 main_ps(PSIn i):SV_Target {
    const float offs[4] = { -1.5, -0.5, 0.5, 1.5 };
    float sum = 0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            sum += uAo.Sample(uPt, i.uv + float2(offs[x], offs[y])*uRtSize.zw).r;
    return float4(sum/16.0, 0,0,0);
}
)";

static std::string Compose(const char* stage) {
    std::string s; s.reserve(8 * 1024);
    s += FULLSCREEN_VS; s += stage; return s;
}
} // namespace

bool SsaoRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device; m_Renderer = renderer;
    if (!m_Device || !m_Renderer) return false;

    m_Kernel = MakeHemisphereKernel();

    auto mk = [&](nvrhi::ShaderType t, const char* stage, const char* entry, const char* prof) {
        const std::string src = Compose(stage);
        return m_Renderer->CreateShader(t, src.c_str(), src.size(), entry, prof);
    };
    m_AoVS   = mk(nvrhi::ShaderType::Vertex, "",     "main_vs", "vs_6_1"); // VS-only compose
    m_AoPS   = mk(nvrhi::ShaderType::Pixel,  AO_PS,  "main_ps", "ps_6_1");
    m_BlurVS = mk(nvrhi::ShaderType::Vertex, "",     "main_vs", "vs_6_1");
    m_BlurPS = mk(nvrhi::ShaderType::Pixel,  BLUR_PS,"main_ps", "ps_6_1");
    if (!m_AoVS || !m_AoPS || !m_BlurVS || !m_BlurPS) {
        SM_ERROR("SsaoRenderPass: shader compilation failed");
        return false;
    }

    { nvrhi::SamplerDesc sd; sd.setAllFilters(false); sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp); m_PointClamp = m_Device->createSampler(sd); }
    if (!m_PointClamp) { SM_ERROR("SsaoRenderPass: sampler creation failed"); return false; }

    auto makeLayout = [&](std::vector<nvrhi::BindingLayoutItem> items) {
        nvrhi::BindingLayoutDesc d; d.visibility = nvrhi::ShaderType::All; d.bindings = std::move(items);
        if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            d.setBindingOffsets(nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0));
        return m_Device->createBindingLayout(d);
    };
    m_AoLayout   = makeLayout({ nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Sampler(3) });
    m_BlurLayout = makeLayout({ nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Sampler(3) });
    if (!m_AoLayout || !m_BlurLayout) { SM_ERROR("SsaoRenderPass: binding layout creation failed"); return false; }

    m_CB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(SsaoCB), "SsaoRenderPass CB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));
    if (!m_CB) { SM_ERROR("SsaoRenderPass: CB creation failed"); return false; }

    return true;
}

void SsaoRenderPass::Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* /*frameBuffer*/,
                            SimulationSnapshot& /*snapshot*/,
                            const ECS* /*world*/,
                            double /*deltaTime*/,
                            FrameAllocator* /*frameAllocator*/)
{
    const SsaoSettings& s = GetSsaoSettings();
    if (!s.Enabled) return;

    nvrhi::IFramebuffer* rawFb  = m_Renderer->GetSsaoRawFramebuffer();
    nvrhi::IFramebuffer* blurFb = m_Renderer->GetSsaoBlurFramebuffer();
    if (!rawFb || !blurFb) return;

    nvrhi::ITexture* gN = m_Renderer->GetGBufferNormal();
    nvrhi::ITexture* gP = m_Renderer->GetGBufferWorldPos();
    if (!gN || !gP) return;

    // Lazy pipelines (rebuilt on resize): no depth, no cull, no blend, full-screen triangle.
    auto makePipe = [&](nvrhi::IShader* vs, nvrhi::IShader* ps, nvrhi::IBindingLayout* layout, nvrhi::IFramebuffer* fb) {
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = vs; pso.PS = ps; pso.bindingLayouts = { layout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        pso.renderState.depthStencilState.depthTestEnable = false;
        pso.renderState.depthStencilState.depthWriteEnable = false;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        nvrhi::BlendState::RenderTarget rt; rt.setBlendEnable(false).setColorWriteMask(nvrhi::ColorMask::All);
        pso.renderState.blendState.setRenderTarget(0, rt);
        return m_Device->createGraphicsPipeline(pso, fb->getFramebufferInfo());
    };
    if (!m_AoPipeline)   m_AoPipeline   = makePipe(m_AoVS,   m_AoPS,   m_AoLayout,   rawFb);
    if (!m_BlurPipeline) m_BlurPipeline = makePipe(m_BlurVS, m_BlurPS, m_BlurLayout, blurFb);
    if (!m_AoPipeline || !m_BlurPipeline) return;

    const auto rawFbi = rawFb->getFramebufferInfo();

    SsaoCB cb{};
    const CameraView& cam = m_Renderer->GetActiveCamera();
    cb.ViewProj = cam.Projection * cam.View;
    cb.View     = cam.View;
    for (int i = 0; i < SsaoKernel::Count; ++i)
        cb.Kernel[i] = glm::vec4(m_Kernel.Samples[i], 0.0f);
    cb.Params = glm::vec4(s.Radius, s.Bias, s.Intensity, s.Power);
    cb.RtSize = glm::vec4((float)rawFbi.width, (float)rawFbi.height,
                          1.0f / (float)rawFbi.width, 1.0f / (float)rawFbi.height);

    commandList->beginMarker("SsaoRenderPass");
    commandList->writeBuffer(m_CB, &cb, sizeof(cb));

    auto draw = [&](nvrhi::IGraphicsPipeline* pipe, nvrhi::IFramebuffer* fb, nvrhi::BindingSetHandle bs) {
        nvrhi::GraphicsState st; st.pipeline = pipe; st.framebuffer = fb; st.bindings = { bs };
        st.viewport.addViewportAndScissorRect(fb->getFramebufferInfo().getViewport());
        commandList->setGraphicsState(st);
        nvrhi::DrawArguments a; a.vertexCount = 3; commandList->draw(a);
    };

    // AO sub-pass: G-buffer normal + world-pos -> raw AO.
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_CB),
            nvrhi::BindingSetItem::Texture_SRV(1, gN),
            nvrhi::BindingSetItem::Texture_SRV(2, gP),
            nvrhi::BindingSetItem::Sampler(3, m_PointClamp) };
        draw(m_AoPipeline, rawFb, m_Device->createBindingSet(d, m_AoLayout));
    }
    // Blur sub-pass: raw AO -> blurred AO.
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_CB),
            nvrhi::BindingSetItem::Texture_SRV(1, m_Renderer->GetSsaoRawTexture()),
            nvrhi::BindingSetItem::Sampler(3, m_PointClamp) };
        draw(m_BlurPipeline, blurFb, m_Device->createBindingSet(d, m_BlurLayout));
    }

    commandList->endMarker();
}

void SsaoRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    // Targets are Renderer-owned (rebuilt by EnsureSsao); just drop the pipelines so they
    // rebind against the new framebuffers.
    m_AoPipeline = nullptr;
    m_BlurPipeline = nullptr;
}

void SsaoRenderPass::Shutdown()
{
    m_AoPipeline = m_BlurPipeline = nullptr;
    m_AoLayout = m_BlurLayout = nullptr;
    m_CB = nullptr;
    m_PointClamp = nullptr;
    m_AoVS = m_AoPS = m_BlurVS = m_BlurPS = nullptr;
    m_Device = nullptr; m_Renderer = nullptr;
}
