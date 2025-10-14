#include "renderer.h"

#include "render.h"

#include <wrl.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_5.h>

#include "image.h"
#include "VertexPacked.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;

struct PerFrameCBData {
    glm::mat4 Model;
    glm::mat4 VP;
    glm::vec3 CameraPos;
    float pad0;
    glm::vec3 SunColor;
    float Ambient;
};

static_assert(sizeof(PerFrameCBData) % 16 == 0);

struct PerDrawCBData {
    glm::vec3 chunkOffset;
    float pad;
    glm::vec2 tileSizeUV;
    glm::vec2 tileTexelOffset;
    glm::vec4 materialTint;
};

static_assert(sizeof(PerDrawCBData) % 16 == 0);

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
    ComPtr<ID3D11Texture2D>        AtlasTexture;
    ComPtr<ID3D11ShaderResourceView> AtlasSRV;
    ComPtr<ID3D11SamplerState>     SamplerState;
    ComPtr<ID3D11Buffer>           PerFrameCB;
    ComPtr<ID3D11Buffer>           PerDrawCB;

    ComPtr<ID3D11VertexShader> DebugVertexShader;
    ComPtr<ID3D11PixelShader>  DebugPixelShader;
    ComPtr<ID3D11InputLayout>  DebugInputLayout;
    ComPtr<ID3D11Buffer>       DebugVB;
    ComPtr<ID3D11Buffer>       DebugIB;

    int Width;
    int Height;
    int AtlasWidth;
    int AtlasHeight;
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

// HLSL for a textured cube using a texture atlas
const char* G_VS_HLSL = R"(
// Per-frame CB (register b0)
cbuffer PerFrame : register(b0)
{
    float4x4 uModel;   // model transform (optional)
    float4x4 uVP;      // view-proj
    float3   uCameraPos;
    float    pad0;
    float3   uSunColor; // optional
    float    uAmbient;
};

// Per-draw CB (register b1)
cbuffer PerDraw : register(b1)
{
    float3 gChunkOffset;      // world offset for sub-chunk (if used)
    float  pad1;
    float2 gTileSizeUV;       // (tileW / atlasW, tileH / atlasH)
    float2 gTileTexelOffset;  // small offset to avoid bleeding (e.g., 0.5/atlasW)
    float4 gMaterialTint;     // optional per-draw tint
};

// Input vertex (matches D3D11 input layout)
struct VSIn
{
    float3 Pos            : POSITION;   // float3 world/local pos (for now)
    uint   MaterialFlags  : TEXCOORD0;  // at offset 12
    uint   TilePacked     : TEXCOORD1;  // at offset 16
    uint   PackedLightUV  : TEXCOORD2;  // at offset 20: [bits 0..15]=packedLight, [16..31]=uvFace
};

// Vertex -> Pixel struct
struct VSOut
{
    float4 PosH   : SV_POSITION;
    float2 UV     : TEXCOORD0;
    float3 World  : TEXCOORD1;
    float  Light  : TEXCOORD2;
    float  AO     : TEXCOORD3;
    uint   MatID  : TEXCOORD4;
};

// decode baked light stored as: [bits 0..3]=blockLight, [4..7]=skyLight
inline float DecodeLight(uint packed)
{
    uint blockLight = packed & 0xFu;
    uint skyLight   = (packed >> 4) & 0xFu;
    float blk = (float)blockLight / 15.0f;
    float sky = (float)skyLight / 15.0f;
    return saturate(sky + blk * 0.9f);
}

// decode AO stored in bits [8..11] (0..15)
inline float DecodeAO(uint packed)
{
    uint ao = (packed >> 8) & 0xFu;
    return 1.0f - (float)ao / 15.0f;
}

