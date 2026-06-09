#include "SsaoRenderPass.h"

#include "Renderer.h"
#include <nvrhi/utils.h>
#include <cstring>
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
// AO compute shader: thread -> pixel -> UV; reads G-buffer world normal/pos, samples a
// tangent-space hemisphere kernel (rotated per pixel via the 4x4 tile), accumulates
// range-weighted occlusion against the stored world positions, and writes a UAV. AO math ported
// verbatim from the previous AO_PS (point sampler kept for the reprojected uWorldPos lookups).
static const char* AO_CS = R"(
Texture2D       uNormal   : register(t1);
Texture2D       uWorldPos : register(t2);
SamplerState    uPt       : register(s3);
RWTexture2D<float> uOut    : register(u4);
cbuffer SsaoCB : register(b0) {
    float4x4 uViewProj; float4x4 uView; float4 uKernel[16]; float4 uParams; float4 uRtSize;
};
[numthreads(8,8,1)]
void main_cs(uint3 tid : SV_DispatchThreadID) {
    uint2 px = tid.xy;
    if (px.x >= (uint)uRtSize.x || px.y >= (uint)uRtSize.y) return;
    float2 uv = (float2(px) + 0.5) * uRtSize.zw;
    float3 N = uNormal.SampleLevel(uPt, uv, 0).xyz;
    if (dot(N,N) < 0.5) { uOut[px] = 1.0; return; }
    N = normalize(N);
    float3 P = uWorldPos.SampleLevel(uPt, uv, 0).xyz;
    float2 fpx  = floor(uv * uRtSize.xy);
    float2 cell = fmod(fpx, 4.0);
    float  idx  = cell.y * 4.0 + cell.x;
    float  a    = frac(sin(idx * 12.9898) * 43758.5453) * 6.28318530718;
    float3 up = abs(N.y) < 0.99 ? float3(0,1,0) : float3(1,0,0);
    float3 T0 = normalize(cross(up, N));
    float3 T  = T0*cos(a) + cross(N,T0)*sin(a);
    float3 B  = cross(N, T);
    float radius = uParams.x; float bias = uParams.y;
    float occ = 0;
    [loop] for (int s = 0; s < 16; ++s) {
        float3 sp = P + (T*uKernel[s].x + B*uKernel[s].y + N*uKernel[s].z)*radius;
        float4 clip = mul(uViewProj, float4(sp,1));
        if (clip.w <= 1e-5) continue;
        float3 ndc = clip.xyz/clip.w;
        float2 uvs = ndc.xy*0.5+0.5; uvs.y = 1-uvs.y;
        if (any(uvs<0)||any(uvs>1)) continue;
        float3 Pocc = uWorldPos.SampleLevel(uPt, uvs, 0).xyz;
        float occZ = mul(uView, float4(Pocc,1)).z;
        float sZ   = mul(uView, float4(sp,1)).z;
        float range = saturate(1.0 - length(Pocc - P)/radius);
        if (occZ > sZ + bias) occ += range;
    }
    occ = (occ/16.0)*uParams.z;
    uOut[px] = pow(saturate(1.0 - occ), uParams.w);
}
)";

