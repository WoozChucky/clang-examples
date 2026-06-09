#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>
#include <memory>
#include <unordered_map>
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
    struct GBufFrameCB { glm::mat4 VP; glm::mat4 PrevVP; };
    struct MeshInstanceCPU { // keep identical to the HLSL InstanceData (VS + PS) field-for-field
        glm::mat4 Model; glm::mat4 NormalMatrix; glm::mat4 PrevModel; glm::vec4 BaseColor;
        uint32_t Flags; uint32_t IsSkinned; uint32_t PrevSkinnedOffset; uint32_t _pad;
    };
    static_assert(sizeof(MeshInstanceCPU) % 16 == 0, "MeshInstanceCPU must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS, m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::GraphicsPipelineHandle m_WireframePipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_FrameCB;
    nvrhi::BufferHandle m_InstanceBuffer;
    // 1-element dummy structured buffer (MeshVertex layout) bound at t6 so gPrevSkinned is always
    // present; the VS only reads it when IsSkinned!=0 (Task 2). Task 1 never sets IsSkinned.
    nvrhi::BufferHandle m_DummyPrevSkinned;
    // Per-entity previous-frame model matrix (key = (uint32_t)EntityId), used for rigid velocity.
    std::unordered_map<uint32_t, glm::mat4> m_PrevModel;
    uint32_t m_MaxInstances = 4096;
};