VSOut main_vs(VSIn vin)
{
    VSOut o;

    // world position (optionally apply chunk offset)
    float3 worldPos = vin.Pos + gChunkOffset;

    //float4 wpos = float4(worldPos, 1.0f);
    float4 wpos = mul(float4(vin.Pos + gChunkOffset, 1.0f), uModel);
    o.PosH = mul(uVP, wpos);

    // decode tile index (we stored 16-bit tile value in low 16 bits)
    uint tilePacked = vin.TilePacked & 0xFFFFu;
    uint tileX = tilePacked & 0xFFu;
    uint tileY = (tilePacked >> 8) & 0xFFu;

    // unpack packedLight and uvFace
    uint packedLight = vin.PackedLightUV & 0xFFFFu;        // bits 0..15
    uint uvFace      = (vin.PackedLightUV >> 16) & 0xFFFFu; // bits 16..31

    uint uBit = uvFace & 1u;
    uint vBit = (uvFace >> 1) & 1u;
    // uint faceIndex = (uvFace >> 8) & 0xFFu; // if you encoded faceIndex there

    float2 localUV = float2((float)uBit, (float)vBit);

    // tile origin / uv compute (same as before)
    float2 tileOrigin = float2((float)tileX, (float)tileY) * gTileSizeUV;
    float2 uv = tileOrigin + localUV * gTileSizeUV;
    uv += gTileTexelOffset;

    o.UV = uv;
    o.World = worldPos;
    o.Light = DecodeLight(packedLight);
    o.AO = DecodeAO(packedLight);
    o.MatID = vin.MaterialFlags & 0xFFFFu;

    return o;
}
)";

const char* G_PS_HLSL = R"(
Texture2D gAtlas : register(t0);       // atlas texture (bind to slot t0)
SamplerState gSampler : register(s0);  // sampler (bind to slot s0)

// Match the VSOut structure for input semantics
struct PSIn
{
    float4 PosH   : SV_POSITION;
    float2 UV     : TEXCOORD0;
    float3 World  : TEXCOORD1;
    float  Light  : TEXCOORD2;
    float  AO     : TEXCOORD3;
    uint   MatID  : TEXCOORD4;
};

// Per-frame and Per-draw CBs are expected to be bound the same as VS (b0, b1)
// If you need uAmbient or tint here, they come from the same CBs.

cbuffer PerFrame : register(b0)
{
    float4x4 uModel;
    float4x4 uVP;
    float3   uCameraPos;
    float    pad0;
    float3   uSunColor;
    float    uAmbient;
};

cbuffer PerDraw : register(b1)
{
    float3 gChunkOffset;
    float  pad1;
    float2 gTileSizeUV;
    float2 gTileTexelOffset;
    float4 gMaterialTint;
};

// Pixel shader
float4 main_ps(PSIn pin) : SV_Target
{
    float4 albedo = gAtlas.Sample(gSampler, pin.UV);

    // If this material is cutout, perform alpha-test here (enable per-material)
    // if (albedo.a < 0.5f) discard;

    // Combine baked light and ambient, then apply AO and tint
    float3 lit = albedo.rgb * (uAmbient + pin.Light * (1.0f - uAmbient));
    lit *= pin.AO;
    lit *= gMaterialTint.rgb;

    return float4(lit, albedo.a);
}
)";

const char* G_VS_DEBUG_HLSL = R"(
struct VSIn { float3 Pos : POSITION; };
struct VSOut { float4 SVPos : SV_POSITION; };
VSOut main_vs_debug(VSIn IN) {
    VSOut OUT;
    OUT.SVPos = float4(IN.Pos, 1.0f);
    return OUT;
}
)";

const char* G_PS_DEBUG_HLSL = R"(
struct VSOut { float4 SVPos : SV_POSITION; };
float4 main_ps(VSOut IN) : SV_Target {
    return float4(1.0f, 0.0f, 1.0f, 1.0f); // magenta
}
)";

ComPtr<ID3DBlob> CompileShader(const char* Source, const char* Entry, const char* Target);
void create_render_and_depth_target();
void create_shaders();
void create_texture_atlas();
void create_cube_buffers();
void create_cube_buffers_packed();
void create_debug_triangle();

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
                infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
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
    create_texture_atlas();
    create_cube_buffers_packed();
    // create_debug_triangle();
}