// HBAO compute shader: horizon-based AO. Same bindings as AO_CS (b0/t1/t2/s3/u4 -> m_AoLayout).
// Marches DIRS directions x STEPS steps in WORLD space, reprojecting each sample via uViewProj
// (matching AO_CS); accumulates the max view-space horizon sine per direction, range-weighted.
static const char* HBAO_CS = R"(
Texture2D       uNormal   : register(t1);
Texture2D       uWorldPos : register(t2);
SamplerState    uPt       : register(s3);
RWTexture2D<float> uOut    : register(u4);
cbuffer SsaoCB : register(b0) {
    float4x4 uViewProj; float4x4 uView; float4 uKernel[16]; float4 uParams; float4 uRtSize;
};
#define DIRS 4
#define STEPS 8
#define PI 3.14159265
[numthreads(8,8,1)]
void main_cs(uint3 tid : SV_DispatchThreadID) {
    uint2 px = tid.xy;
    if (px.x >= (uint)uRtSize.x || px.y >= (uint)uRtSize.y) return;
    float2 uv = (float2(px) + 0.5) * uRtSize.zw;
    float3 Nw = uNormal.SampleLevel(uPt, uv, 0).xyz;
    if (dot(Nw,Nw) < 0.5) { uOut[px] = 1.0; return; }
    Nw = normalize(Nw);
    float3 Pw = uWorldPos.SampleLevel(uPt, uv, 0).xyz;
    float3 Pv = mul(uView, float4(Pw,1)).xyz;
    float3 Nv = normalize(mul((float3x3)uView, Nw));
    float radius = uParams.x, intensity = uParams.z, power = uParams.w;
    float3 up = abs(Nw.y) < 0.99 ? float3(0,1,0) : float3(1,0,0);
    float3 T0 = normalize(cross(up, Nw));
    float3 B0 = cross(Nw, T0);
    float2 fpx = floor(uv * uRtSize.xy); float2 cell = fmod(fpx, 4.0);
    float jit = frac(sin((cell.y*4.0 + cell.x) * 12.9898) * 43758.5453);
    float occ = 0.0;
    [loop] for (int d = 0; d < DIRS; ++d) {
        float ang = (float(d) + jit) * (PI / DIRS);
        float3 dir = T0 * cos(ang) + B0 * sin(ang);
        float maxSin = 0.0;
        [loop] for (int s = 1; s <= STEPS; ++s) {
            float3 sw = Pw + dir * (radius * (float(s) / STEPS));
            float4 clip = mul(uViewProj, float4(sw,1));
            if (clip.w <= 1e-5) continue;
            float2 suv = clip.xy/clip.w * 0.5 + 0.5; suv.y = 1 - suv.y;
            if (any(suv < 0) || any(suv > 1)) continue;
            float3 Ow = uWorldPos.SampleLevel(uPt, suv, 0).xyz;
            float3 Ov = mul(uView, float4(Ow,1)).xyz;
            float3 h  = Ov - Pv;
            float  len = length(h);
            if (len < 1e-4 || len > radius) continue;
            float sinH = dot(normalize(h), Nv);
            float w = saturate(1.0 - len / radius);
            maxSin = max(maxSin, sinH * w);
        }
        occ += maxSin;
    }
    occ = (occ / DIRS) * intensity;
    uOut[px] = pow(saturate(1.0 - occ), power);
}
)";

// Blur compute shader: STRAIGHT 4x4 box over the raw AO (LDS optimization comes in Task 3). The
// integer offsets { -1, 0, 1, 2 } reproduce the previous PS's point-sampled {-1.5,-0.5,0.5,1.5}
// texel box (the half-texel offsets land in the same source texels under point sampling).
static const char* BLUR_CS = R"(
Texture2D<float>   uAo  : register(t1);
RWTexture2D<float> uOut : register(u2);
cbuffer SsaoCB : register(b0) {
    float4x4 uViewProj; float4x4 uView; float4 uKernel[16]; float4 uParams; float4 uRtSize;
};
#define TILE 8
#define APRON_LO 1
#define APRON_HI 2
#define SPAN (TILE + APRON_LO + APRON_HI)   // 11
groupshared float gTile[SPAN][SPAN];
[numthreads(8,8,1)]
void main_cs(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID, uint3 dtid : SV_DispatchThreadID) {
    int W = (int)uRtSize.x, H = (int)uRtSize.y;
    int2 groupBase = int2(gid.xy) * TILE;                  // top-left pixel of this group's tile
    // Cooperatively load SPAN*SPAN texels (origin = groupBase - APRON_LO) into LDS, each fetched once.
    for (int t = int(gtid.y) * TILE + int(gtid.x); t < SPAN*SPAN; t += TILE*TILE) {
        int lx = t % SPAN, ly = t / SPAN;
        int2 q = clamp(groupBase - APRON_LO + int2(lx, ly), int2(0,0), int2(W-1,H-1));
        gTile[ly][lx] = uAo.Load(int3(q,0));
    }
    GroupMemoryBarrierWithGroupSync();
    int2 px = int2(dtid.xy);
    if (px.x >= W || px.y >= H) return;
    // This pixel's index inside the tile = gtid + APRON_LO. Box offsets {-1,0,1,2} (== T2 straight box).
    const int offs[4] = { -1, 0, 1, 2 };
    int lx = int(gtid.x) + APRON_LO, ly = int(gtid.y) + APRON_LO;
    float sum = 0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            sum += gTile[ly + offs[y]][lx + offs[x]];
    uOut[px] = sum / 16.0;
}
)";
} // namespace

