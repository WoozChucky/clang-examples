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
}

void RendererBackend::BackBufferResized() {
    uint32_t backBufferCount = GetBackBufferCount();
    m_SwapChainFramebuffers.resize(backBufferCount);
    for (uint32_t index = 0; index < backBufferCount; index++)
    {
        m_SwapChainFramebuffers[index] = GetDevice()->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(GetBackBuffer(index)));
    }
}
