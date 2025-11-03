#pragma once

#include "RendererBackend.h"

#include <Windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_5.h>

#include <nvrhi/d3d12.h>

using Microsoft::WRL::ComPtr;
using nvrhi::RefCountPtr;

class RendererBackendDX12 final : public RendererBackend {
public:
    explicit RendererBackendDX12(const RendererBackendSettings &settings, GLFWwindow* window)
        : RendererBackend(settings, window)
    {}
    ~RendererBackendDX12() override = default;
    bool Init() override;
    void Shutdown(uint32_t timeoutMs) override;
    [[nodiscard]] RendererAPI GetAPI() const override;
    nvrhi::DeviceHandle CreateDevice() override;
    void CreateSwapChain(uint32_t width, uint32_t height) override;
    nvrhi::CommandListHandle CreateCommandList() override;
    void ResizeSwapChain(uint32_t width, uint32_t height) override;
    nvrhi::ITexture* GetCurrentBackBuffer() override;
    nvrhi::ITexture* GetBackBuffer(uint32_t index) override;
    uint32_t GetCurrentBackBufferIndex() override;
    uint32_t GetBackBufferCount() override;
    uint32_t* GetFrameIndexPtr() override;
    nvrhi::IFramebuffer* GetFrameBuffer(int32_t index) override;
    bool BeginFrame() override;
    bool Present() override;
protected:
    void DestroyDeviceAndSwapChain() override;

private:
    static void EnableDebugLayerIfAvailable();
    static void ValidateDX12UltimateCapabilities(ID3D12Device* device);
    static RefCountPtr<IDXGIAdapter1> PickHardwareAdapter(const RefCountPtr<IDXGIFactory2>& factory);
    static void ReportLiveObjects();
    static bool MoveWindowOntoAdapter(IDXGIAdapter* targetAdapter, RECT& rect);

    void CreateRenderTargets();
    void ReleaseRenderTargets();
    void ResizeSwapChain();
    void BackBufferResizing();
    void BackBufferResized();

private:
    nvrhi::DeviceHandle                             m_Device;
    nvrhi::CommandListHandle                        m_CommandList;
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
    uint32_t                                        m_FrameIndex = 0;

    std::vector<nvrhi::FramebufferHandle>           m_SwapChainFramebuffers;
    bool                                            m_ResizeRequested = false;
};