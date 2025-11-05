#include "RendererBackendDX12.h"

#include <dxgidebug.h>
#include <lib.h>
#include <nvrhi/validation.h>

#ifndef GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif // GLFW_EXPOSE_NATIVE_WIN32

#include <d3dcompiler.h>
#include <GLFW/glfw3native.h>

#define HR_ASSERT(x, msg) SM_ASSERT(SUCCEEDED(x), msg)

bool RendererBackendDX12::Init() {
    if (!m_DxgiFactory2) {
        EnableDebugLayerIfAvailable();

        UINT flags = 0;
#if defined(_DEBUG)
        flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
        const auto hRes = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_DxgiFactory2));
        SM_ASSERT(SUCCEEDED(hRes), "Failed to create DXGI Factory2");
    }

    return true;
}

void RendererBackendDX12::Shutdown(uint32_t timeoutMs) {
    m_RhiSwapChainBuffers.clear();

    DestroyDeviceAndSwapChain();
}

RendererAPI RendererBackendDX12::GetAPI() const {
    return RendererAPI::DirectX12;
}

nvrhi::DeviceHandle RendererBackendDX12::CreateDevice() {

    m_DxgiAdapter = PickHardwareAdapter(m_DxgiFactory2);
    if (!m_DxgiAdapter) {
        // No hardware adapter found; user might want to fall back to WARP.
        // We throw here to keep this example focused. In production, consider creating WARP adapter:
        // factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter))
        SM_ASSERT(false, "No suitable hardware adapter found for D3D12.");
        return nullptr;
    }

    HRESULT hr = D3D12CreateDevice(m_DxgiAdapter, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&m_Device12));
    HR_ASSERT(hr, "Failed to create D3D12 device");

#if defined(_DEBUG)
    {
        RefCountPtr<ID3D12InfoQueue> pInfoQueue;
        m_Device12->QueryInterface(&pInfoQueue);

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
#endif

    // Sanity: confirm CheckFeatureSupport & capability tiers (DXR/VRS/Mesh/SamplerFeedback).
    ValidateDX12UltimateCapabilities(m_Device12);

    D3D12_COMMAND_QUEUE_DESC queueDesc;
    ZeroMemory(&queueDesc, sizeof(queueDesc));
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.NodeMask = 1;

    hr = m_Device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_GraphicsQueue));
    HR_ASSERT(hr, "Failed to create graphics queue");
    hr = m_GraphicsQueue->SetName(L"Graphics Queue");
    HR_ASSERT(hr, "Failed to name graphics queue");

    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    hr = m_Device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_ComputeQueue));
    HR_ASSERT(hr, "Failed to create compute queue");
    hr = m_ComputeQueue->SetName(L"Compute Queue");
    HR_ASSERT(hr, "Failed to name compute queue");

    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    hr = m_Device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CopyQueue));
    HR_ASSERT(hr, "Failed to create copy queue");
    hr = m_CopyQueue->SetName(L"Copy Queue");
    HR_ASSERT(hr, "Failed to name copy queue");

    m_DeviceDesc.errorCB = &DefaultMessageCallback::GetInstance();
    m_DeviceDesc.logBufferLifetime = true;
    m_DeviceDesc.renderTargetViewHeapSize = 1024;
    m_DeviceDesc.depthStencilViewHeapSize = 1024;
    m_DeviceDesc.shaderResourceViewHeapSize = 16384;
    m_DeviceDesc.samplerHeapSize = 1024;
    m_DeviceDesc.maxTimerQueries = 256;
    m_DeviceDesc.enableHeapDirectlyIndexed = false;
    m_DeviceDesc.aftermathEnabled = false;

    m_DeviceDesc.pDevice = m_Device12;
    m_DeviceDesc.pGraphicsCommandQueue = m_GraphicsQueue;
    m_DeviceDesc.pComputeCommandQueue = m_ComputeQueue;
    m_DeviceDesc.pCopyCommandQueue = m_CopyQueue;

    SM_TRACE("[D3D12] Created graphics (DIRECT) queue, compute queue, and copy queue.");

    nvrhi::DeviceHandle nvrhiDevice = nvrhi::d3d12::createDevice(m_DeviceDesc);

#if defined(_DEBUG)
    nvrhiDevice = nvrhi::validation::createValidationLayer(nvrhiDevice);
#endif

    m_Device = nvrhiDevice;
    m_FrameCount = 1;
    return nvrhiDevice;
}

