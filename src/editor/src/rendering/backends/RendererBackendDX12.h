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
    explicit RendererBackendDX12(RendererBackendSettings &settings, GLFWwindow* window)
        : RendererBackend(settings, window)
    {}
    ~RendererBackendDX12() override = default;
    bool Init() override;
    void Shutdown(uint32_t timeoutMs) override;
    [[nodiscard]] RendererAPI GetAPI() const override;
    nvrhi::DeviceHandle CreateDevice() override;
    void CreateSwapChain(uint32_t width, uint32_t height) override;
    void ResizeSwapChain(uint32_t width, uint32_t height) override;
    nvrhi::ITexture* GetCurrentBackBuffer() override;
    nvrhi::ITexture* GetBackBuffer(uint32_t index) override;
    uint32_t GetCurrentBackBufferIndex() override;
    uint32_t GetBackBufferCount() override;
    bool BeginFrame() override;
    bool Present() override;
    nvrhi::ShaderHandle CreateShaderFromMemory(
        nvrhi::ShaderType shaderType,
        const char* content,
        size_t contentSize,
        const char* entryPoint,
        const char* targetName) override;

    // TEMP DIAGNOSTIC (hot-swap leak hunt): dumps all live D3D/DXGI objects via
    // the global DXGI debug interface. Backend-agnostic (reports across devices).
    static void ReportLiveObjects();
protected:
    void DestroyDeviceAndSwapChain() override;
    nvrhi::DeviceHandle GetDevice() override;

private:
    static void EnableDebugLayerIfAvailable();
    static void ValidateDX12UltimateCapabilities(ID3D12Device* device);
    static RefCountPtr<IDXGIAdapter1> PickHardwareAdapter(const RefCountPtr<IDXGIFactory2>& factory);
    static bool MoveWindowOntoAdapter(IDXGIAdapter* targetAdapter, RECT& rect);

    void CreateRenderTargets();
    void ReleaseRenderTargets();
    void ResizeSwapChain();


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

    bool                                            m_ResizeRequested = false;
};
