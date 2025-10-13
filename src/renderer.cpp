#include "renderer.h"

#include "render.h"

#include <wrl.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_5.h>

#include "image.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;

typedef struct RendererContext {
    ComPtr<ID3D11Device>           Device;
    ComPtr<ID3D11DeviceContext>    DeviceContext;
    ComPtr<IDXGISwapChain>         SwapChain;
    ComPtr<ID3D11RenderTargetView> RenderTargetView;
    ComPtr<ID3D11DepthStencilView> DepthStencilView;
    ComPtr<ID3D11DepthStencilState> DepthState;
    ComPtr<ID3D11RasterizerState>   RasterState;
    ComPtr<ID3D11Texture2D>        DepthStencilTexture;
    ComPtr<ID3D11VertexShader>     VertexShader;
    ComPtr<ID3D11PixelShader>      PixelShader;
    ComPtr<ID3D11InputLayout>      InputLayout;
    ComPtr<ID3D11Buffer>           VertexBuffer;
    ComPtr<ID3D11Buffer>           IndexBuffer;
    ComPtr<ID3D11Buffer>           ConstantBuffer;
    ComPtr<ID3D11Buffer>           AtlasImageBuffer;
    int Width;
    int Height;
    bool m_SwapChainOccluded;
    bool m_VSync;
    bool m_TearingSupported;
} RendererContext;

static RendererContext* g_RendererContext;

static const char* FeatureLevelToString(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        case D3D_FEATURE_LEVEL_10_1: return "10_1";
        case D3D_FEATURE_LEVEL_10_0: return "10_0";
        case D3D_FEATURE_LEVEL_9_3:  return "9_3";
        case D3D_FEATURE_LEVEL_9_2:  return "9_2";
        case D3D_FEATURE_LEVEL_9_1:  return "9_1";
        default: return "Unknown";
    }
}

static const char* VendorNameFromPCIVendorId(UINT vendorId) {
    switch (vendorId) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: // fallthrough
        case 0x1022: return "AMD";       // 0x1022 is AMD (CPUs/APUs), 0x1002 is ATI/AMD GPUs
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft"; // WARP/Software adapter often shows Microsoft
        default: return "UnknownVendor";
    }
}

static bool QueryTearingSupport() {
    BOOL allowTearing = FALSE;
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory1;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1))) &&
        SUCCEEDED(factory1.As(&factory5))) {
        factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing,
            sizeof(allowTearing));
        }
    return allowTearing == TRUE;
}

// HLSL for a colored cube using vertex/index buffers and a constant buffer (MVP)
const char* G_VS_HLSL = R"(
cbuffer PerFrame : register(b0)
{
    float4x4 uModel;
    float4x4 uVP;
};

struct VSIn {
    float3 Pos : POSITION;
    float3 Color : COLOR;
};

struct VSOut {
    float4 Pos : SV_POSITION;
    float4 Color : COLOR;
};

VSOut main(VSIn input) {
    VSOut o;
    float4 wpos = mul(uModel, float4(input.Pos, 1.0f));
    o.Pos = mul(uVP, wpos);         // P * V * M * v, where uVP = V * P on CPU
    o.Color = float4(input.Color, 1.0f);
    return o;
}
)";

const char* G_PS_HLSL = R"(
struct PSIn {
    float4 Pos : SV_POSITION;
    float4 Color : COLOR;
};
float4 main(PSIn input) : SV_Target {
    return input.Color;
}
)";

void create_render_and_depth_target();
void create_shaders();
void create_cube_buffers();

struct VertexPC {
    float px, py, pz;
    float cr, cg, cb;
};

