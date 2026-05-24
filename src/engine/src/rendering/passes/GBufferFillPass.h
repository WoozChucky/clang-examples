#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>
#include "IRenderPass.h"

class Renderer;

// Deferred geometry pass: writes albedo / world-normal / world-position into the
// Renderer's G-buffer MRTs + shared depth. No lighting.
class GBufferFillPass : public IRenderPass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot, const ECS* world,
                double deltaTime, FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;
private:
    struct GBufFrameCB { glm::mat4 VP; };
    struct MeshInstanceCPU { // keep identical to MeshRenderPass's MeshInstanceCPU
        glm::mat4 Model; glm::mat4 NormalMatrix; glm::vec4 BaseColor;
        uint32_t Flags; uint32_t _pad[3];
    };
    static_assert(sizeof(MeshInstanceCPU) % 16 == 0, "MeshInstanceCPU must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS, m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_FrameCB;
    nvrhi::BufferHandle m_InstanceBuffer;
    uint32_t m_MaxInstances = 4096;
};