void RendererBackendDX12::CreateSwapChain(const uint32_t width, const uint32_t height) {
    UINT windowStyle = (WS_OVERLAPPEDWINDOW | WS_VISIBLE);

    m_Settings.backBufferWidth = width;
    m_Settings.backBufferHeight = height;

    RECT rect = { 0, 0, LONG(m_Settings.backBufferWidth), LONG(m_Settings.backBufferHeight) };
    AdjustWindowRect(&rect, windowStyle, FALSE);

    if (MoveWindowOntoAdapter(m_DxgiAdapter, rect))
    {
        glfwSetWindowPos(m_Window, rect.left, rect.top);
    }

    m_hWnd = glfwGetWin32Window(m_Window);

    HRESULT hr = E_FAIL;

    RECT clientRect;
    GetClientRect(m_hWnd, &clientRect);
    UINT calculatedWidth = clientRect.right - clientRect.left;
    UINT calculatedHeight = clientRect.bottom - clientRect.top;

    ZeroMemory(&m_SwapChainDesc, sizeof(m_SwapChainDesc));
    m_SwapChainDesc.Width = calculatedWidth;
    m_SwapChainDesc.Height = calculatedHeight;
    m_SwapChainDesc.SampleDesc.Count = m_Settings.swapChainSampleCount;
    m_SwapChainDesc.SampleDesc.Quality = m_Settings.swapChainSampleQuality;
    m_SwapChainDesc.BufferUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
    m_SwapChainDesc.BufferCount = m_Settings.swapChainBufferCount;
    m_SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    m_SwapChainDesc.Flags = 0;
    switch (m_Settings.swapChainFormat)
    {
        case nvrhi::Format::SRGBA8_UNORM:
            m_SwapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case nvrhi::Format::SBGRA8_UNORM:
            m_SwapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        default:
            m_SwapChainDesc.Format = nvrhi::d3d12::convertFormat(m_Settings.swapChainFormat);
            break;
    }

    RefCountPtr<IDXGIFactory5> pDxgiFactory5;
    if (SUCCEEDED(m_DxgiFactory2->QueryInterface(IID_PPV_ARGS(&pDxgiFactory5))))
    {
        BOOL supported = 0;
        if (SUCCEEDED(pDxgiFactory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported))))
            m_TearingSupported = (supported != 0);
    }

    if (m_TearingSupported)
    {
        m_SwapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    m_FullScreenDesc = {};
    m_FullScreenDesc.RefreshRate.Numerator = m_Settings.refreshRate;
    m_FullScreenDesc.RefreshRate.Denominator = 1;
    m_FullScreenDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
    m_FullScreenDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    m_FullScreenDesc.Windowed = true;

    RefCountPtr<IDXGISwapChain1> pSwapChain1;
    hr = m_DxgiFactory2->CreateSwapChainForHwnd(m_GraphicsQueue, m_hWnd, &m_SwapChainDesc, &m_FullScreenDesc, nullptr, &pSwapChain1);
    HR_ASSERT(hr, "Failed to create swap chain for HWND");

    hr = pSwapChain1->QueryInterface(IID_PPV_ARGS(&m_SwapChain));
    HR_ASSERT(hr, "Failed to get IDXGISwapChain3 interface");

    CreateRenderTargets();

    hr = m_Device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_FrameFence));
    HR_ASSERT(hr, "Failed to create frame fence");

    for(UINT bufferIndex = 0; bufferIndex < m_SwapChainDesc.BufferCount; bufferIndex++)
    {
        m_FrameFenceEvents.push_back( CreateEvent(nullptr, false, true, nullptr) );
    }

    BackBufferResized();
}

nvrhi::CommandListHandle RendererBackendDX12::CreateCommandList() {
    if (!m_CommandList) {
        m_CommandList = m_Device->createCommandList();
    }

    return m_CommandList;
}

void RendererBackendDX12::ResizeSwapChain(const uint32_t width, const uint32_t height) {
    m_Settings.backBufferWidth = width;
    m_Settings.backBufferHeight = height;
    m_ResizeRequested = true;
}

nvrhi::ITexture * RendererBackendDX12::GetCurrentBackBuffer() {
    return m_RhiSwapChainBuffers[m_SwapChain->GetCurrentBackBufferIndex()];
}

nvrhi::ITexture * RendererBackendDX12::GetBackBuffer(uint32_t index) {
    if (index < m_RhiSwapChainBuffers.size())
        return m_RhiSwapChainBuffers[index];
    return nullptr;
}

uint32_t RendererBackendDX12::GetCurrentBackBufferIndex() {
    return m_SwapChain->GetCurrentBackBufferIndex();
}

uint32_t RendererBackendDX12::GetBackBufferCount() {
    return m_SwapChainDesc.BufferCount;
}

uint32_t * RendererBackendDX12::GetFrameIndexPtr() {
    return &m_FrameIndex;
}

