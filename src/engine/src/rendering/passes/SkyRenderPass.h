#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>
#include "IRenderPass.h"

class Renderer;

// Full-screen procedural sky drawn into sky pixels (far-plane depth test):
// day/night gradient + sun disc + moon disc. Runs after LightingRenderPass.
class SkyRenderPass : public IRenderPass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot, const ECS* world,
                double deltaTime, FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;
private:
    struct SkyFrameCB {
        glm::mat4 InvViewProj;  // 64
        glm::vec4 CameraPos;    // xyz
        glm::vec4 SunDir;       // xyz = light travel direction
        glm::vec4 DayZenith;
        glm::vec4 DayHorizon;
        glm::vec4 NightZenith;
        glm::vec4 NightHorizon;
        glm::vec4 SunColor;     // rgb; w = glow exponent
        glm::vec4 MoonColor;    // rgb; w = glow exponent
        glm::vec4 Disc;         // x=sunCosOuter y=sunCosInner z=moonCosOuter w=moonCosInner
    };
    static_assert(sizeof(SkyFrameCB) % 16 == 0, "SkyFrameCB must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS, m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_FrameCB;
};
