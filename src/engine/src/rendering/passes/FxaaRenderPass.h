#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/vec2.hpp>
#include <cstdint>
#include "IRenderPass.h"

class Renderer;

// Full-screen FXAA resolve. Reads the Renderer's scene-color SRV
// (Renderer::GetSceneColorTexture) and writes the destination framebuffer passed
// to Render(). Owned by Renderer and invoked between the World and Overlay pass
// loops; it is NOT stored in Renderer::m_RenderPasses (it needs a source SRV +
// a distinct destination FB, which the generic pass loop doesn't express).
class FxaaRenderPass : public IRenderPass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot, const ECS* world,
                double deltaTime, FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;
private:
    struct FxaaFrameCB {
        glm::vec2 RcpFrame; // (1/width, 1/height)
        glm::vec2 _pad;
    };
    static_assert(sizeof(FxaaFrameCB) % 16 == 0, "FxaaFrameCB must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS, m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::SamplerHandle m_Sampler;
    nvrhi::BufferHandle m_FrameCB;
};
