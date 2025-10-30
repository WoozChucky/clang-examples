#pragma once
#include <dxgi.h>

#include <nvrhi/d3d11.h>

#include "renderer_common.h"

using nvrhi::RefCountPtr;

typedef struct RendererBackendDX11 {
    RendererBackendAPI                              m_Api = RendererBackendAPI::Direct3D11;
    DebugMessageCallback*                           m_MessageCallback;
    nvrhi::DeviceHandle                             m_Device;
    nvrhi::CommandListHandle                        m_CommandList;
    nvrhi::d3d11::DeviceDesc                        m_DeviceDesc;
    RefCountPtr<IDXGIFactory1>                      m_DxgiFactory1;
    RefCountPtr<IDXGIAdapter>                       m_DxgiAdapter;
    RefCountPtr<ID3D11Device>                       m_Device11;
    RefCountPtr<ID3D11DeviceContext>                m_ImmediateContext;
    RefCountPtr<IDXGISwapChain>                     m_SwapChain;
    DXGI_SWAP_CHAIN_DESC                            m_SwapChainDesc{};
    HWND                                            m_hWnd = nullptr;

    nvrhi::TextureHandle                            m_RhiBackBuffer;
    RefCountPtr<ID3D11Texture2D>                    m_D3D11BackBuffer;

    UINT64                                          m_FrameCount = 1;

    RendererBackendSettings                         m_Settings;

    bool                                            m_ResizeRequested;
    double m_AverageFrameTime = 0.0;
    double m_AverageTimeUpdateInterval = 0.5;
    double m_FrameTimeSum = 0.0;
    int m_NumberOfAccumulatedFrames = 0;

    uint32_t m_FrameIndex = 0;

    std::vector<nvrhi::FramebufferHandle> m_SwapChainFramebuffers;
} RendererBackendDX11;

namespace directx11 {
    void create_internal_instance(RendererBackend* backend);
    nvrhi::DeviceHandle create_device(RendererBackend* backend, void* platform);
    void create_swapchain(RendererBackend* backend, HWND hWnd, int width, int height);
    nvrhi::CommandListHandle create_command_list(RendererBackend* backend);
    void renderer_resize_swapchain(RendererBackend* backend, int width, int height);
    bool renderer_begin_frame(RendererBackend* backend);
    nvrhi::IFramebuffer* renderer_get_framebuffer(RendererBackend* backend, int32_t index = -1);
    bool renderer_present(RendererBackend* backend);
    void renderer_update_avg_frame_time(RendererBackend* backend, double elapsedTime);
    uint32_t* renderer_get_frame_index(RendererBackend* backend);
    void renderer_backend_shutdown(RendererBackend* backend);
}