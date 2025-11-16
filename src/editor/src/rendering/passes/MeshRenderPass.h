#pragma once

#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <vector>
#include <cstdint>

#include "IRenderPass.h"

class Renderer;

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
                double deltaTime,
                FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;

    // Adds (uploads) a model to the pass storage. Optional texture in RGBA8.
    // If useTexture is false or texture data is null, a default white texture will be bound.
    ModelHandle AddModel(const MeshVertex* vertices, uint32_t vertexCount,
                         const uint32_t* indices, uint32_t indexCount,
                         bool useTexture,
                         const uint32_t* textureRgba8 = nullptr,
                         uint32_t texWidth = 0, uint32_t texHeight = 0);

private:
    struct PerFrameCB
    {
        glm::mat4 P;   // Projection
        glm::mat4 VP;  // View-Projection
        glm::vec4 LightDir; // xyz = light direction
    };

    struct PerDrawCB
    {
        glm::mat4 Model;     // world transform for this draw
        glm::vec4 BaseColor; // material base color (used when not sampling or as tint)
        uint32_t  Flags;     // bit 0 = sample texture
        uint32_t  _pad[3]{}; // padding to 16-byte alignment
    };

    struct Model
    {
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        uint32_t indexCount = 0;
        bool     useTexture = false;
        nvrhi::TextureHandle texture;   // may be null -> default white
        nvrhi::BindingSetHandle bindingSet; // references per-frame/per-draw CBs + texture + sampler
    };

private:
    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;

    // Shaders & pipeline
    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;

    // Common resources
    nvrhi::BufferHandle m_PerFrameCB;
    nvrhi::BufferHandle m_PerDrawCB;
    nvrhi::SamplerHandle m_Sampler;
    nvrhi::TextureHandle m_DefaultWhite;

    std::vector<Model> m_Models;
};
