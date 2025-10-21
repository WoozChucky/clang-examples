#include "renderer_dx12.h"

#include "lib.h"

#include <optional>

#include <nvrhi/validation.h>

#include <dxgidebug.h>

#define HR_ASSERT(x, msg) SM_ASSERT(SUCCEEDED(x), msg)

void DebugMessageCallback::message(nvrhi::MessageSeverity severity, const char* messageText) {
    SM_TRACE("NVRHI: %s", messageText);
};

// Try to enable the D3D12 debug layer (only available on developer SKUs).
void EnableDebugLayerIfAvailable() {
#if defined(_DEBUG)
    RefCountPtr<ID3D12Debug> debug;
    HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (SUCCEEDED(hr)) {
        debug->EnableDebugLayer();
        // If you want GPU-based validation (slower), you can enable it here via ID3D12Debug1.
        // ComPtr<ID3D12Debug1> debug1;
        // if (SUCCEEDED(debug.As(&debug1))) debug1->SetEnableGPUBasedValidation(TRUE);
        SM_TRACE("D3D12 Debug layer enabled.");
    } else {
        SM_TRACE("D3D12 Debug layer not available.");
    }
#else
    (void)0;
#endif
}

void ReportLiveObjects()
{
    RefCountPtr<IDXGIDebug> pDebug;
    DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug));

    if (pDebug)
    {
        DXGI_DEBUG_RLO_FLAGS flags = (DXGI_DEBUG_RLO_FLAGS)(DXGI_DEBUG_RLO_IGNORE_INTERNAL | DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_DETAIL);
        HRESULT hr = pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, flags);
        if (FAILED(hr))
        {
            SM_TRACE("ReportLiveObjects failed, HRESULT = 0x%08x", hr);
        }
    }
}

// Select a suitable hardware adapter (prefer discrete adapters that support D3D12).
// Returns nullptr if none found (caller should handle fallback to WARP if desired).
RefCountPtr<IDXGIAdapter1> PickHardwareAdapter(const RefCountPtr<IDXGIFactory2>& factory) {
    RefCountPtr<IDXGIAdapter1> bestAdapter;
    SIZE_T bestDedicatedVideoMem = 0;

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        RefCountPtr<IDXGIAdapter1> adapter;
        if (DXGI_ERROR_NOT_FOUND == factory->EnumAdapters1(adapterIndex, &adapter)) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // Skip software adapters
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        // Check D3D12 support (try creating device at feature level 11_1)
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_1, _uuidof(ID3D12Device), nullptr))) {
            // Prefer adapter with more dedicated video memory
            if (desc.DedicatedVideoMemory > bestDedicatedVideoMem) {
                bestDedicatedVideoMem = desc.DedicatedVideoMemory;
                bestAdapter = adapter;
            }
        }
    }

    return bestAdapter;
}

void ValidateDX12UltimateCapabilities(ID3D12Device* device) {
    // 1) Raytracing (Options5 -> RaytracingTier). Want at least Tier_1_1 (DXR 1.1).
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)))) {
        if (opts5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
            throw std::runtime_error("DX12 Ultimate required: Raytracing Tier 1.1 not supported on adapter.");
        }
    } else {
        throw std::runtime_error("Failed to query D3D12_OPTIONS5; cannot confirm raytracing support.");
    }

    // 2) Variable Rate Shading (Options6 -> VariableShadingRateTier). Want Tier 2 for full DX12U VRS features.
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 opts6 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opts6, sizeof(opts6)))) {
        if (opts6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2) {
            throw std::runtime_error("DX12 Ultimate required: Variable Shading Rate Tier 2 not supported.");
        }
    } else {
        throw std::runtime_error("Failed to query D3D12_OPTIONS6; cannot confirm VRS support.");
    }

    // 3) Mesh shaders + sampler feedback (Options7).
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7)))) {
        if (opts7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
            throw std::runtime_error("DX12 Ultimate required: Mesh shader tier not supported on adapter.");
        }
        if (opts7.SamplerFeedbackTier == D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED) {
            throw std::runtime_error("DX12 Ultimate required: Sampler Feedback not supported on adapter.");
        }
    } else {
        throw std::runtime_error("Failed to query D3D12_OPTIONS7; cannot confirm mesh/sampler feedback support.");
    }

    // Passed all checks:
    SM_TRACE("[D3D12] Adapter supports: DXR 1.1, VRS Tier2, Mesh shaders, Sampler Feedback.");
}

