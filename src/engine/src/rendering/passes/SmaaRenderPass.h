#pragma once

#include <nvrhi/nvrhi.h>
#include <vector>
#include <utility>

#include "IRenderPass.h"

class Renderer;

// SMAA 1x resolve (Renderer-owned, NOT in m_RenderPasses). Occupies the same socket as
// FxaaRenderPass: reads the offscreen scene-color SRV, writes the swapchain framebuffer.
// Three full-screen sub-passes: luma edge detection -> blend-weight calc -> neighborhood blend.
class SmaaRenderPass : public IRenderPass {
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
    bool EnsureTargets(uint32_t width, uint32_t height);

    nvrhi::IDevice* m_Device   = nullptr;
    Renderer*       m_Renderer = nullptr;

    nvrhi::ShaderHandle m_EdgeVS, m_EdgePS;
    nvrhi::ShaderHandle m_WeightVS, m_WeightPS;
    nvrhi::ShaderHandle m_BlendVS, m_BlendPS;

    nvrhi::GraphicsPipelineHandle m_EdgePipeline, m_WeightPipeline, m_BlendPipeline;

    nvrhi::BindingLayoutHandle m_EdgeLayout, m_WeightLayout, m_BlendLayout;
    nvrhi::BufferHandle        m_FrameCB;
    nvrhi::SamplerHandle       m_LinearClamp;
    nvrhi::SamplerHandle       m_PointClamp;

    nvrhi::TextureHandle m_AreaTex;
    nvrhi::TextureHandle m_SearchTex;

    uint32_t              m_Width = 0, m_Height = 0;
    nvrhi::TextureHandle  m_EdgesTex;
    nvrhi::TextureHandle  m_BlendTex;
    nvrhi::FramebufferHandle m_EdgesFb, m_BlendFb;
};