void renderer_init(int width, int height, void* handle, BumpAllocator* persistentStorage) {

    g_RendererContext = reinterpret_cast<RendererContext *>(bump_alloc(persistentStorage, sizeof(RendererContext)));
    if(!g_RendererContext)
    {
        SM_ERROR("Failed to allocate RendererContext");
        return;
    }
    g_RendererContext->m_VSync = true;
    g_RendererContext->m_TearingSupported = QueryTearingSupport();

    // Feature levels we will accept (highest first)
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL chosenLevel = D3D_FEATURE_LEVEL_11_0;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2; // double-buffer
    sd.OutputWindow = static_cast<HWND>(handle);
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // preferred modern swap effect
    sd.Flags = 0;

  UINT createFlags = 0;
#if defined(_DEBUG)
  // Request debug layer when building in debug - helpful for validation and catching mistakes
  createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    ComPtr<ID3D11Device> deviceRaw;
    ComPtr<ID3D11DeviceContext> contextRaw;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,                    // default adapter
        D3D_DRIVER_TYPE_HARDWARE,   // use hardware
        nullptr,                    // no software rasterizer
        createFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &sd,
        &g_RendererContext->SwapChain,
        &deviceRaw,
        &chosenLevel,
        &contextRaw
    );
    SM_ASSERT(SUCCEEDED(hr), "D3D11CreateDeviceAndSwapChain failed");

    // Device/adapter debug information
    {
        // chosenLevel already set by D3D11CreateDeviceAndSwapChain
        SM_TRACE("D3D11 Feature Level: %s", FeatureLevelToString(chosenLevel));

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(deviceRaw.As(&dxgiDevice)) && dxgiDevice) {
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter) {
                DXGI_ADAPTER_DESC desc = {};
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    // Convert wide char description to UTF-8/ASCII for logging
                    char name[256] = {};
                    size_t outLen = 0;
                    wcstombs_s(&outLen, name, desc.Description, _TRUNCATE);

                    const char* vendorName = VendorNameFromPCIVendorId(desc.VendorId);
                    UINT vramMB = (UINT)(desc.DedicatedVideoMemory / (1024ull * 1024ull));

                    SM_TRACE("Adapter: %s (%s)", name, vendorName);
                    SM_TRACE("  VendorId=0x%04X, DeviceId=0x%04X, SubSysId=0x%08X, Revision=%u",
                             desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision);
                    SM_TRACE("  DedicatedVideoMemory: %u MB", vramMB);

                    // Factory and features (tearing support, if DXGI 1.5+)
                    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
                    if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) && factory) {
                        BOOL allowTearing = FALSE;
                        Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
                        if (SUCCEEDED(factory.As(&factory5)) && factory5) {
                            if (SUCCEEDED(factory5->CheckFeatureSupport(
                                    DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                    &allowTearing,
                                    sizeof(allowTearing)))) {
                                SM_TRACE("DXGI: Present allow tearing: %s", allowTearing ? "Yes" : "No");
                            }
                        }
                    }
                }
            }
        }

        // D3D11 feature queries (optional but handy)
        D3D11_FEATURE_DATA_D3D11_OPTIONS opts = {};
        if (SUCCEEDED(deviceRaw->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof(opts)))) {
            SM_TRACE("D3D11 Options: OutputMergerLogicOp=%s, UAVOnlyRenderingForcedSampleCount=%u",
                     opts.OutputMergerLogicOp ? "Yes" : "No",
                     opts.UAVOnlyRenderingForcedSampleCount);
        }

    #if defined(_DEBUG)
        // Debug layer / info queue (only available if device created with D3D11_CREATE_DEVICE_DEBUG)
        Microsoft::WRL::ComPtr<ID3D11Debug> d3dDebug;
        if (SUCCEEDED(deviceRaw.As(&d3dDebug)) && d3dDebug) {
            Microsoft::WRL::ComPtr<ID3D11InfoQueue> infoQueue;
            if (SUCCEEDED(d3dDebug.As(&infoQueue)) && infoQueue) {
                infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
                SM_TRACE("D3D11 Debug layer: Enabled (break on error/corruption)");
            } else {
                SM_TRACE("D3D11 Debug layer requested, but info queue not available");
            }
        } else {
            SM_TRACE("D3D11 Debug layer: Not available (ensure Graphics Tools/SDK is installed)");
        }
    #endif
    }

    g_RendererContext->Device = deviceRaw;
    g_RendererContext->DeviceContext = contextRaw;
    g_RendererContext->Width = width;
    g_RendererContext->Height = height;
    g_RendererContext->m_SwapChainOccluded = false;

    create_render_and_depth_target();
    create_shaders();
    create_cube_buffers();

    {
        int imageWidth = 0;
        int imageHeight = 0;
        int imageChannels = 0;
        auto* imageData = (unsigned char*) image_load("assets/block_atlas.png", &imageWidth, &imageHeight, &imageChannels);
        SM_ASSERT(imageData, "Failed to load image");
        SM_ASSERT(imageWidth > 0 && imageHeight > 0 && imageChannels > 0, "Invalid image dimensions");

        auto byteWidth = imageWidth * imageHeight * imageChannels;
        SM_TRACE("Loaded image: %dx%d, channels=%d, size=%d bytes", imageWidth, imageHeight, imageChannels, byteWidth);

        // Temp code to create and fill the AtlasImageBuffer (single image)
        D3D11_BUFFER_DESC atlasBufDesc = {};
        atlasBufDesc.ByteWidth = 0;
        atlasBufDesc.Usage = D3D11_USAGE_DEFAULT;
        atlasBufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA atlasBufData = {};
        atlasBufData.pSysMem = imageData;
        HRESULT hr = g_RendererContext->Device->CreateBuffer(&atlasBufDesc, &atlasBufData, &g_RendererContext->AtlasImageBuffer);
        SM_ASSERT(SUCCEEDED(hr), "Failed to create vertex buffer");
    }
}

