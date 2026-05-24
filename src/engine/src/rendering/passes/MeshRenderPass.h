#pragma once

#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <vector>
#include <cstdint>

#include "IRenderPass.h"

class Renderer;
class MeshSystem;
class MaterialSystem;

// A simple 3D mesh render pass that supports POSITION, COLOR, UV
// Pixel shader can either sample a texture or use the vertex color
class MeshRenderPass : public IRenderPass
{
public:
    MeshRenderPass() = default;
    ~MeshRenderPass() override = default;

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
    struct DirectionalLight
    {
        glm::vec4 Direction; // xyz = light direction
        glm::vec4 Color;
    };

    struct PerFrameCB
    {
        glm::mat4 P;   // Projection
        glm::mat4 VP;  // View-Projection
        DirectionalLight DirectionalLight;
        uint32_t PointLightCount = 0; // number of point lights in the structured buffer
        float    Ambient = 0.0f;
        uint32_t _pfPad[2]{};         // padding to 16-byte alignment
    };

    struct PerDrawCB
    {
        glm::mat4 Model;     // world transform for this draw
        glm::mat4 NormalMatrix;
        glm::vec4 BaseColor; // material base color (used when not sampling or as tint)
        uint32_t  Flags;     // bit 0 = sample texture
        uint32_t  _pad[3]{}; // padding to 16-byte alignment
    };

private:
    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;

    // Shaders & pipeline
    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::GraphicsPipelineHandle m_WireframePipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;

    // Common resources
    nvrhi::BufferHandle m_PerFrameCB;
    nvrhi::BufferHandle m_PerDrawCB;

    // Point lights GPU buffer (StructuredBuffer SRV)
    struct PointLightCPU
    {
        glm::vec4 Position;  // xyz = world position
        glm::vec4 Color;     // rgba
        float     Intensity; // scalar multiplier
        float     Range;     // attenuation range
        float     _pad[2];   // padding to 16-byte alignment
    };
    static_assert(sizeof(PointLightCPU) % 16 == 0, "PointLightCPU must be 16-byte aligned");

    nvrhi::BufferHandle m_PointLightBuffer;
    uint32_t m_MaxPointLights = 256;

    // Instance data for instanced rendering
    struct MeshInstanceCPU
    {
        glm::mat4 Model;
        glm::mat4 NormalMatrix;
        glm::vec4 BaseColor;
        uint32_t Flags;
        uint32_t _pad[3]; // 16-byte alignment
    };
    static_assert(sizeof(MeshInstanceCPU) % 16 == 0, "MeshInstanceCPU must be 16-byte aligned");

    nvrhi::BufferHandle m_InstanceBuffer;
    uint32_t m_MaxInstances = 4096;
};
