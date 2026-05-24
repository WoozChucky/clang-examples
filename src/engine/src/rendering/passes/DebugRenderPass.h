#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <vector>
#include <cstdint>

#include "IRenderPass.h"
#include "DebugDraw.h"

class Renderer;

// Draws editor debug gizmos (light gizmos, game-camera frustum, selected-entity AABB) as line lists,
// AFTER OutlineRenderPass, depth-tested (test on, write off, cull none). Gizmo selection is gated by
// GetDebugDrawSettings(); when all are off (default, and always in runtime) the pass early-outs.
class DebugRenderPass : public IRenderPass
{
public:
    DebugRenderPass() = default;
    ~DebugRenderPass() override = default;

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
    struct DebugCB { glm::mat4 VP; };
    static_assert(sizeof(DebugCB) % 16 == 0, "DebugCB must be 16-byte aligned");

    void EnsureVertexCapacity(size_t vertexCount);

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;

    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_CB;
    nvrhi::BindingSetHandle m_BindingSet;

    nvrhi::BufferHandle m_VertexBuffer;
    size_t m_VertexCapacity = 0;
    std::vector<DebugVertex> m_Verts;
};
