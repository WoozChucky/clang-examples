#include "SkinningComputePass.h"

#include "Renderer.h"
#include "MeshSystem.h"
#include "PaletteFrame.h"
#include "lib.h"
#include <nvrhi/utils.h>
#include <glm/mat4x4.hpp>
#include <algorithm>
#include <cstdint>
#include <vector>

// Compute skinning shader. Mirrors GBUF_SKINNED_VS_HLSL / SkinVertexCPU (Skinning.h) exactly:
// skin = sum_i weight_i * palette[paletteOffset + boneIndex_i]; pos = skin*v.Position, normal = (3x3 skin)*v.Normal.
// MeshVertex here MUST match the C++ MeshVertex (px,py,pz,nx,ny,nz,u,v = 8 floats = 32B) and the
// skinnedVB structStride. BoneVertex matches SkinnedVertex{uvec4 idx, vec4 weight} = 32B.
// Sentinel paletteOffset 0xFFFFFFFF = no palette range for this entity -> copy bind-pose vertex (no skin).
static const char* SKIN_CS_HLSL = R"(
struct MeshVertex { float3 Position; float3 Normal; float2 UV; };
struct BoneVertex { uint4 BoneIndices; float4 BoneWeights; };
StructuredBuffer<MeshVertex>   gIn      : register(t0);
StructuredBuffer<BoneVertex>   gBoneIn  : register(t1);
StructuredBuffer<float4x4>     gPalette : register(t2);
RWStructuredBuffer<MeshVertex> gOut     : register(u3);
cbuffer Params : register(b4) { uint gPaletteOffset; uint gOutputOffset; uint gVertexCount; uint _pad; };
[numthreads(64,1,1)]
void main_cs(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= gVertexCount) return;
    MeshVertex v = gIn[i];
    MeshVertex o = v;
    if (gPaletteOffset != 0xFFFFFFFFu) {
        BoneVertex b = gBoneIn[i];
        float4x4 skin =
            b.BoneWeights.x * gPalette[gPaletteOffset + b.BoneIndices.x] +
            b.BoneWeights.y * gPalette[gPaletteOffset + b.BoneIndices.y] +
            b.BoneWeights.z * gPalette[gPaletteOffset + b.BoneIndices.z] +
            b.BoneWeights.w * gPalette[gPaletteOffset + b.BoneIndices.w];
        o.Position = mul(skin, float4(v.Position, 1.0)).xyz;
        o.Normal   = mul((float3x3)skin, v.Normal);
    }
    gOut[gOutputOffset + i] = o;
}
)";

// Per-dispatch constants (16B). Mirrors the HLSL Params cbuffer.
struct SkinParamsCB {
    uint32_t PaletteOffset;
    uint32_t OutputOffset;
    uint32_t VertexCount;
    uint32_t _pad;
};
static_assert(sizeof(SkinParamsCB) == 16, "SkinParamsCB must be 16 bytes");

bool SkinningComputePass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer || !renderer->GetMeshSystem())
        return false;

    m_CS = m_Renderer->CreateShader(nvrhi::ShaderType::Compute, SKIN_CS_HLSL, 0, "main_cs", "cs_6_1");
    if (!m_CS)
        return false;

    // Binding layout: t0 in-VB SRV, t1 bone SRV, t2 palette SRV, u3 out-VB UAV, b4 volatile params CB.
    // NVRHI is flat (Vulkan): t/u/b share one number space and VulkanBindingOffsets stay 0, so the
    // HLSL register NUMBERS must be globally unique (0/1/2/3/4) — same convention as GBufferFillPass.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3),
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(4)
    };
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        layoutDesc.setBindingOffsets(nvrhi::VulkanBindingOffsets{}
            .setConstantBufferOffset(0)
            .setShaderResourceOffset(0)
            .setUnorderedAccessViewOffset(0)
            .setSamplerOffset(0));
    }
    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);
    if (!m_BindingLayout)
        return false;

    nvrhi::ComputePipelineDesc psoDesc;
    psoDesc.CS = m_CS;
    psoDesc.bindingLayouts = { m_BindingLayout };
    m_Pipeline = m_Device->createComputePipeline(psoDesc);
    if (!m_Pipeline)
        return false;

    // Volatile params CB: written per-dispatch in the command list (one per skinned entity).
    // maxVersions must cover (skinned entities per frame) * (frames in flight); 1024 is generous.
    m_ParamsCB = m_Device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(SkinParamsCB), "SkinningComputePass ParamsCB",
                                                       /*maxVersions=*/1024));
    if (!m_ParamsCB)
        return false;

    return true;
}

void SkinningComputePass::EnsurePaletteCapacity(uint32_t matrixCount)
{
    if (matrixCount <= m_PaletteCapacity && m_PaletteBuffer)
        return;
    uint32_t cap = m_PaletteCapacity ? m_PaletteCapacity : 64;
    while (cap < matrixCount) cap *= 2;

    nvrhi::BufferDesc pd;
    pd.debugName = "SkinningComputePass PaletteBuffer";
    pd.byteSize = static_cast<uint64_t>(sizeof(glm::mat4)) * cap;
    pd.structStride = sizeof(glm::mat4);
    pd.initialState = nvrhi::ResourceStates::CopyDest;
    pd.keepInitialState = true;
    m_PaletteBuffer = m_Device->createBuffer(pd);
    m_PaletteCapacity = cap;
}