nvrhi::IFramebuffer * RendererBackendDX12::GetFrameBuffer(int32_t index) {
    if (index < 0) {
        index = static_cast<int32_t>(GetCurrentBackBufferIndex());
    }
    return m_SwapChainFramebuffers[index];
}

bool RendererBackendDX12::BeginFrame() {
    DXGI_SWAP_CHAIN_DESC1 newSwapChainDesc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC newFullScreenDesc;
    if (SUCCEEDED(m_SwapChain->GetDesc1(&newSwapChainDesc)) && SUCCEEDED(m_SwapChain->GetFullscreenDesc(&newFullScreenDesc)))
    {
        if (m_FullScreenDesc.Windowed != newFullScreenDesc.Windowed || m_ResizeRequested)
        {
            m_ResizeRequested = false;
            BackBufferResizing();

            m_FullScreenDesc = newFullScreenDesc;
            m_SwapChainDesc = newSwapChainDesc;

            ResizeSwapChain();
            BackBufferResized();
            SM_TRACE("Swap chain resized: %ux%u", m_Settings.backBufferWidth, m_Settings.backBufferHeight);
        }

    }

    const UINT bufferIndex = m_SwapChain->GetCurrentBackBufferIndex();

    WaitForSingleObject(m_FrameFenceEvents[bufferIndex], INFINITE);
    return true;
}

bool RendererBackendDX12::Present() {
    auto bufferIndex = m_SwapChain->GetCurrentBackBufferIndex();

    UINT presentFlags = 0;
    if (!m_Settings.vsyncEnabled && m_FullScreenDesc.Windowed && m_TearingSupported)
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;

    HRESULT result = m_SwapChain->Present(m_Settings.vsyncEnabled ? 1 : 0, presentFlags);
    if (result == DXGI_STATUS_OCCLUDED)
    {
        SM_TRACE("DXGI_STATUS_OCCLUDED");
    }

    m_FrameFence->SetEventOnCompletion(m_FrameCount, m_FrameFenceEvents[bufferIndex]);
    m_GraphicsQueue->Signal(m_FrameFence, m_FrameCount);
    m_FrameCount++;
    return SUCCEEDED(result);
}

nvrhi::ShaderHandle RendererBackendDX12::CreateShaderFromMemory(nvrhi::ShaderType shaderType, const char *content,
    size_t contentSize, const char *entryPoint, const char *targetName) {
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.debugName = "ShaderFromMemory";
    shaderDesc.shaderType = shaderType;
    shaderDesc.entryName = entryPoint;

    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompile(
        content, strlen(content),
        nullptr,
        nullptr, nullptr,
        entryPoint,
        targetName,
        flags, 0,
        &bytecode, &errors
    );
    if (FAILED(hr)) {
        if (errors) {
            char temp[1024];
            size_t size = std::min<size_t>(errors->GetBufferSize(), sizeof(temp) - 1);
            memcpy(temp, errors->GetBufferPointer(), size);
            temp[size] = 0;
            SM_ERROR("Shader compile error: %s", (char*)errors->GetBufferPointer());
        }
        SM_ASSERT(false, "Shader compile failed");
    }

    if(!bytecode)
        return nullptr;

    return m_Device->createShader(shaderDesc, bytecode->GetBufferPointer(), bytecode->GetBufferSize());
}

void RendererBackendDX12::DestroyDeviceAndSwapChain() {
    m_RhiSwapChainBuffers.clear();

    ReleaseRenderTargets();

    m_Device = nullptr;

    for (auto fenceEvent : m_FrameFenceEvents)
    {
        WaitForSingleObject(fenceEvent, INFINITE);
        CloseHandle(fenceEvent);
    }

    m_FrameFenceEvents.clear();

    if (m_SwapChain)
    {
        m_SwapChain->SetFullscreenState(false, nullptr);
    }

    m_SwapChainBuffers.clear();

    m_FrameFence = nullptr;
    m_SwapChain = nullptr;
    m_GraphicsQueue = nullptr;
    m_ComputeQueue = nullptr;
    m_CopyQueue = nullptr;
    m_Device12 = nullptr;
}