void create_texture_atlas() {
    int imageWidth = 0;
    int imageHeight = 0;
    int imageChannels = 0;
    auto* imageData = (unsigned char*) image_load("assets/block_atlas.png", &imageWidth, &imageHeight, &imageChannels);
    SM_ASSERT(imageData, "Failed to load image");
    SM_ASSERT(imageWidth > 0 && imageHeight > 0, "Invalid image dimensions");

    const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM; // image_load outputs 4 channels
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = imageWidth;
    texDesc.Height = imageHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = fmt;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = imageData;
    initData.SysMemPitch = (UINT)(imageWidth * 4);

    HRESULT hr = g_RendererContext->Device->CreateTexture2D(&texDesc, &initData, &g_RendererContext->AtlasTexture);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create atlas texture");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = fmt;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    hr = g_RendererContext->Device->CreateShaderResourceView(g_RendererContext->AtlasTexture.Get(), &srvDesc, &g_RendererContext->AtlasSRV);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create atlas SRV");

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxLOD = 0;
    hr = g_RendererContext->Device->CreateSamplerState(&sampDesc, &g_RendererContext->SamplerState);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create sampler state");

    // Store atlas dimensions for UV computation
    g_RendererContext->AtlasWidth = imageWidth;
    g_RendererContext->AtlasHeight = imageHeight;

    image_free(imageData);
}

void create_cube_buffers_packed() {
    struct Tile { int x, y; };
    Tile faceTiles[6] = {
        {0, 11}, // Front (+Z)
        {0, 11}, // Right (+X)
        {0, 11}, // Back  (-Z)
        {0, 11}, // Left  (-X)
        {1, 11}, // Top   (+Y)
        {1, 11}, // Bottom(-Y)
    };

    struct Float3 { float x,y,z; };
    Float3 nnn = {-0.5f, -0.5f, -0.5f};
    Float3 pnn = { 0.5f, -0.5f, -0.5f};
    Float3 ppn = { 0.5f,  0.5f, -0.5f};
    Float3 npn = {-0.5f,  0.5f, -0.5f};
    Float3 nnp = {-0.5f, -0.5f,  0.5f};
    Float3 pnp = { 0.5f, -0.5f,  0.5f};
    Float3 ppp = { 0.5f,  0.5f,  0.5f};
    Float3 npp = {-0.5f,  0.5f,  0.5f};

    VertexPacked vertices[24];

    auto set_face = [&](const int baseIndex, const Float3& p0, const Float3& p1, const Float3& p2, const Float3& p3, Tile t, int faceIndex){
        // We'll encode per-vertex corner bits (uBit,vBit) as follows:
        // mapping to your earlier corners: (u,v) = (0,1),(1,1),(1,0),(0,0)
        const int cornerU[4] = {0,1,1,0};
        const int cornerV[4] = {1,1,0,0};

        const uint16_t tilePacked = PackTile(t.x, t.y);
        const uint16_t packedLight = PackLight(15, 15, 0);

        const auto materialFlags = static_cast<uint32_t>(t.x | (t.y << 8)); // example: lower 16 bits could be tile index
        // You can also store explicit tile index in materialFlags if you prefer

        const Float3 pos[4] = { p0, p1, p2, p3 };
        for (int i = 0; i < 4; ++i)
        {
            const uint16_t uvFace = PackUVFace(cornerU[i], cornerV[i], faceIndex);

            VertexPacked v = {};
            v.pos.x = pos[i].x;
            v.pos.y = pos[i].y;
            v.pos.z = pos[i].z;
            v.materialFlags = materialFlags;
            v.tilePacked = static_cast<uint32_t>(tilePacked);
            v.packedLightUV = (static_cast<uint32_t>(packedLight) | (static_cast<uint32_t>(uvFace) << 16));
            vertices[baseIndex + i] = v;
        }
    };

    set_face( 0, nnp, pnp, ppp, npp, faceTiles[0], 0); // Front (+Z)
    set_face( 4, pnp, pnn, ppn, ppp, faceTiles[1], 1); // Right (+X)
    set_face( 8, pnn, nnn, npn, ppn, faceTiles[2], 2); // Back  (-Z)
    set_face(12, nnn, nnp, npp, npn, faceTiles[3], 3); // Left  (-X)
    set_face(16, npp, ppp, ppn, npn, faceTiles[4], 4); // Top   (+Y)
    set_face(20, nnn, pnn, pnp, nnp, faceTiles[5], 5); // Bottom(-Y)

    static const uint16_t indices[] = {
        0,1,2, 0,2,3,
        4,5,6, 4,6,7,
        8,9,10, 8,10,11,
        12,13,14, 12,14,15,
        16,17,18, 16,18,19,
        20,21,22, 20,22,23,
    };

    // Create vertex buffer
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = (UINT)sizeof(vertices);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = 0;
    vbDesc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = vertices;
    HRESULT hr = g_RendererContext->Device->CreateBuffer(&vbDesc, &vbData, &g_RendererContext->VertexBuffer);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create vertex buffer");

    // Create index buffer (unchanged)
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = (UINT)sizeof(indices);
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ibDesc.CPUAccessFlags = 0;
    ibDesc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = indices;
    hr = g_RendererContext->Device->CreateBuffer(&ibDesc, &ibData, &g_RendererContext->IndexBuffer);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create index buffer");

    // Create PerFrame constant buffer
    {
        D3D11_BUFFER_DESC perFrameDesc = {};
        perFrameDesc.ByteWidth = sizeof(PerFrameCBData); // should be multiple of 16
        perFrameDesc.Usage = D3D11_USAGE_DYNAMIC;
        perFrameDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        perFrameDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        perFrameDesc.MiscFlags = 0;
        perFrameDesc.StructureByteStride = 0;

        HRESULT hr = g_RendererContext->Device->CreateBuffer(&perFrameDesc, nullptr, &g_RendererContext->PerFrameCB);
        SM_ASSERT(SUCCEEDED(hr), "Failed to create PerFrame CB");
    }

    // Create PerDraw constant buffer
    {
        D3D11_BUFFER_DESC perDrawDesc = {};
        perDrawDesc.ByteWidth = sizeof(PerDrawCBData); // should be multiple of 16
        perDrawDesc.Usage = D3D11_USAGE_DYNAMIC;
        perDrawDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        perDrawDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        perDrawDesc.MiscFlags = 0;
        perDrawDesc.StructureByteStride = 0;

        HRESULT hr = g_RendererContext->Device->CreateBuffer(&perDrawDesc, nullptr, &g_RendererContext->PerDrawCB);
        SM_ASSERT(SUCCEEDED(hr), "Failed to create PerDraw CB");
    }
}