void create_cube_buffers() {
    // Define a unit cube centered at origin with per-vertex colors
    static const VertexPC vertices[] = {
        // Front face (z = +0.5)
        {-0.5f, -0.5f,  0.5f, 1, 0, 0}, // 0
        { 0.5f, -0.5f,  0.5f, 0, 1, 0}, // 1
        { 0.5f,  0.5f,  0.5f, 0, 0, 1}, // 2
        {-0.5f,  0.5f,  0.5f, 1, 1, 0}, // 3
        // Back face (z = -0.5)
        {-0.5f, -0.5f, -0.5f, 1, 0, 1}, // 4
        { 0.5f, -0.5f, -0.5f, 0, 1, 1}, // 5
        { 0.5f,  0.5f, -0.5f, 1, 1, 1}, // 6
        {-0.5f,  0.5f, -0.5f, 0, 0, 0}, // 7
    };

    static const uint16_t indices[] = {
        // Front
        0,1,2,  0,2,3,
        // Right
        1,5,6,  1,6,2,
        // Back
        5,4,7,  5,7,6,
        // Left
        4,0,3,  4,3,7,
        // Top
        3,2,6,  3,6,7,
        // Bottom
        4,5,1,  4,1,0,
    };

    // Vertex buffer
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = (UINT)sizeof(vertices);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = vertices;
    HRESULT hr = g_RendererContext->Device->CreateBuffer(&vbDesc, &vbData, &g_RendererContext->VertexBuffer);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create vertex buffer");

    // Index buffer
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = (UINT)sizeof(indices);
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = indices;
    hr = g_RendererContext->Device->CreateBuffer(&ibDesc, &ibData, &g_RendererContext->IndexBuffer);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create index buffer");

    // Constant buffer for Model and ViewProj matrices (2 x 16 floats)
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(float) * 32; // two 4x4 matrices
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = 0;
    cbDesc.MiscFlags = 0;
    cbDesc.StructureByteStride = 0;
    hr = g_RendererContext->Device->CreateBuffer(&cbDesc, nullptr, &g_RendererContext->ConstantBuffer);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create constant buffer");
}