// Private Impls
void RendererBackendDX12::CreateRenderTargets() {
    m_SwapChainBuffers.resize(m_SwapChainDesc.BufferCount);
    m_RhiSwapChainBuffers.resize(m_SwapChainDesc.BufferCount);

    for(UINT n = 0; n < m_SwapChainDesc.BufferCount; n++)
    {
        const HRESULT hr = m_SwapChain->GetBuffer(n, IID_PPV_ARGS(&m_SwapChainBuffers[n]));
        HR_ASSERT(hr, "Failed to get swap chain buffer");

        nvrhi::TextureDesc textureDesc;
        textureDesc.width = m_Settings.backBufferWidth;
        textureDesc.height = m_Settings.backBufferHeight;
        textureDesc.sampleCount = m_Settings.swapChainSampleCount;
        textureDesc.sampleQuality = m_Settings.swapChainSampleQuality;
        textureDesc.format = m_Settings.swapChainFormat;
        textureDesc.debugName = "SwapChainBuffer";
        textureDesc.isRenderTarget = true;
        textureDesc.isUAV = false;
        textureDesc.initialState = nvrhi::ResourceStates::Present;
        textureDesc.keepInitialState = true;

        m_RhiSwapChainBuffers[n] = m_Device->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(m_SwapChainBuffers[n]), textureDesc);
    }
}

void RendererBackendDX12::ReleaseRenderTargets() {
    if (m_Device)
    {
        // Make sure that all frames have finished rendering
        m_Device->waitForIdle();

        // Release all in-flight references to the render targets
        m_Device->runGarbageCollection();
    }

    // Set the events so that WaitForSingleObject in OneFrame will not hang later
    for(auto e : m_FrameFenceEvents)
        SetEvent(e);

    // Release the old buffers because ResizeBuffers requires that
    m_RhiSwapChainBuffers.clear();
    m_SwapChainBuffers.clear();
}

void RendererBackendDX12::ResizeSwapChain() {
    ReleaseRenderTargets();

    if (!m_Device)
        return;

    if (!m_SwapChain)
        return;

    const HRESULT hr = m_SwapChain->ResizeBuffers(m_Settings.swapChainBufferCount,
                                            m_Settings.backBufferWidth,
                                            m_Settings.backBufferHeight,
                                            m_SwapChainDesc.Format,
                                            m_SwapChainDesc.Flags);
    HR_ASSERT(hr, "Failed to resize swap chain buffers");

    CreateRenderTargets();
}

void RendererBackendDX12::BackBufferResizing() {
    m_SwapChainFramebuffers.clear();
}

void RendererBackendDX12::BackBufferResized() {
    uint32_t backBufferCount = GetBackBufferCount();
    m_SwapChainFramebuffers.resize(backBufferCount);
    for (uint32_t index = 0; index < backBufferCount; index++)
    {
        m_SwapChainFramebuffers[index] = m_Device->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(GetBackBuffer(index)));
    }
}


// Static Impls

void RendererBackendDX12::EnableDebugLayerIfAvailable() {
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (SUCCEEDED(hr)) {
        debug->EnableDebugLayer();
        // If you want GPU-based validation (slower), you can enable it here via ID3D12Debug1.
        ComPtr<ID3D12Debug1> debug1;
        if (SUCCEEDED(debug.As(&debug1))) debug1->SetEnableGPUBasedValidation(TRUE);
        SM_TRACE("D3D12 Debug layer enabled.");
    } else {
        SM_TRACE("D3D12 Debug layer not available.");
    }
#else
    (void)0;
#endif
}

RefCountPtr<IDXGIAdapter1> RendererBackendDX12::PickHardwareAdapter(const RefCountPtr<IDXGIFactory2>& factory) {
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

void RendererBackendDX12::ReportLiveObjects()
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

void RendererBackendDX12::ValidateDX12UltimateCapabilities(ID3D12Device* device) {
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

bool RendererBackendDX12::MoveWindowOntoAdapter(IDXGIAdapter* targetAdapter, RECT& rect)
{
    assert(targetAdapter != nullptr);

    HRESULT hres = S_OK;
    unsigned int outputNo = 0;
    while (SUCCEEDED(hres))
    {
        nvrhi::RefCountPtr<IDXGIOutput> pOutput;
        hres = targetAdapter->EnumOutputs(outputNo++, &pOutput);

        if (SUCCEEDED(hres) && pOutput)
        {
            DXGI_OUTPUT_DESC OutputDesc;
            pOutput->GetDesc(&OutputDesc);
            const RECT desktop = OutputDesc.DesktopCoordinates;
            const int centreX = (int)desktop.left + (int)(desktop.right - desktop.left) / 2;
            const int centreY = (int)desktop.top + (int)(desktop.bottom - desktop.top) / 2;
            const int winW = rect.right - rect.left;
            const int winH = rect.bottom - rect.top;
            const int left = centreX - winW / 2;
            const int right = left + winW;
            const int top = centreY - winH / 2;
            const int bottom = top + winH;
            rect.left = std::max(left, (int)desktop.left);
            rect.right = std::min(right, (int)desktop.right);
            rect.bottom = std::min(bottom, (int)desktop.bottom);
            rect.top = std::max(top, (int)desktop.top);

            // If there is more than one output, go with the first found.  Multi-monitor support could go here.
            return true;
        }
    }

    return false;
}