void create_debug_triangle() {
    // VS
    auto vsBlob = CompileShader(G_VS_DEBUG_HLSL, "main_vs_debug", "vs_5_0");
    HRESULT hr = g_RendererContext->Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_RendererContext->DebugVertexShader);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create Debug VS");

    // Input layout for position-only
    D3D11_INPUT_ELEMENT_DESC inDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    hr = g_RendererContext->Device->CreateInputLayout(inDesc, _countof(inDesc), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_RendererContext->DebugInputLayout);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create debug input layout");

    // PS
    auto psBlob = CompileShader(G_PS_DEBUG_HLSL, "main_ps", "ps_5_0");
    hr = g_RendererContext->Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_RendererContext->DebugPixelShader);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create Debug PS");

    // Simple triangle in NDC (x,y,z) coordinates. This bypasses any matrices so it must appear on screen.
    struct TriV { float x,y,z; };
    TriV triVerts[3] = {
        { -0.5f, -0.5f, 0.5f },
        {  0.5f, -0.5f, 0.5f },
        {  0.0f,  0.5f, 0.5f },
    };
    uint16_t triIdx[3] = { 0,1,2 };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.ByteWidth = sizeof(triVerts);
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vinit = { triVerts, 0, 0 };
    hr = g_RendererContext->Device->CreateBuffer(&vbDesc, &vinit, &g_RendererContext->DebugVB);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create debug VB");

    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.ByteWidth = sizeof(triIdx);
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iinit = { triIdx, 0, 0 };
    hr = g_RendererContext->Device->CreateBuffer(&ibDesc, &iinit, &g_RendererContext->DebugIB);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create debug IB");
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
            SM_ERROR("Shader compile error: %s", (char*)errors->GetBufferPointer());
        }
        SM_ASSERT(false, "Shader compile failed");
    }
    return bytecode;
}