void create_render_targets(RendererBackendDX12* dx12)
{
    dx12->m_SwapChainBuffers.resize(dx12->m_SwapChainDesc.BufferCount);
    dx12->m_RhiSwapChainBuffers.resize(dx12->m_SwapChainDesc.BufferCount);

    for(UINT n = 0; n <dx12-> m_SwapChainDesc.BufferCount; n++)
    {
        const HRESULT hr = dx12->m_SwapChain->GetBuffer(n, IID_PPV_ARGS(&dx12->m_SwapChainBuffers[n]));
        HR_ASSERT(hr, "Failed to get swap chain buffer");

        nvrhi::TextureDesc textureDesc;
        textureDesc.width = dx12->m_Settings.backBufferWidth;
        textureDesc.height = dx12->m_Settings.backBufferHeight;
        textureDesc.sampleCount = dx12->m_Settings.swapChainSampleCount;
        textureDesc.sampleQuality = dx12->m_Settings.swapChainSampleQuality;
        textureDesc.format = dx12->m_Settings.swapChainFormat;
        textureDesc.debugName = "SwapChainBuffer";
        textureDesc.isRenderTarget = true;
        textureDesc.isUAV = false;
        textureDesc.initialState = nvrhi::ResourceStates::Present;
        textureDesc.keepInitialState = true;

        dx12->m_RhiSwapChainBuffers[n] = dx12->m_Device->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(dx12->m_SwapChainBuffers[n]), textureDesc);
    }
}

void release_render_targets(RendererBackendDX12* dx12) {
    if (dx12->m_Device)
    {
        // Make sure that all frames have finished rendering
        dx12->m_Device->waitForIdle();

        // Release all in-flight references to the render targets
        dx12->m_Device->runGarbageCollection();
    }

    // Set the events so that WaitForSingleObject in OneFrame will not hang later
    for(auto e : dx12->m_FrameFenceEvents)
        SetEvent(e);

    // Release the old buffers because ResizeBuffers requires that
    dx12->m_RhiSwapChainBuffers.clear();
    dx12->m_SwapChainBuffers.clear();
}

void resize_swapchain(RendererBackendDX12* dx12) {
    release_render_targets(dx12);

    if (!dx12->m_Device)
        return;

    if (!dx12->m_SwapChain)
        return;

    const HRESULT hr = dx12->m_SwapChain->ResizeBuffers(dx12->m_Settings.swapChainBufferCount,
                                            dx12->m_Settings.backBufferWidth,
                                            dx12->m_Settings.backBufferHeight,
                                            dx12->m_SwapChainDesc.Format,
                                            dx12->m_SwapChainDesc.Flags);
    HR_ASSERT(hr, "Failed to resize swap chain buffers");

    create_render_targets(dx12);
}

void create_internal_instance(RendererBackend* backend) {

    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ERROR("RendererBackendDX12 is null");
        return;
    }

    if (!dx12->m_DxgiFactory2) {
        EnableDebugLayerIfAvailable();

        UINT flags = 0;
#if defined(_DEBUG)
        flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
        const auto hRes = CreateDXGIFactory2(0, IID_PPV_ARGS(&dx12->m_DxgiFactory2));
        HR_ASSERT(SUCCEEDED(hRes), "Failed to create DXGI Factory2");
    }
}

