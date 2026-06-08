#pragma once

#include <nvrhi/nvrhi.h>

#include "IRenderPass.h"
#include "SsaoMath.h"

class Renderer;

// Screen-space ambient occlusion (ordered World-stage IRenderPass; lives in m_RenderPasses
// between GBufferFill and Lighting). Reads the G-buffer world Normal/Pos, computes per-pixel AO
// into the Renderer-owned m_SsaoRaw target, then box-blurs into m_SsaoBlur. Ignores the
// framebuffer argument passed to Render() (writes Renderer-owned targets, like ShadowDepthPass).
// The lighting pass consumes m_SsaoBlur in a later task; AO is written-but-unused here.
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

    nvrhi::ShaderHandle m_AoCS, m_BlurCS;

    nvrhi::ComputePipelineHandle m_AoPipeline, m_BlurPipeline;
    nvrhi::BindingLayoutHandle   m_AoLayout, m_BlurLayout;

    nvrhi::BufferHandle  m_CB;
    nvrhi::SamplerHandle m_PointClamp;

    SsaoKernel m_Kernel{};
};
