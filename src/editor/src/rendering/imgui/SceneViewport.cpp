#include "SceneViewport.h"

#include "lib.h"

nvrhi::IFramebuffer* SceneViewport::EnsureTargets(uint32_t w, uint32_t h,
                                                  nvrhi::Format colorFormat, uint32_t sampleCount)
{
    if (!m_Device)
        return nullptr;

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (m_Fb && w == m_W && h == m_H && colorFormat == m_ColorFormat && sampleCount == m_Samples)
        return m_Fb;

    m_W = w; m_H = h; m_ColorFormat = colorFormat; m_Samples = sampleCount;

    // Color: render target + shader resource (so ImGui can sample it). Unlike MeshPreviewRenderer
    // (which pins keepInitialState), we let NVRHI auto-track the RenderTarget -> ShaderResource
    // transition between the scene command list and the ImGui command list.
    nvrhi::TextureDesc colorDesc;
    colorDesc.width = m_W;
    colorDesc.height = m_H;
    colorDesc.format = colorFormat;
    colorDesc.dimension = nvrhi::TextureDimension::Texture2D;
    colorDesc.sampleCount = sampleCount;
    colorDesc.isRenderTarget = true;
    colorDesc.isShaderResource = true;
    colorDesc.debugName = "SceneViewportColor";
    colorDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    m_Color = m_Device->createTexture(colorDesc);

    nvrhi::TextureDesc depthDesc;
    depthDesc.width = m_W;
    depthDesc.height = m_H;
    depthDesc.format = nvrhi::Format::D32;
    depthDesc.dimension = nvrhi::TextureDimension::Texture2D;
    depthDesc.sampleCount = sampleCount;
    depthDesc.isRenderTarget = true;
    depthDesc.debugName = "SceneViewportDepth";
    depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
    m_Depth = m_Device->createTexture(depthDesc);

    m_Fb = m_Device->createFramebuffer(
        nvrhi::FramebufferDesc()
            .addColorAttachment(m_Color)
            .setDepthAttachment(m_Depth));

    if (!m_Color || !m_Depth || !m_Fb) {
        char warn[128];
        snprintf(warn, sizeof(warn),
                 "SceneViewport: failed to create %ux%u render target", m_W, m_H);
        SM_WARN(warn);
    }

    return m_Fb;
}

void SceneViewport::Release()
{
    m_Fb = nullptr;
    m_Color = nullptr;
    m_Depth = nullptr;
    m_W = 0; m_H = 0;
    m_ColorFormat = nvrhi::Format::UNKNOWN;
    m_Samples = 0;
}
