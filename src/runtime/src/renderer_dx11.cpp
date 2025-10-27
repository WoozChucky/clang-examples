#include "renderer_dx11.h"

#include <dxgi1_3.h>
#include <dxgidebug.h>

#include <lib.h>

#include <nvrhi/validation.h>

#define HR_ASSERT(x, msg) SM_ASSERT(SUCCEEDED(x), msg)

// Adjust window rect so that it is centred on the given adapter.  Clamps to fit if it's too big.
static bool move_window_onto_adapter(IDXGIAdapter* targetAdapter, RECT& rect)
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

void report_live_objects()
{
    nvrhi::RefCountPtr<IDXGIDebug> pDebug;
    DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug));

    if (pDebug)
        pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_DETAIL);
}

void release_render_target(RendererBackendDX11* dx11) {
    dx11->m_RhiBackBuffer = nullptr;
    dx11->m_D3D11BackBuffer = nullptr;
}

void BackBufferResizing(RendererBackendDX11* backend)
{
    backend->m_SwapChainFramebuffers.clear();
}

uint32_t GetBackBufferCount()
{
    return 1;
}

nvrhi::ITexture* GetBackBuffer(RendererBackendDX11* backend, uint32_t index)
{
    if (index == 0)
        return backend->m_RhiBackBuffer;
    return nullptr;
}

void BackBufferResized(RendererBackendDX11* backend)
{
    uint32_t backBufferCount = GetBackBufferCount();
    backend->m_SwapChainFramebuffers.resize(backBufferCount);
    for (uint32_t index = 0; index < backBufferCount; index++)
    {
        backend->m_SwapChainFramebuffers[index] = backend->m_Device->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(GetBackBuffer(backend, index)));
    }
}


void create_render_targets(RendererBackendDX11* dx11) {

    release_render_target(dx11);

    const HRESULT hr = dx11->m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&dx11->m_D3D11BackBuffer);  // NOLINT(clang-diagnostic-language-extension-token)
    HR_ASSERT(hr, "Failed to get swap chain back buffer");

    nvrhi::TextureDesc textureDesc;
    textureDesc.width = dx11->m_Settings.backBufferWidth;
    textureDesc.height = dx11->m_Settings.backBufferHeight;
    textureDesc.sampleCount = dx11->m_Settings.swapChainSampleCount;
    textureDesc.sampleQuality = dx11->m_Settings.swapChainSampleQuality;
    textureDesc.format = dx11->m_Settings.swapChainFormat;
    textureDesc.debugName = "SwapChainBuffer";
    textureDesc.isRenderTarget = true;
    textureDesc.isUAV = false;

    dx11->m_RhiBackBuffer = dx11->m_Device->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D11_Resource, static_cast<ID3D11Resource*>(dx11->m_D3D11BackBuffer), textureDesc);
}

void directx11::create_internal_instance(RendererBackend* backend) {
    const auto dx11 = reinterpret_cast<RendererBackendDX11 *>(backend);
    if (!dx11) {
        SM_ERROR("RendererBackendDX12 is null");
        return;
    }

    if (!dx11->m_DxgiFactory1) {
        const auto hRes = CreateDXGIFactory1(IID_PPV_ARGS(&dx11->m_DxgiFactory1));
        HR_ASSERT(SUCCEEDED(hRes), "Failed to create DXGI Factory2");
    }
}

