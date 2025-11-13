#pragma once

#include "IRenderPass.h"
#include <nvrhi/nvrhi.h>

struct Glyph
{
    glm::vec2 size;          // pixel size in the atlas
    glm::vec2 offset;        // bearing from baseline (top-left convention)
    glm::vec2 advance;       // advance in pixels
    glm::vec2 textureCoords; // top-left in atlas (pixels)
};

struct FontAtlas
{
    uint32_t width = 512, height = 512, pixelHeight = 0;
    Glyph glyphs[128] = {};
    nvrhi::TextureHandle texture;          // grayscale atlas (R8_UNORM)
    nvrhi::SamplerHandle sampler;          // clamp + linear
    nvrhi::BindingLayoutHandle layout;     // layout: t0 + s0 (created in font.cpp, not used by UI)
    nvrhi::BindingSetHandle bindingSet;    // binds font texture at t0, sampler at s0 (font.cpp)
    // UI-specific binding set (uses UI pass layout: b0 (per-frame), t0 (font), t1 (instances), s0)
    nvrhi::BindingSetHandle uiBindingSet;
};

class UiRenderPass final : public IRenderPass {
public:
    UiRenderPass() = default;
    ~UiRenderPass() override = default;

    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* framebuffer,
        SimulationSnapshot& snapshot,
        double deltaTime) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;

private:
    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;

    // Resources
    nvrhi::BufferHandle m_VertexBuffer;             // static quad vertices (pos, uv)
    nvrhi::BufferHandle m_IndexBuffer;              // static quad indices
    nvrhi::BufferHandle m_PerFrameConstantBuffer;
    nvrhi::BufferHandle m_InstanceBuffer;
    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSet;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    uint32_t m_IndexCount = 0;
    uint32_t m_MaxInstances = 16384;

    FontAtlas                m_FontAtlas{};
};