void create_render_and_depth_target() {
    // RTV
    ComPtr<ID3D11Texture2D> BackBuffer;
    HRESULT hr = g_RendererContext->SwapChain->GetBuffer(0, IID_PPV_ARGS(&BackBuffer));
    SM_ASSERT(SUCCEEDED(hr), "Failed to get back buffer from swap chain");
    hr = g_RendererContext->Device->CreateRenderTargetView(BackBuffer.Get(), nullptr, &g_RendererContext->RenderTargetView);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create render target view");

    // Depth stencil
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = g_RendererContext->Width;
    desc.Height = g_RendererContext->Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = g_RendererContext->Device->CreateTexture2D(&desc, nullptr, &g_RendererContext->DepthStencilTexture);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create depth stencil texture");
    hr = g_RendererContext->Device->CreateDepthStencilView(g_RendererContext->DepthStencilTexture.Get(), nullptr, &g_RendererContext->DepthStencilView);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create depth stencil view");

    // Depth state: enable for 3D depth testing
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = g_RendererContext->Device->CreateDepthStencilState(&dsd, &g_RendererContext->DepthState);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create depth state");

    // Rasterizer: enable back-face culling (clockwise is front by default in D3D)
    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_BACK;
    rs.FrontCounterClockwise = TRUE; // treat CCW as front face to match our cube indices
    rs.DepthClipEnable = TRUE;
    hr = g_RendererContext->Device->CreateRasterizerState(&rs, &g_RendererContext->RasterState);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create rasterizer state");

    // Initial bind
    g_RendererContext->DeviceContext->OMSetRenderTargets(1, g_RendererContext->RenderTargetView.GetAddressOf(), g_RendererContext->DepthStencilView.Get());
    g_RendererContext->DeviceContext->OMSetDepthStencilState(g_RendererContext->DepthState.Get(), 0);
    g_RendererContext->DeviceContext->RSSetState(g_RendererContext->RasterState.Get());

    // Viewport
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)g_RendererContext->Width; vp.Height = (float)g_RendererContext->Height;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    g_RendererContext->DeviceContext->RSSetViewports(1, &vp);
}

ComPtr<ID3DBlob> CompileShader(const char* Source, const char* Entry, const char* Target) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompile(
        Source, strlen(Source),
        nullptr,
        nullptr, nullptr,
        Entry, Target,
        flags, 0,
        &bytecode, &errors
    );
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA((char*)errors->GetBufferPointer());
        }
        SM_ASSERT(false, "Shader compile failed");
    }
    return bytecode;
}

void create_shaders() {
    // Vertex shader
    ComPtr<ID3DBlob> vsBlob = CompileShader(G_VS_HLSL, "main", "vs_5_0");
    HRESULT hr = g_RendererContext->Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_RendererContext->VertexShader);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create vertex shader");

    // Input layout for POSITION (float3) and COLOR (float3)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = g_RendererContext->Device->CreateInputLayout(
        layout, _countof(layout),
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        &g_RendererContext->InputLayout);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create input layout");

    // Pixel shader
    ComPtr<ID3DBlob> psBlob = CompileShader(G_PS_HLSL, "main", "ps_5_0");
    hr = g_RendererContext->Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_RendererContext->PixelShader);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create pixel shader");
}

