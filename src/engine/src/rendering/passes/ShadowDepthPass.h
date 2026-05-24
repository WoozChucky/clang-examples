#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include "IRenderPass.h"

class Renderer;

// Renders scene depth from the sun's POV into the Renderer's shadow map (depth-only, front-face cull),
// fitting an ortho light frustum to the visible-mesh AABB. Gated by GetShadowSettings().Enabled +
// IsSunUp; when gated off it sets Renderer.GetShadowView().Enabled = 0 and skips. Runs before MeshRenderPass.
class ShadowDepthPass : public IRenderPass
{
public:
    ShadowDepthPass() = default;
    ~ShadowDepthPass() override = default;

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
    struct ShadowCB { glm::mat4 LightVP; glm::mat4 Model; };
    static_assert(sizeof(ShadowCB) % 16 == 0, "ShadowCB must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_CB;
    nvrhi::BindingSetHandle m_BindingSet;
};
