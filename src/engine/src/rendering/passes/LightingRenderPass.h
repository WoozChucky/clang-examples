#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>
#include "IRenderPass.h"

class Renderer;

// Full-screen deferred lighting: reads the Renderer's G-buffer, computes the
// directional + point + shadow lighting and fog, writes the scene color.
class LightingRenderPass : public IRenderPass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot, const ECS* world,
                double deltaTime, FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;
private:
    struct DirectionalLight { glm::vec4 Direction; glm::vec4 Color; };
    struct LightFrameCB {
        glm::mat4 LightVP;
        DirectionalLight Dir;
        glm::vec4 CameraPos;   // xyz
        glm::vec4 Fog;         // rgb=color, w=density
        uint32_t  PointLightCount; float Ambient; int ShadowEnabled; float ShadowBias;
        int       FogEnabled;  int _pad[3];
    };
    static_assert(sizeof(LightFrameCB) % 16 == 0, "LightFrameCB must be 16-byte aligned");

    struct PointLightCPU { // keep identical to MeshRenderPass's PointLightCPU
        glm::vec4 Position; glm::vec4 Color; float Intensity; float Range; float _pad[2];
    };
    static_assert(sizeof(PointLightCPU) % 16 == 0, "PointLightCPU must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS, m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::SamplerHandle m_GBufSampler;
    nvrhi::BufferHandle m_FrameCB;
    nvrhi::BufferHandle m_PointLightBuffer;
    uint32_t m_MaxPointLights = 256;
};
