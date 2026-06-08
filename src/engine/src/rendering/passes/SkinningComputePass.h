#pragma once
#include <nvrhi/nvrhi.h>
#include <unordered_map>
#include <cstdint>
#include "ECS.h"   // EntityId

class Renderer;
struct PaletteFrame;
class ECS;

// First compute pass: each frame, GPU-skins every skinned entity's mesh vertices into a single
// per-frame skinned vertex buffer + builds an entity->outputVertexOffset table. Runs BEFORE
// shadow/g-buffer. Nothing consumes the output yet (g-buffer still VS-skins, shadow still static);
// this isolates "does the first compute dispatch run cleanly". Renders must look exactly as before.
//
// Standalone (not an IRenderPass): Renderer owns it, calls Execute() once per frame on the shared
// command list before the World pass loop, and drives DestroyGpuResources/RecreateGpuResources on
// backend swap (mirrors the FXAA/SMAA Renderer-owned-pass pattern).
class SkinningComputePass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer);
    // Records skinning dispatches for this frame + uploads the palette. palette null/empty => no work.
    void Execute(nvrhi::ICommandList* cl, const ECS* world, const PaletteFrame* palette);
    nvrhi::IBuffer* GetSkinnedVertexBuffer() const { return m_SkinnedVB; }
    int64_t GetSkinnedVertexOffset(EntityId e) const;       // -1 if entity not skinned this frame
    nvrhi::IBuffer* GetPaletteBuffer() const { return m_PaletteBuffer; }  // moved here from GBuffer
    void DestroyGpuResources();
    bool RecreateGpuResources(nvrhi::IDevice* device, Renderer* renderer);
private:
    void EnsureSkinnedCapacity(uint32_t totalVertices);
    void EnsurePaletteCapacity(uint32_t matrixCount);
    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_CS;
    nvrhi::ComputePipelineHandle m_Pipeline;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_SkinnedVB;     // UAV(write)+VertexBuffer(read), MeshVertex layout
    nvrhi::BufferHandle m_PaletteBuffer; // StructuredBuffer<float4x4> SRV (palette)
    nvrhi::BufferHandle m_ParamsCB;      // per-dispatch constants (volatile)
    uint32_t m_SkinnedCapacity = 0;      // in MeshVertex elements
    uint32_t m_PaletteCapacity = 0;      // in mat4 elements
    std::unordered_map<EntityId, uint32_t> m_OffsetByEntity;
};
