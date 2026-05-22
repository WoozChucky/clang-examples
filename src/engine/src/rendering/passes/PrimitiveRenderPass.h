#pragma once

#include "IRenderPass.h"
#include <nvrhi/nvrhi.h>

#include "ApplicationContext.h"

class Renderer;

// Primitive render pass implementation
class PrimitiveRenderPass final : public IRenderPass {
public:
    PrimitiveRenderPass() = default;
    ~PrimitiveRenderPass() override = default;

    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* frameBuffer,
        SimulationSnapshot& snapshot,
        const ECS* world,
        double deltaTime,
        FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;

private:
    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;

    // Resources
    nvrhi::BufferHandle m_VertexBuffer;
    nvrhi::BufferHandle m_IndexBuffer;
    nvrhi::BufferHandle m_PerFrameConstantBuffer;
    nvrhi::BufferHandle m_PerDrawCB;
    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSet;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    uint32_t m_IndexCount = 0;
};