void create_shaders() {
    // Vertex shader
    ComPtr<ID3DBlob> vsBlob = CompileShader(G_VS_HLSL, "main_vs", "vs_5_0");
    HRESULT hr = g_RendererContext->Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_RendererContext->VertexShader);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create vertex shader");
    // Input layout (must match VertexPacked structure)
    static const D3D11_INPUT_ELEMENT_DESC InputLayout[] =
    {
        // SemanticName, SemanticIndex, Format, InputSlot, AlignedByteOffset, InputSlotClass, InstanceDataStepRate
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 }, // float3 pos (0)
        { "TEXCOORD", 0, DXGI_FORMAT_R32_UINT,        0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }, // materialFlags (uint32) (12)
        { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT,        0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 }, // tilePacked (uint32)   (16)
        { "TEXCOORD", 2, DXGI_FORMAT_R32_UINT,        0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 }, // packedLightUV (uint32) (20)
    };
    hr = g_RendererContext->Device->CreateInputLayout(
        InputLayout, _countof(InputLayout),
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        &g_RendererContext->InputLayout);
    SM_ASSERT(SUCCEEDED(hr), "Failed to create input layout");

    // Pixel shader
    ComPtr<ID3DBlob> psBlob = CompileShader(G_PS_HLSL, "main_ps", "ps_5_0");
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


    if (false) {
        // Save/restore state is optional but simpler to just set what we need:
        g_RendererContext->DeviceContext->IASetInputLayout(g_RendererContext->DebugInputLayout.Get());
        UINT strideDebug = sizeof(float) * 3;
        UINT offsetDebug = 0;
        ID3D11Buffer* vbs[] = { g_RendererContext->DebugVB.Get() };
        g_RendererContext->DeviceContext->IASetVertexBuffers(0, 1, vbs, &strideDebug, &offsetDebug);
        g_RendererContext->DeviceContext->IASetIndexBuffer(g_RendererContext->DebugIB.Get(), DXGI_FORMAT_R16_UINT, 0);
        g_RendererContext->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Bind debug shaders
        g_RendererContext->DeviceContext->VSSetShader(g_RendererContext->DebugVertexShader.Get(), nullptr, 0);
        g_RendererContext->DeviceContext->PSSetShader(g_RendererContext->DebugPixelShader.Get(), nullptr, 0);

        // Draw
        g_RendererContext->DeviceContext->DrawIndexed(3, 0, 0);

        // Restore your main shaders / input layout afterwards (optional)
        g_RendererContext->DeviceContext->VSSetShader(g_RendererContext->VertexShader.Get(), nullptr, 0);
        g_RendererContext->DeviceContext->PSSetShader(g_RendererContext->PixelShader.Get(), nullptr, 0);
        g_RendererContext->DeviceContext->IASetInputLayout(g_RendererContext->InputLayout.Get());
    }

    // IA: bind cube vertex/index buffers
    UINT stride = 24; // sizeof(VertexPacked) (float3 pos=12 + uint32 matflags=4 + 3 * uint16 = 6 + 2 padding =24)
    UINT offset = 0;
    ID3D11Buffer* vb = g_RendererContext->VertexBuffer.Get();
    g_RendererContext->DeviceContext->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_RendererContext->DeviceContext->IASetIndexBuffer(g_RendererContext->IndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    g_RendererContext->DeviceContext->IASetInputLayout(g_RendererContext->InputLayout.Get());
    g_RendererContext->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set shaders
    g_RendererContext->DeviceContext->VSSetShader(g_RendererContext->VertexShader.Get(), nullptr, 0);
    g_RendererContext->DeviceContext->PSSetShader(g_RendererContext->PixelShader.Get(), nullptr, 0);

    // Bind atlas texture and sampler
    if (g_RendererContext->AtlasSRV && g_RendererContext->SamplerState) {
        ID3D11ShaderResourceView* srvs[] = { g_RendererContext->AtlasSRV.Get() };
        g_RendererContext->DeviceContext->PSSetShaderResources(0, 1, srvs);
        ID3D11SamplerState* samps[] = { g_RendererContext->SamplerState.Get() };
        g_RendererContext->DeviceContext->PSSetSamplers(0, 1, samps);
    }

    // Update constant buffer with Model and View-Projection matrices
    if (g_RendererContext->PerFrameCB) {
        // Build PerFrame data (must match PerFrameCBData layout)
        PerFrameCBData perFrame = {};
        perFrame.Model = renderData->modelMatrix3D;   // glm::mat4
        {
            glm::mat4 V = renderData->gameCamera.get_view_matrix();
            glm::mat4 P = renderData->gameCamera.get_projection_matrix();
            perFrame.VP = P * V;
        }
        perFrame.CameraPos = renderData->gameCamera.position; // supply camera pos if available
        perFrame.SunColor = glm::vec3(1.0f); // tweak as needed
        perFrame.Ambient = 0.08f; // tune as needed

        // Upload via UpdateSubresource (ok for small CBs); you can use Map/Unmap with WRITE_DISCARD as alternative
        // g_RendererContext->DeviceContext->UpdateSubresource(g_RendererContext->PerFrameCB.Get(), 0, nullptr, &perFrame, 0, 0);
        // Map PerFrameCB (D3D11_USAGE_DYNAMIC) and write with WRITE_DISCARD
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = g_RendererContext->DeviceContext->Map(
            g_RendererContext->PerFrameCB.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped
        );
        if (SUCCEEDED(hr)) {
            // memcpy into mapped.pData (size is sizeof(PerFrameCBData))
            memcpy(mapped.pData, &perFrame, sizeof(PerFrameCBData));
            g_RendererContext->DeviceContext->Unmap(g_RendererContext->PerFrameCB.Get(), 0);
        } else {
            SM_ERROR("Failed to Map PerFrameCB (hr=0x%08X)", hr);
        }

        ID3D11Buffer* frameCbs[] = { g_RendererContext->PerFrameCB.Get() };
        // Bind to slot b0 for both VS and PS
        g_RendererContext->DeviceContext->VSSetConstantBuffers(0, 1, frameCbs);
        g_RendererContext->DeviceContext->PSSetConstantBuffers(0, 1, frameCbs);
    }

    // PerDrawCB already updated above — bind it to slot b1 for both VS and PS
    if (g_RendererContext->PerDrawCB) {
        constexpr float tilePixelW = 16.0f;
        constexpr float tilePixelH = 16.0f;

        PerDrawCBData perDrawData = {};
        perDrawData.chunkOffset = glm::vec3(0.0f); // if you have per-subchunk world offset set it here
        perDrawData.tileSizeUV = glm::vec2(tilePixelW / (float)g_RendererContext->AtlasWidth,
                                           tilePixelH / (float)g_RendererContext->AtlasHeight);
        // texel inset to avoid bleeding when not using padding (0.5/atlasSize)
        perDrawData.tileTexelOffset = glm::vec2(0.5f / (float)g_RendererContext->AtlasWidth,
                                                0.5f / (float)g_RendererContext->AtlasHeight);
        perDrawData.materialTint = glm::vec4(1.0f);

        //g_RendererContext->DeviceContext->UpdateSubresource(g_RendererContext->PerDrawCB.Get(), 0, nullptr, &perDrawData, 0, 0);

        D3D11_MAPPED_SUBRESOURCE mappedDraw = {};
        HRESULT hr2 = g_RendererContext->DeviceContext->Map(
            g_RendererContext->PerDrawCB.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mappedDraw
        );
        if (SUCCEEDED(hr2)) {
            memcpy(mappedDraw.pData, &perDrawData, sizeof(PerDrawCBData));
            g_RendererContext->DeviceContext->Unmap(g_RendererContext->PerDrawCB.Get(), 0);
        } else {
            SM_ERROR("Failed to Map PerDrawCB (hr=0x%08X)", hr2);
        }

        ID3D11Buffer* drawCbs[] = { g_RendererContext->PerDrawCB.Get() };
        g_RendererContext->DeviceContext->VSSetConstantBuffers(1, 1, drawCbs);
        g_RendererContext->DeviceContext->PSSetConstantBuffers(1, 1, drawCbs);
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
    g_RendererContext->PerFrameCB.Reset();
    g_RendererContext->PerDrawCB.Reset();

    // Release texture resources
    g_RendererContext->SamplerState.Reset();
    g_RendererContext->AtlasSRV.Reset();
    g_RendererContext->AtlasTexture.Reset();

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