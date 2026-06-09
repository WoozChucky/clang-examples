#pragma once

#include <nvrhi/nvrhi.h>

#include "IRenderPass.h"
#include "SsaoMath.h"

class Renderer;

// Compute SSAO: two compute dispatches — AO (G-buffer normal/world-pos -> raw AO UAV) then a
// 4x4 box blur with an LDS tile cache (raw -> blurred AO UAV). The lighting pass samples the
// blurred AO. Ordered between GBufferFill and Lighting; ignores the framebuffer arg (writes
// Renderer-owned UAV textures).
class SsaoRenderPass : public IRenderPass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList,
                nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot,
                const ECS* world,
                double deltaTime,
                FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;

private:
    nvrhi::IDevice* m_Device   = nullptr;
    Renderer*       m_Renderer = nullptr;

    nvrhi::ShaderHandle m_AoCS, m_BlurCS, m_HbaoCS, m_GtaoCS;

    nvrhi::ComputePipelineHandle m_AoPipeline, m_BlurPipeline, m_HbaoPipeline, m_GtaoPipeline;
    nvrhi::BindingLayoutHandle   m_AoLayout, m_BlurLayout;

    nvrhi::BufferHandle  m_CB;
    nvrhi::SamplerHandle m_PointClamp;

    SsaoKernel m_Kernel{};
};