nvrhi::DeviceHandle directx11::create_device(RendererBackend* backend) {

    const auto dx11 = reinterpret_cast<RendererBackendDX11 *>(backend);
    if (!dx11) {
        SM_ERROR("RendererBackendDX12 is null");
        return nullptr;
    }

    // Pick hardware adapter
    if (FAILED(dx11->m_DxgiFactory1->EnumAdapters(0, &dx11->m_DxgiAdapter))) {
        SM_ERROR("Failed to enumerate DX11 adapter");
        return nullptr;
    }

    if (!dx11->m_DxgiAdapter) {
        SM_ASSERT(false, "No suitable hardware adapter found for D3D11.");
        return nullptr;
    }

    DXGI_ADAPTER_DESC aDesc;

    dx11->m_DxgiAdapter->GetDesc(&aDesc);

    UINT createFlags = 0;
#if defined(_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    constexpr D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;

    const HRESULT hr = D3D11CreateDevice(
        dx11->m_DxgiAdapter, // pAdapter
        D3D_DRIVER_TYPE_UNKNOWN, // DriverType
        nullptr, // Software
        createFlags, // Flags
        &featureLevel, // pFeatureLevels
        1, // FeatureLevels
        D3D11_SDK_VERSION, // SDKVersion
        &dx11->m_Device11, // ppDevice
        nullptr, // pFeatureLevel
        &dx11->m_ImmediateContext // ppImmediateContext
    );

    HR_ASSERT(hr, "Failed to create D3D11 device");

    dx11->m_MessageCallback = new DebugMessageCallback(); // reinterpret_cast<DebugMessageCallback *>(bump_alloc(persistentStorage, sizeof(DebugMessageCallback)));
    dx11->m_DeviceDesc.messageCallback = dx11->m_MessageCallback;
    dx11->m_DeviceDesc.context = dx11->m_ImmediateContext;
    dx11->m_DeviceDesc.aftermathEnabled = false;

    nvrhi::DeviceHandle nvrhiDevice = nvrhi::d3d11::createDevice(dx11->m_DeviceDesc);

#if defined(_DEBUG)
    nvrhiDevice = nvrhi::validation::createValidationLayer(nvrhiDevice);
#endif

    dx11->m_Device = nvrhiDevice;
    dx11->m_FrameCount = 1;
    return nvrhiDevice;
}

void directx11::create_swapchain(RendererBackend* backend, HWND hWnd, const int width, const int height) {
    const auto dx11 = reinterpret_cast<RendererBackendDX11 *>(backend);
    if (!dx11) {
        SM_ERROR("RendererBackendDX11 is null");
        return;
    }

    dx11->m_Settings.backBufferWidth = width;
    dx11->m_Settings.backBufferHeight = height;

    UINT windowStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;

    RECT rect = { 0, 0, LONG(width), LONG(height) };
    AdjustWindowRect(&rect, windowStyle, FALSE);

    ZeroMemory(&dx11->m_SwapChainDesc, sizeof(dx11->m_SwapChainDesc));
    dx11->m_SwapChainDesc.BufferCount = dx11->m_Settings.swapChainBufferCount;
    dx11->m_SwapChainDesc.BufferDesc.Width = width;
    dx11->m_SwapChainDesc.BufferDesc.Height = height;
    dx11->m_SwapChainDesc.BufferDesc.RefreshRate.Numerator = dx11->m_Settings.refreshRate;
    dx11->m_SwapChainDesc.BufferDesc.RefreshRate.Denominator = 0;
    dx11->m_SwapChainDesc.BufferUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
    dx11->m_SwapChainDesc.OutputWindow = hWnd;
    dx11->m_SwapChainDesc.SampleDesc.Count = dx11->m_Settings.swapChainSampleCount;
    dx11->m_SwapChainDesc.SampleDesc.Quality = dx11->m_Settings.swapChainSampleQuality;
    dx11->m_SwapChainDesc.Windowed = TRUE;
    dx11->m_SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    dx11->m_SwapChainDesc.Flags = 0;

    switch (dx11->m_Settings.swapChainFormat)
    {
        case nvrhi::Format::SRGBA8_UNORM:
            dx11->m_SwapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case nvrhi::Format::SBGRA8_UNORM:
            dx11->m_SwapChainDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        default:
            dx11->m_SwapChainDesc.BufferDesc.Format = nvrhi::d3d11::convertFormat(dx11->m_Settings.swapChainFormat);
            break;
    }

    dx11->m_hWnd = hWnd;


    auto hr = dx11->m_DxgiFactory1->CreateSwapChain(dx11->m_Device11, &dx11->m_SwapChainDesc, &dx11->m_SwapChain);
    HR_ASSERT(hr, "Failed to create swap chain for HWND");

    create_render_targets(dx11);
}

void directx11::renderer_resize_swapchain(RendererBackend* backend, const int width, const int height) {
    const auto dx11 = reinterpret_cast<RendererBackendDX11 *>(backend);
    if (!dx11) {
        SM_ERROR("RendererBackendDX11 is null");
        return;
    }

    dx11->m_Settings.backBufferWidth = width;
    dx11->m_Settings.backBufferHeight = height;
    dx11->m_ResizeRequested = true;
}

