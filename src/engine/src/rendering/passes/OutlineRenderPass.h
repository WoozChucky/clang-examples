#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>

#include "IRenderPass.h"

class Renderer;

// Draws the selected entity's mesh enlarged along its normals in a flat color (inverted hull),
// BEFORE MeshRenderPass, so the normal mesh covers the center and a colored rim remains.
// Reads the selected entity from ApplicationContext (set by the editor overlay); no-op in runtime.
class OutlineRenderPass : public IRenderPass
{
public:
    OutlineRenderPass() = default;
    ~OutlineRenderPass() override = default;

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
    struct OutlineCB
    {
        glm::mat4 VP;
        glm::mat4 Model;
        float     OutlineWidth;
        float     _pad0[3];
        glm::vec4 OutlineColor;
    };
    static_assert(sizeof(OutlineCB) % 16 == 0, "OutlineCB must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;

    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_CB;
    nvrhi::BindingSetHandle m_BindingSet;
};