nvrhi::DeviceHandle create_device(RendererBackend* backend) {

    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ERROR("RendererBackendDX12 is null");
        return nullptr;
    }

    // Pick hardware adapter
    dx12->m_DxgiAdapter = PickHardwareAdapter(dx12->m_DxgiFactory2);
    if (!dx12->m_DxgiAdapter) {
        // No hardware adapter found; user might want to fall back to WARP.
        // We throw here to keep this example focused. In production, consider creating WARP adapter:
        // factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter))
        SM_ASSERT(false, "No suitable hardware adapter found for D3D12.");
        return nullptr;
    }

    HRESULT hr = D3D12CreateDevice(dx12->m_DxgiAdapter, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&dx12->m_Device12));
    HR_ASSERT(hr, "Failed to create D3D12 device");

    {
        RefCountPtr<ID3D12InfoQueue> pInfoQueue;
        dx12->m_Device12->QueryInterface(&pInfoQueue);

        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);

        D3D12_MESSAGE_ID disableMessageIDs[] = {
            D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_COMMAND_LIST_STATIC_DESCRIPTOR_RESOURCE_DIMENSION_MISMATCH, // descriptor validation doesn't understand acceleration structures
            D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED, // NGX currently generates benign resource creation warnings
        };

        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.pIDList = disableMessageIDs;
        filter.DenyList.NumIDs = std::size(disableMessageIDs);
        pInfoQueue->AddStorageFilterEntries(&filter);
    }

    // Sanity: confirm CheckFeatureSupport & capability tiers (DXR/VRS/Mesh/SamplerFeedback).
    ValidateDX12UltimateCapabilities(dx12->m_Device12);

    D3D12_COMMAND_QUEUE_DESC queueDesc;
    ZeroMemory(&queueDesc, sizeof(queueDesc));
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.NodeMask = 1;

    hr = dx12->m_Device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dx12->m_GraphicsQueue));
    HR_ASSERT(SUCCEEDED(hr), "Failed to create graphics queue");
    hr = dx12->m_GraphicsQueue->SetName(L"Graphics Queue");
    HR_ASSERT(hr, "Failed to name graphics queue");

    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    hr = dx12->m_Device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dx12->m_ComputeQueue));
    HR_ASSERT(hr, "Failed to create compute queue");
    hr = dx12->m_ComputeQueue->SetName(L"Compute Queue");
    HR_ASSERT(hr, "Failed to name compute queue");

    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    hr = dx12->m_Device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dx12->m_CopyQueue));
    HR_ASSERT(hr, "Failed to create copy queue");
    hr = dx12->m_CopyQueue->SetName(L"Copy Queue");
    HR_ASSERT(hr, "Failed to name copy queue");

    dx12->m_MessageCallback = new DebugMessageCallback(); // reinterpret_cast<DebugMessageCallback *>(bump_alloc(persistentStorage, sizeof(DebugMessageCallback)));
    dx12->m_DeviceDesc.errorCB = dx12->m_MessageCallback;
    dx12->m_DeviceDesc.logBufferLifetime = true;
    dx12->m_DeviceDesc.renderTargetViewHeapSize = 1024;
    dx12->m_DeviceDesc.depthStencilViewHeapSize = 1024;
    dx12->m_DeviceDesc.shaderResourceViewHeapSize = 16384;
    dx12->m_DeviceDesc.samplerHeapSize = 1024;
    dx12->m_DeviceDesc.maxTimerQueries = 256;
    dx12->m_DeviceDesc.enableHeapDirectlyIndexed = false;
    dx12->m_DeviceDesc.aftermathEnabled = false;

    dx12->m_DeviceDesc.pDevice = dx12->m_Device12;
    dx12->m_DeviceDesc.pGraphicsCommandQueue = dx12->m_GraphicsQueue;
    dx12->m_DeviceDesc.pComputeCommandQueue = dx12->m_ComputeQueue;
    dx12->m_DeviceDesc.pCopyCommandQueue = dx12->m_CopyQueue;

    SM_TRACE("[D3D12] Created graphics (DIRECT) queue, compute queue, and copy queue.");

    nvrhi::DeviceHandle nvrhiDevice = nvrhi::d3d12::createDevice(dx12->m_DeviceDesc);

#if defined(_DEBUG)
    nvrhiDevice = nvrhi::validation::createValidationLayer(nvrhiDevice);
#endif

    dx12->m_Device = nvrhiDevice;

    return nvrhiDevice;
}

