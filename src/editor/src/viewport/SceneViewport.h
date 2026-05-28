#pragma once
#include <cstdint>
#include <nvrhi/nvrhi.h>

// Owns the editor's offscreen scene render target (color + depth + framebuffer). The gameplay
// passes render into this; ImGui then samples the color texture in the Viewport panel. Color
// format + sample count are matched to the swapchain so the passes' pipelines stay compatible;
// depth is D32 (matches the swapchain depth). RenderThread-only; no locking.
class SceneViewport {
public:
    void Init(nvrhi::IDevice* device) { m_Device = device; }

    // (Re)create targets if size/format/samples changed. w,h are clamped to >= 1. Returns the
    // framebuffer to render the scene into (never null once a valid device is set).
    nvrhi::IFramebuffer* EnsureTargets(uint32_t w, uint32_t h,
                                       nvrhi::Format colorFormat, uint32_t sampleCount);

    nvrhi::ITexture* ColorTexture() const { return m_Color.Get(); }

    // Device-lost: drop all device-bound resources (rebuilt lazily by the next EnsureTargets).
    void Release();

private:
    nvrhi::IDevice* m_Device = nullptr;
    nvrhi::TextureHandle m_Color;
    nvrhi::TextureHandle m_Depth;
    nvrhi::FramebufferHandle m_Fb;
    uint32_t m_W = 0;
    uint32_t m_H = 0;
    nvrhi::Format m_ColorFormat = nvrhi::Format::UNKNOWN;
    uint32_t m_Samples = 0;
};
