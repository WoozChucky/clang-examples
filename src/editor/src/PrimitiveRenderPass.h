#pragma once

#include "IRenderPass.h"
#include <nvrhi/nvrhi.h>
#include "glm/mat4x4.hpp"
#include "glm/vec2.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"

class Renderer;

// Helper structs for primitive pass
struct PerFrameCBData {
    glm::mat4 Model;
    glm::mat4 VP;
    glm::vec4 CameraPos;
};
static_assert(sizeof(PerFrameCBData) % 16 == 0);

struct PrimPerDrawCB {
    glm::vec4 GridColor;
    glm::vec4 AxisXColor;
    glm::vec4 AxisZColor;
    glm::vec2 GridParams;
    glm::vec2 FadeParams;
};
static_assert(sizeof(PrimPerDrawCB) % 16 == 0);

enum class PrimitiveType : uint32_t { Plane = 0, Cube = 1, Sphere = 2, Cone = 3, Line = 4 };

struct PrimitiveInstance
{
    PrimitiveType type;
    glm::mat4     transform; // world transform
    glm::vec4     color;     // base color (not all primitives use it)
    glm::vec4     params;    // optional parameters per primitive
};

// Primitive render pass implementation
class PrimitiveRenderPass : public IRenderPass {
public:
    PrimitiveRenderPass() = default;
    ~PrimitiveRenderPass() override = default;

    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* framebuffer,
        PerspectiveCamera3D& camera,
        double deltaTime) override;
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