void create_swapchain(RendererBackend* backend, HWND hWnd, const int width, const int height) {
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ERROR("RendererBackendDX12 is null");
        return;
    }

    dx12->m_Settings.backBufferWidth = width;
    dx12->m_Settings.backBufferHeight = height;

    ZeroMemory(&dx12->m_SwapChainDesc, sizeof(dx12->m_SwapChainDesc));
    dx12->m_SwapChainDesc.Width = width;
    dx12->m_SwapChainDesc.Height = height;
    dx12->m_SwapChainDesc.SampleDesc.Count = dx12->m_Settings.swapChainSampleCount;
    dx12->m_SwapChainDesc.SampleDesc.Quality = dx12->m_Settings.swapChainSampleQuality;
    dx12->m_SwapChainDesc.BufferUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
    dx12->m_SwapChainDesc.BufferCount = dx12->m_Settings.swapChainBufferCount;
    dx12->m_SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    dx12->m_SwapChainDesc.Flags = 0;
    switch (dx12->m_Settings.swapChainFormat)
    {
        case nvrhi::Format::SRGBA8_UNORM:
            dx12->m_SwapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case nvrhi::Format::SBGRA8_UNORM:
            dx12->m_SwapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        default:
            dx12->m_SwapChainDesc.Format = nvrhi::d3d12::convertFormat(dx12->m_Settings.swapChainFormat);
            break;
    }

    RefCountPtr<IDXGIFactory5> pDxgiFactory5;
    if (SUCCEEDED(dx12->m_DxgiFactory2->QueryInterface(IID_PPV_ARGS(&pDxgiFactory5))))
    {
        BOOL supported = 0;
        if (SUCCEEDED(pDxgiFactory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported))))
            dx12->m_TearingSupported = (supported != 0);
    }

    if (dx12->m_TearingSupported)
    {
        dx12->m_SwapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    dx12->m_FullScreenDesc = {};
    dx12->m_FullScreenDesc.RefreshRate.Numerator = dx12->m_Settings.refreshRate;
    dx12->m_FullScreenDesc.RefreshRate.Denominator = 1;
    dx12->m_FullScreenDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
    dx12->m_FullScreenDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    dx12->m_FullScreenDesc.Windowed = true;

    dx12->m_hWnd = hWnd;

    RefCountPtr<IDXGISwapChain1> pSwapChain1;
    auto hr = dx12->m_DxgiFactory2->CreateSwapChainForHwnd(dx12->m_GraphicsQueue, dx12->m_hWnd, &dx12->m_SwapChainDesc, &dx12->m_FullScreenDesc, nullptr, &pSwapChain1);
    HR_ASSERT(hr, "Failed to create swap chain for HWND");

    hr = pSwapChain1->QueryInterface(IID_PPV_ARGS(&dx12->m_SwapChain));
    HR_ASSERT(hr, "Failed to get IDXGISwapChain3 interface");

    create_render_targets(dx12);

    hr = dx12->m_Device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx12->m_FrameFence));
    HR_ASSERT(hr, "Failed to create frame fence");

    for(UINT bufferIndex = 0; bufferIndex < dx12->m_SwapChainDesc.BufferCount; bufferIndex++)
    {
        dx12->m_FrameFenceEvents.push_back( CreateEvent(nullptr, false, true, nullptr) );
    }
}

void destroy_device_and_swapchain(RendererBackend* backend) {
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ERROR("RendererBackendDX12 is null");
        return;
    }

    dx12->m_RhiSwapChainBuffers.clear();

    release_render_targets(dx12);

    dx12->m_Device = nullptr;

    for (auto fenceEvent : dx12->m_FrameFenceEvents)
    {
        WaitForSingleObject(fenceEvent, INFINITE);
        CloseHandle(fenceEvent);
    }

    dx12->m_FrameFenceEvents.clear();

    if (dx12->m_SwapChain)
    {
        dx12->m_SwapChain->SetFullscreenState(false, nullptr);
    }

    dx12->m_SwapChainBuffers.clear();

    dx12->m_FrameFence = nullptr;
    dx12->m_SwapChain = nullptr;
    dx12->m_GraphicsQueue = nullptr;
    dx12->m_ComputeQueue = nullptr;
    dx12->m_CopyQueue = nullptr;
    dx12->m_Device12 = nullptr;
}

void renderer_backend_shutdown(RendererBackend* backend) {
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ERROR("RendererBackendDX12 is null");
        return;
    }

    dx12->m_SwapChainFramebuffers.clear();

    destroy_device_and_swapchain(backend);
}

uint32_t* renderer_get_frame_index(RendererBackend* backend) {
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return nullptr;
    }
    return &dx12->m_FrameIndex;
}

nvrhi::ITexture* GetCurrentBackBuffer(RendererBackend* backend)
{
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return nullptr;
    }
    return dx12->m_RhiSwapChainBuffers[dx12->m_SwapChain->GetCurrentBackBufferIndex()];
}