void resize_swapchain(RendererBackendDX11* backend) {
    release_render_target(backend);

    if (!backend->m_SwapChain)
        return;

    const HRESULT hr = backend->m_SwapChain->ResizeBuffers(
        backend->m_Settings.swapChainBufferCount,
        backend->m_Settings.backBufferWidth,
        backend->m_Settings.backBufferHeight,
        backend->m_SwapChainDesc.BufferDesc.Format,
        backend->m_SwapChainDesc.Flags);
    HR_ASSERT(hr, "Failed to resize swap chain buffers");

    create_render_targets(backend);
}

uint32_t* directx11::renderer_get_frame_index(RendererBackend* backend) {
    const auto dx11 = reinterpret_cast<RendererBackendDX11 *>(backend);
    if (!dx11) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return nullptr;
    }
    return &dx11->m_FrameIndex;
}

uint32_t GetCurrentBackBufferIndex(RendererBackendDX11*)
{
    return 0;
}

nvrhi::IFramebuffer* directx11::renderer_get_framebuffer(RendererBackend* backend, int32_t index) {
    const auto dx11 = reinterpret_cast<RendererBackendDX11 *>(backend);
    if (!dx11) {
        SM_ASSERT(false, "RendererBackendDX12 is null");
        return nullptr;
    }

    if (index < 0) {
        index = GetCurrentBackBufferIndex(dx11); // NOLINT(*-narrowing-conversions)
    }
    return dx11->m_SwapChainFramebuffers[index];
}

bool directx11::renderer_begin_frame(RendererBackend* backend) {
    const auto dx11 = reinterpret_cast<RendererBackendDX11 *>(backend);
    if (!dx11) {
        SM_ERROR("RendererBackendDX12 is null");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC newSwapChainDesc;
    if (SUCCEEDED(dx11->m_SwapChain->GetDesc(&newSwapChainDesc)))
    {
        if (dx11->m_SwapChainDesc.Windowed != newSwapChainDesc.Windowed ||
            dx11->m_ResizeRequested)
        {
            dx11->m_ResizeRequested = false;
            BackBufferResizing(dx11);

            dx11->m_SwapChainDesc = newSwapChainDesc;

            //if (newSwapChainDesc.Windowed)
            //    glfwSetWindowMonitor(m_Window, nullptr, 50, 50, newSwapChainDesc.BufferDesc.Width, newSwapChainDesc.BufferDesc.Height, 0);

            resize_swapchain(dx11);
            BackBufferResized(dx11);
        }
    }
    return true;
}

bool directx11::renderer_present(RendererBackend* backend) {
    const auto dx11 = reinterpret_cast<RendererBackendDX11 *>(backend);
    if (!dx11) {
        SM_ERROR("RendererBackendDX12 is null");
        return false;
    }

    HRESULT result = dx11->m_SwapChain->Present(dx11->m_Settings.vsyncEnabled ? 1 : 0, 0);
    return SUCCEEDED(result);
}

void directx11::renderer_backend_shutdown(RendererBackend* backend) {

    const auto dx11 = reinterpret_cast<RendererBackendDX11 *>(backend);
    if (!dx11) {
        SM_ERROR("RendererBackendDX12 is null");
        return;
    }

    dx11->m_SwapChainFramebuffers.clear();

    dx11->m_RhiBackBuffer = nullptr;
    dx11->m_Device = nullptr;

    if (dx11->m_SwapChain) {
        dx11->m_SwapChain->SetFullscreenState(false, nullptr);
    }

    release_render_target(dx11);

    dx11->m_SwapChain = nullptr;
    dx11->m_ImmediateContext = nullptr;
    dx11->m_Device = nullptr;

}

void directx11::renderer_update_avg_frame_time(RendererBackend* backend, double elapsedTime)
{
    const auto dx11 = reinterpret_cast<RendererBackendDX11 *>(backend);
    if (!dx11) {
        SM_ERROR("RendererBackendDX12 is null");
        return;
    }

    dx11->m_FrameTimeSum += elapsedTime;
    dx11->m_NumberOfAccumulatedFrames += 1;

    if (dx11->m_FrameTimeSum > dx11->m_AverageTimeUpdateInterval && dx11->m_NumberOfAccumulatedFrames > 0)
    {
        dx11->m_AverageFrameTime = dx11->m_FrameTimeSum / double(dx11->m_NumberOfAccumulatedFrames);
        dx11->m_NumberOfAccumulatedFrames = 0;
        dx11->m_FrameTimeSum = 0.0;
    }
}