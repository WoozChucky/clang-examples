#pragma once

#include <Windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_5.h>

#include <nvrhi/d3d12.h>

#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using nvrhi::RefCountPtr;

class DebugMessageCallback : public nvrhi::IMessageCallback {
public:
    void message(nvrhi::MessageSeverity severity, const char* messageText) override;
};

typedef struct RendererBackend RendererBackend;

typedef struct RendererBackendSettings {
    uint32_t                         refreshRate = 60;
    uint32_t                         swapChainBufferCount = 3;
    nvrhi::Format                    swapChainFormat = nvrhi::Format::SRGBA8_UNORM;
    uint32_t                         swapChainSampleCount = 1;
    uint32_t                         swapChainSampleQuality = 0;
    uint32_t                         maxFramesInFlight = 2;
    uint32_t                         backBufferWidth;
    uint32_t                         backBufferHeight;
    bool                             vsyncEnabled = true;
} RendererBackendSettings;

typedef struct RendererBackendDX12 {
    DebugMessageCallback*                           m_MessageCallback;
    nvrhi::DeviceHandle                             m_Device;
    nvrhi::d3d12::DeviceDesc                        m_DeviceDesc;
    RefCountPtr<IDXGIFactory2>                      m_DxgiFactory2;
    RefCountPtr<ID3D12Device>                       m_Device12;
    RefCountPtr<ID3D12CommandQueue>                 m_GraphicsQueue;
    RefCountPtr<ID3D12CommandQueue>                 m_ComputeQueue;
    RefCountPtr<ID3D12CommandQueue>                 m_CopyQueue;
    RefCountPtr<IDXGISwapChain3>                    m_SwapChain;
    DXGI_SWAP_CHAIN_DESC1                           m_SwapChainDesc{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC                 m_FullScreenDesc{};
    RefCountPtr<IDXGIAdapter>                       m_DxgiAdapter;
    HWND                                            m_hWnd = nullptr;
    bool                                            m_TearingSupported = false;

    std::vector<RefCountPtr<ID3D12Resource>>        m_SwapChainBuffers;
    std::vector<nvrhi::TextureHandle>               m_RhiSwapChainBuffers;
    RefCountPtr<ID3D12Fence>                        m_FrameFence;
    std::vector<HANDLE>                             m_FrameFenceEvents;

    UINT64                                          m_FrameCount = 1;

    RendererBackendSettings                         m_Settings;

    double m_AverageFrameTime = 0.0;
    double m_AverageTimeUpdateInterval = 0.5;
    double m_FrameTimeSum = 0.0;
    int m_NumberOfAccumulatedFrames = 0;

    uint32_t m_FrameIndex = 0;

    std::vector<nvrhi::FramebufferHandle> m_SwapChainFramebuffers;
} RendererBackendDX12;

void create_internal_instance(RendererBackend* backend);
nvrhi::DeviceHandle create_device(RendererBackend* backend);
void create_swapchain(RendererBackend* backend, HWND hWnd, int width, int height);
bool renderer_begin_frame(RendererBackend* backend);
void renderer_render(RendererBackend* backend);
bool renderer_present(RendererBackend* backend);
void renderer_update_avg_frame_time(RendererBackend* backend, double elapsedTime);
uint32_t* renderer_get_frame_index(RendererBackend* backend);