void render(RenderData* renderData, pfnRenderUIOverlay uiOverlay) {
    if (g_RendererContext->m_SwapChainOccluded && g_RendererContext->SwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
        // Still occluded, skip rendering
        SM_TRACE("Window minimizing / screen locked...")
        return;
    }
    g_RendererContext->m_SwapChainOccluded = false;

    // Re-bind state each frame (cheap and robust)
    g_RendererContext->DeviceContext->OMSetRenderTargets(1, g_RendererContext->RenderTargetView.GetAddressOf(), g_RendererContext->DepthStencilView.Get());
    g_RendererContext->DeviceContext->OMSetDepthStencilState(g_RendererContext->DepthState.Get(), 0);
    g_RendererContext->DeviceContext->RSSetState(g_RendererContext->RasterState.Get());

    // clear
    float clearColor[4] = { renderData->clearColor.r, renderData->clearColor.g, renderData->clearColor.b, renderData->clearColor.a };
    g_RendererContext->DeviceContext->ClearRenderTargetView(g_RendererContext->RenderTargetView.Get(), clearColor);
    g_RendererContext->DeviceContext->ClearDepthStencilView(g_RendererContext->DepthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    // IA: bind cube vertex/index buffers
    UINT stride = 24; // 3 floats pos (12) + 3 floats color (12)
    UINT offset = 0;
    ID3D11Buffer* vb = g_RendererContext->VertexBuffer.Get();
    g_RendererContext->DeviceContext->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_RendererContext->DeviceContext->IASetIndexBuffer(g_RendererContext->IndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    g_RendererContext->DeviceContext->IASetInputLayout(g_RendererContext->InputLayout.Get());
    g_RendererContext->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set shaders
    g_RendererContext->DeviceContext->VSSetShader(g_RendererContext->VertexShader.Get(), nullptr, 0);
    g_RendererContext->DeviceContext->PSSetShader(g_RendererContext->PixelShader.Get(), nullptr, 0);

    // Update constant buffer with Model and View-Projection matrices
    if (g_RendererContext->ConstantBuffer) {
        float aspect = (g_RendererContext->Height != 0)
            ? ((float)g_RendererContext->Width / (float)g_RendererContext->Height) : 1.0f;
        if (fabsf(renderData->gameCamera.aspectRatio - aspect) > 0.0001f) {
            renderData->gameCamera.aspectRatio = aspect;
            renderData->gameCamera.m_ProjDirty = true;
        }

        glm::mat4 V = renderData->gameCamera.get_view_matrix();
        glm::mat4 P = renderData->gameCamera.get_projection_matrix();
        glm::mat4 VP = P * V;

        struct CBufData { glm::mat4 Model; glm::mat4 VP; };
        CBufData cb = { renderData->modelMatrix3D, VP };

        auto has_nan = [](const glm::mat4& m){
            const float* p = (const float*)&m;
            for (int i=0;i<16;i++) if (!std::isfinite(p[i])) return true;
            return false;
        };
        if (has_nan(cb.Model) || has_nan(cb.VP)) {
            SM_ERROR("Matrix contains NaN/Inf; skipping draw this frame");
        }

        g_RendererContext->DeviceContext->UpdateSubresource(
            g_RendererContext->ConstantBuffer.Get(), 0, nullptr, &cb, 0, 0);
        ID3D11Buffer* cbs[] = { g_RendererContext->ConstantBuffer.Get() };
        g_RendererContext->DeviceContext->VSSetConstantBuffers(0, 1, cbs);
    }

    // Draw indexed (36 indices for a cube)
    g_RendererContext->DeviceContext->DrawIndexed(36, 0, 0);

    if (uiOverlay) {
        uiOverlay();
    }

    // Present with correct vsync/tearing flags
    UINT syncInterval = g_RendererContext->m_VSync ? 1 : 0;
    UINT presentFlags = (!g_RendererContext->m_VSync && g_RendererContext->m_TearingSupported)
                        ? DXGI_PRESENT_ALLOW_TEARING
                        : 0;

    HRESULT hr = g_RendererContext->SwapChain->Present(syncInterval, 0);
    if (hr == DXGI_STATUS_OCCLUDED) {
        g_RendererContext->m_SwapChainOccluded = true;
        SM_TRACE("Window minimizing / screen locked...");
    } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        SM_ERROR("Device lost, need to recreate device and swap chain and all resources");
    } else {
        SM_ASSERT(SUCCEEDED(hr), "SwapChain Present failed");
    }
}

void renderer_shutdown() {

    if (g_RendererContext->DeviceContext) {
        // Unbind everything we bound during rendering to avoid ref cycles
        ID3D11RenderTargetView* nullRTV[1] = { nullptr };
        g_RendererContext->DeviceContext->OMSetRenderTargets(1, nullRTV, nullptr);

        g_RendererContext->DeviceContext->VSSetShader(nullptr, nullptr, 0);
        g_RendererContext->DeviceContext->PSSetShader(nullptr, nullptr, 0);
        g_RendererContext->DeviceContext->GSSetShader(nullptr, nullptr, 0);
        g_RendererContext->DeviceContext->HSSetShader(nullptr, nullptr, 0);
        g_RendererContext->DeviceContext->DSSetShader(nullptr, nullptr, 0);
        g_RendererContext->DeviceContext->CSSetShader(nullptr, nullptr, 0);

        ID3D11Buffer* nullVB[1] = { nullptr };
        UINT strides[1] = { 0 };
        UINT offsets[1] = { 0 };
        g_RendererContext->DeviceContext->IASetVertexBuffers(0, 1, nullVB, strides, offsets);
        g_RendererContext->DeviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        g_RendererContext->DeviceContext->IASetInputLayout(nullptr);

        g_RendererContext->DeviceContext->OMSetDepthStencilState(nullptr, 0);
        g_RendererContext->DeviceContext->RSSetState(nullptr);
        g_RendererContext->DeviceContext->RSSetViewports(0, nullptr);
        g_RendererContext->DeviceContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

        // Clear all state (releases references held by the context)
        g_RendererContext->DeviceContext->ClearState();
        g_RendererContext->DeviceContext->Flush();
    }

    // If you ever switch to fullscreen, make sure we return to windowed before releasing
    if (g_RendererContext->SwapChain) {
        // Ignore failure if already windowed; it’s just defensive
        g_RendererContext->SwapChain->SetFullscreenState(FALSE, nullptr);
    }

    // Release state objects and views first
    g_RendererContext->DepthState.Reset();
    g_RendererContext->RasterState.Reset();

    g_RendererContext->DepthStencilView.Reset();
    g_RendererContext->RenderTargetView.Reset();

    // Release geometry and constant buffers
    g_RendererContext->VertexBuffer.Reset();
    g_RendererContext->IndexBuffer.Reset();
    g_RendererContext->ConstantBuffer.Reset();

    // Release depth texture
    g_RendererContext->DepthStencilTexture.Reset();

    // Release shaders and input layout
    g_RendererContext->InputLayout.Reset();
    g_RendererContext->PixelShader.Reset();
    g_RendererContext->VertexShader.Reset();

    // Release swap chain, context, and device (order helps on some drivers)
    g_RendererContext->SwapChain.Reset();

    if (g_RendererContext->DeviceContext) {
        // Make sure no outstanding references (defensive)
        g_RendererContext->DeviceContext->Flush();
    }
    g_RendererContext->DeviceContext.Reset();

    // In Debug configs with D3D11 debug layer, report live objects (optional but very useful)
#if defined(_DEBUG)
    if (g_RendererContext->Device) {
        ComPtr<ID3D11Debug> d3dDebug;
        if (SUCCEEDED(g_RendererContext->Device.As(&d3dDebug)) && d3dDebug) {
            d3dDebug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
        }
    }
#endif

    g_RendererContext->Device.Reset();
}

void renderer_resize(const int width, const int height) {
    if (!g_RendererContext || !g_RendererContext->SwapChain) {
        return;
    }

    // Ignore minimized/zero-size resize to avoid invalid calls
    if (width <= 0 || height <= 0) {
        return;
    }

    // Unbind existing render targets to break references held by the device context
    if (g_RendererContext->DeviceContext) {
        ID3D11RenderTargetView* nullRTV[1] = { nullptr };
        g_RendererContext->DeviceContext->OMSetRenderTargets(1, nullRTV, nullptr);
        g_RendererContext->DeviceContext->Flush();
    }

    // Release size-dependent resources before resizing the swap chain
    g_RendererContext->DepthStencilView.Reset();
    g_RendererContext->RenderTargetView.Reset();
    g_RendererContext->DepthStencilTexture.Reset();

    // Resize swap chain buffers
    HRESULT hr = g_RendererContext->SwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    SM_ASSERT(SUCCEEDED(hr), "SwapChain ResizeBuffers failed");

    // Store new size and recreate targets and viewport
    g_RendererContext->Width = width;
    g_RendererContext->Height = height;
    create_render_and_depth_target();

    // After a successful resize assume not occluded
    g_RendererContext->m_SwapChainOccluded = false;
}

void renderer_set_vsync(bool enabled) {
    g_RendererContext->m_VSync = enabled;
    SM_TRACE("VSync = %d", enabled);
}

void* renderer_get_device() {
    return g_RendererContext->Device.Get();
}

void* renderer_get_device_context() {
    return g_RendererContext->DeviceContext.Get();
}