bool SsaoRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device; m_Renderer = renderer;
    if (!m_Device || !m_Renderer) return false;

    m_Kernel = MakeHemisphereKernel();

    m_AoCS   = m_Renderer->CreateShader(nvrhi::ShaderType::Compute, AO_CS,   strlen(AO_CS),   "main_cs", "cs_6_1");
    m_BlurCS = m_Renderer->CreateShader(nvrhi::ShaderType::Compute, BLUR_CS, strlen(BLUR_CS), "main_cs", "cs_6_1");
    if (!m_AoCS || !m_BlurCS) {
        SM_ERROR("SsaoRenderPass: shader compilation failed");
        return false;
    }

    m_HbaoCS = m_Renderer->CreateShader(nvrhi::ShaderType::Compute, HBAO_CS, strlen(HBAO_CS), "main_cs", "cs_6_1");
    if (!m_HbaoCS) { SM_ERROR("SsaoRenderPass: HBAO shader compilation failed"); return false; }

    { nvrhi::SamplerDesc sd; sd.setAllFilters(false); sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp); m_PointClamp = m_Device->createSampler(sd); }
    if (!m_PointClamp) { SM_ERROR("SsaoRenderPass: sampler creation failed"); return false; }

    // Compute binding layouts. NVRHI is flat (Vulkan): t/u/b/s share one number space and
    // VulkanBindingOffsets stay 0, so the HLSL register NUMBERS are globally unique per layout.
    auto makeLayout = [&](std::vector<nvrhi::BindingLayoutItem> items) {
        nvrhi::BindingLayoutDesc d; d.visibility = nvrhi::ShaderType::Compute; d.bindings = std::move(items);
        if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            d.setBindingOffsets(nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setUnorderedAccessViewOffset(0).setSamplerOffset(0));
        return m_Device->createBindingLayout(d);
    };
    m_AoLayout   = makeLayout({ nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Sampler(3), nvrhi::BindingLayoutItem::Texture_UAV(4) });
    m_BlurLayout = makeLayout({ nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_UAV(2) });
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
    if (s.Mode == AoMode::Off) return;

    nvrhi::ITexture* gN   = m_Renderer->GetGBufferNormal();
    nvrhi::ITexture* gP   = m_Renderer->GetGBufferWorldPos();
    nvrhi::ITexture* raw  = m_Renderer->GetSsaoRawTexture();
    nvrhi::ITexture* blur = m_Renderer->GetSsaoTexture();
    if (!gN || !gP || !raw || !blur) return;

    // Lazy compute pipelines (dropped + rebuilt on resize like the old graphics ones).
    if (!m_AoPipeline)   m_AoPipeline   = m_Device->createComputePipeline(nvrhi::ComputePipelineDesc{}.setComputeShader(m_AoCS).addBindingLayout(m_AoLayout));
    if (!m_BlurPipeline) m_BlurPipeline = m_Device->createComputePipeline(nvrhi::ComputePipelineDesc{}.setComputeShader(m_BlurCS).addBindingLayout(m_BlurLayout));
    if (!m_AoPipeline || !m_BlurPipeline) return;

    if (s.Mode == AoMode::HBAO && !m_HbaoPipeline)
        m_HbaoPipeline = m_Device->createComputePipeline(nvrhi::ComputePipelineDesc{}.setComputeShader(m_HbaoCS).addBindingLayout(m_AoLayout));
    nvrhi::IComputePipeline* aoPipe = m_AoPipeline;
    if (s.Mode == AoMode::HBAO && m_HbaoPipeline) aoPipe = m_HbaoPipeline;

    const uint32_t W = raw->getDesc().width, H = raw->getDesc().height;
    const uint32_t gx = (W + 7) / 8, gy = (H + 7) / 8;

    SsaoCB cb{};
    const CameraView& cam = m_Renderer->GetActiveCamera();
    cb.ViewProj = cam.Projection * cam.View;
    cb.View     = cam.View;
    for (int i = 0; i < SsaoKernel::Count; ++i)
        cb.Kernel[i] = glm::vec4(m_Kernel.Samples[i], 0.0f);
    cb.Params = glm::vec4(s.Radius, s.Bias, s.Intensity, s.Power);
    cb.RtSize = glm::vec4((float)W, (float)H, 1.0f / (float)W, 1.0f / (float)H);

    commandList->beginMarker("SsaoRenderPass");
    commandList->writeBuffer(m_CB, &cb, sizeof(cb));

    // AO dispatch: G-buffer normal + world-pos -> raw AO (UAV write).
    commandList->setTextureState(raw, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_CB),
            nvrhi::BindingSetItem::Texture_SRV(1, gN),
            nvrhi::BindingSetItem::Texture_SRV(2, gP),
            nvrhi::BindingSetItem::Sampler(3, m_PointClamp),
            nvrhi::BindingSetItem::Texture_UAV(4, raw) };
        // keep the binding-set handle alive through setComputeState (st.bindings holds only a raw ptr)
        nvrhi::BindingSetHandle bs = m_Device->createBindingSet(d, m_AoLayout);
        nvrhi::ComputeState st; st.pipeline = aoPipe; st.bindings = { bs };
        commandList->setComputeState(st); // commits the pending texture-state barrier
        commandList->dispatch(gx, gy, 1);
    }

    // Blur dispatch: raw AO (now SRV) -> blurred AO (UAV write).
    commandList->setTextureState(raw,  nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(blur, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_CB),
            nvrhi::BindingSetItem::Texture_SRV(1, raw),
            nvrhi::BindingSetItem::Texture_UAV(2, blur) };
        // keep the binding-set handle alive through setComputeState (st.bindings holds only a raw ptr)
        nvrhi::BindingSetHandle bs = m_Device->createBindingSet(d, m_BlurLayout);
        nvrhi::ComputeState st; st.pipeline = m_BlurPipeline; st.bindings = { bs };
        commandList->setComputeState(st);
        commandList->dispatch(gx, gy, 1);
    }

    // Blur output -> SRV so the lighting pass can sample it.
    commandList->setTextureState(blur, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->endMarker();
}

void SsaoRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    // Targets are Renderer-owned (rebuilt by EnsureSsao); just drop the pipelines so the
    // compute pipelines rebind against the resized textures.
    m_AoPipeline = nullptr;
    m_BlurPipeline = nullptr;
    m_HbaoPipeline = nullptr;
}

void SsaoRenderPass::Shutdown()
{
    m_AoPipeline = m_BlurPipeline = nullptr;
    m_HbaoPipeline = nullptr;
    m_AoLayout = m_BlurLayout = nullptr;
    m_CB = nullptr;
    m_PointClamp = nullptr;
    m_AoCS = m_BlurCS = nullptr;
    m_HbaoCS = nullptr;
    m_Device = nullptr; m_Renderer = nullptr;
}