void SkinningComputePass::EnsureSkinnedCapacity(uint32_t totalVertices)
{
    if (totalVertices <= m_SkinnedCapacity && m_SkinnedVB)
        return;
    uint32_t cap = m_SkinnedCapacity ? m_SkinnedCapacity : 4096;
    while (cap < totalVertices) cap *= 2;

    // Dual usage: written by the compute shader (UAV) then read by the raster path (VertexBuffer).
    // Tracked (keepInitialState), NOT permanent state, so it can flip UAV<->VertexBuffer per frame.
    nvrhi::BufferDesc bd;
    bd.debugName = "SkinningComputePass SkinnedVB";
    bd.byteSize = static_cast<uint64_t>(sizeof(MeshVertex)) * cap;
    bd.structStride = sizeof(MeshVertex);
    bd.isVertexBuffer = true;
    bd.canHaveUAVs = true;
    bd.initialState = nvrhi::ResourceStates::Common;
    bd.keepInitialState = true;
    m_SkinnedVB = m_Device->createBuffer(bd);
    m_SkinnedCapacity = cap;
}

int64_t SkinningComputePass::GetSkinnedVertexOffset(EntityId e) const
{
    auto it = m_OffsetByEntity.find(e);
    return (it != m_OffsetByEntity.end()) ? static_cast<int64_t>(it->second) : -1;
}

void SkinningComputePass::Execute(nvrhi::ICommandList* cl, const ECS* world, const PaletteFrame* palette)
{
    m_OffsetByEntity.clear();
    if (!cl || !world || !palette || palette->ranges.empty() || palette->matrices.empty())
        return;

    MeshSystem* meshSystem = m_Renderer->GetMeshSystem();
    if (!meshSystem)
        return;

    cl->beginMarker("SkinningComputePass");

    // Upload the bone palette to the StructuredBuffer<float4x4> SRV (moved here from GBuffer; the
    // compute pass is the natural owner). Runs BEFORE GBuffer's Execute, which now reads this buffer.
    const uint32_t matrixCount = static_cast<uint32_t>(palette->matrices.size());
    EnsurePaletteCapacity(matrixCount);
    cl->writeBuffer(m_PaletteBuffer, palette->matrices.data(),
                    static_cast<size_t>(sizeof(glm::mat4)) * matrixCount);

    // Plan output ranges: for each skinned entity, lay out its vertices contiguously in the shared
    // output VB. m_OffsetByEntity[entity] = base offset (in vertices). Accumulate total for capacity.
    struct Job {
        EntityId entity;
        uint32_t paletteOffset;
        uint32_t outputOffset;
        uint32_t vertexCount;
        nvrhi::IBuffer* inVB;
        nvrhi::IBuffer* boneVB;
    };
    std::vector<Job> jobs;
    jobs.reserve(palette->ranges.size());
    uint32_t outputOffset = 0;
    for (const auto& rg : palette->ranges)
    {
        const auto* meshComp = world->GetComponent<MeshComponent>(rg.entity);
        if (!meshComp)
            continue;
        auto res = meshSystem->GetMeshResources(meshComp->MeshId);
        if (!res.valid || !res.isSkinned || !res.boneBuffer || !res.vertexBuffer || res.vertexCount == 0)
            continue;

        const uint32_t base = outputOffset;
        m_OffsetByEntity[rg.entity] = base;
        jobs.push_back(Job{ rg.entity, rg.offset, base, res.vertexCount,
                            res.vertexBuffer, res.boneBuffer });
        outputOffset += res.vertexCount;
    }

    if (jobs.empty())
    {
        cl->endMarker();
        return;
    }

    EnsureSkinnedCapacity(outputOffset);

    // Output VB -> UnorderedAccess for the compute writes.
    cl->setBufferState(m_SkinnedVB, nvrhi::ResourceStates::UnorderedAccess);

    for (const auto& job : jobs)
    {
        // Inputs -> ShaderResource (tracked skinned VB / bone buffer flip to SRV for compute).
        cl->setBufferState(job.inVB, nvrhi::ResourceStates::ShaderResource);
        cl->setBufferState(job.boneVB, nvrhi::ResourceStates::ShaderResource);

        SkinParamsCB params{ job.paletteOffset, job.outputOffset, job.vertexCount, 0u };
        cl->writeBuffer(m_ParamsCB, &params, sizeof(params));

        nvrhi::BindingSetDesc bsd;
        bsd.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, job.inVB),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(1, job.boneVB),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_PaletteBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_SkinnedVB),
            nvrhi::BindingSetItem::ConstantBuffer(4, m_ParamsCB)
        };
        nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bsd, m_BindingLayout);

        nvrhi::ComputeState state;
        state.pipeline = m_Pipeline;
        state.bindings = { bindingSet };
        cl->setComputeState(state); // commits the pending buffer-state barriers

        cl->dispatch((job.vertexCount + 63u) / 64u, 1, 1);
    }

    // Barrier UAV -> VertexBuffer so a later raster pass can read the skinned VB (shadow + g-buffer read this skinned VB as a vertex buffer).
    cl->setBufferState(m_SkinnedVB, nvrhi::ResourceStates::VertexBuffer);

    cl->endMarker();
}

void SkinningComputePass::DestroyGpuResources()
{
    m_Pipeline = nullptr;
    m_BindingLayout = nullptr;
    m_CS = nullptr;
    m_SkinnedVB = nullptr;
    m_PaletteBuffer = nullptr;
    m_ParamsCB = nullptr;
    m_SkinnedCapacity = 0;
    m_PaletteCapacity = 0;
    m_OffsetByEntity.clear();
    m_Device = nullptr;
    m_Renderer = nullptr;
}

bool SkinningComputePass::RecreateGpuResources(nvrhi::IDevice* device, Renderer* renderer)
{
    // Buffers are per-frame transient (rebuilt lazily by Ensure*Capacity in Execute), so a swap
    // just needs to rebuild the shader/pipeline/layout/CB against the new device.
    return Initialize(device, renderer);
}
