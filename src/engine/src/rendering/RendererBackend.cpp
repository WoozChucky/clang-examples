#include "RendererBackend.h"


nvrhi::IFramebuffer* RendererBackend::GetCurrentFrameBuffer() {
    return GetFrameBuffer(GetCurrentBackBufferIndex());
}

nvrhi::IFramebuffer* RendererBackend::GetFrameBuffer(uint32_t index) {
    if (index < m_SwapChainFramebuffers.size())
        return m_SwapChainFramebuffers[index];

    return nullptr;
}

void RendererBackend::BackBufferResizing() {
    m_SwapChainFramebuffers.clear();
    m_SwapChainDepthTextures.clear();
}

void RendererBackend::BackBufferResized() {
    const uint32_t backBufferCount = GetBackBufferCount();
    m_SwapChainFramebuffers.resize(backBufferCount);
    m_SwapChainDepthTextures.resize(backBufferCount);
    for (uint32_t index = 0; index < backBufferCount; index++)
    {
        // Create depth texture matching swapchain size/format
        nvrhi::TextureDesc depthDesc;
        depthDesc.width = m_Settings->backBufferWidth;
        depthDesc.height = m_Settings->backBufferHeight;
        depthDesc.dimension = nvrhi::TextureDimension::Texture2D;
        depthDesc.sampleCount = m_Settings->swapChainSampleCount;
        depthDesc.sampleQuality = m_Settings->swapChainSampleQuality;
        depthDesc.format = nvrhi::Format::D32;
        depthDesc.isRenderTarget = true;
        depthDesc.debugName = "SwapchainDepth";
        depthDesc.keepInitialState = true;
        depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
        m_SwapChainDepthTextures[index] = GetDevice()->createTexture(depthDesc);

        m_SwapChainFramebuffers[index] = GetDevice()->createFramebuffer(
            nvrhi::FramebufferDesc()
                    .addColorAttachment(GetBackBuffer(index))
                    .setDepthAttachment(m_SwapChainDepthTextures[index])
        );
    }
}