nvrhi::ITexture* GetBackBuffer(RendererBackend* backend, uint32_t index)
{
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return nullptr;
    }

    if (index < dx12->m_RhiSwapChainBuffers.size())
        return dx12->m_RhiSwapChainBuffers[index];
    return nullptr;
}

uint32_t GetCurrentBackBufferIndex(RendererBackend* backend)
{
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return 0;
    }

    return dx12->m_SwapChain->GetCurrentBackBufferIndex();
}

uint32_t GetBackBufferCount(RendererBackend* backend)
{
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return 0;
    }

    return dx12->m_SwapChainDesc.BufferCount;
}

void BackBufferResizing(RendererBackend* backend)
{
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return;
    }

    dx12->m_SwapChainFramebuffers.clear();
}

void BackBufferResized(RendererBackend* backend)
{
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return;
    }

    uint32_t backBufferCount = GetBackBufferCount(backend);
    dx12->m_SwapChainFramebuffers.resize(backBufferCount);
    for (uint32_t index = 0; index < backBufferCount; index++)
    {
        dx12->m_SwapChainFramebuffers[index] = dx12->m_Device->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(GetBackBuffer(backend, index)));
    }
}

bool renderer_begin_frame(RendererBackend* backend)
{
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 newSwapChainDesc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC newFullScreenDesc;
    if (SUCCEEDED(dx12->m_SwapChain->GetDesc1(&newSwapChainDesc)) && SUCCEEDED(dx12->m_SwapChain->GetFullscreenDesc(&newFullScreenDesc)))
    {
        if (dx12->m_FullScreenDesc.Windowed != newFullScreenDesc.Windowed)
        {
            BackBufferResizing(backend);

            dx12->m_FullScreenDesc = newFullScreenDesc;
            dx12->m_SwapChainDesc = newSwapChainDesc;
            dx12->m_Settings.backBufferWidth = newSwapChainDesc.Width;
            dx12->m_Settings.backBufferHeight = newSwapChainDesc.Height;

            resize_swapchain(dx12);
            BackBufferResized(backend);
        }

    }

    auto bufferIndex = dx12->m_SwapChain->GetCurrentBackBufferIndex();

    WaitForSingleObject(dx12->m_FrameFenceEvents[bufferIndex], INFINITE);

    return true;
}

void renderer_render(RendererBackend* backend) {
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return;
    }

    nvrhi::IFramebuffer* framebuffer = dx12->m_SwapChainFramebuffers[GetCurrentBackBufferIndex(backend)];

    //TODO: here we would use the framebuffer for rendering stuff from the renderer interface data
}

bool renderer_present(RendererBackend* backend)
{
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ERROR("RendererBackendDX12 is null");
        return false;
    }

    auto bufferIndex = dx12->m_SwapChain->GetCurrentBackBufferIndex();

    UINT presentFlags = 0;
    if (!dx12->m_Settings.vsyncEnabled && dx12->m_FullScreenDesc.Windowed && dx12->m_TearingSupported)
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;

    HRESULT result = dx12->m_SwapChain->Present(dx12->m_Settings.vsyncEnabled ? 1 : 0, presentFlags);

    dx12->m_FrameFence->SetEventOnCompletion(dx12->m_FrameCount, dx12->m_FrameFenceEvents[bufferIndex]);
    dx12->m_GraphicsQueue->Signal(dx12->m_FrameFence, dx12->m_FrameCount);
    dx12->m_FrameCount++;
    return SUCCEEDED(result);
}

void renderer_update_avg_frame_time(RendererBackend* backend, double elapsedTime)
{
    const auto dx12 = reinterpret_cast<RendererBackendDX12 *>(backend);
    if (!dx12) {
        SM_ERROR("RendererBackendDX12 is null");
        return;
    }

    dx12->m_FrameTimeSum += elapsedTime;
    dx12->m_NumberOfAccumulatedFrames += 1;

    if (dx12->m_FrameTimeSum > dx12->m_AverageTimeUpdateInterval && dx12->m_NumberOfAccumulatedFrames > 0)
    {
        dx12->m_AverageFrameTime = dx12->m_FrameTimeSum / double(dx12->m_NumberOfAccumulatedFrames);
        dx12->m_NumberOfAccumulatedFrames = 0;
        dx12->m_FrameTimeSum = 0.0;
    }
}
