#include "renderer.h"

#include <chrono>
#include <d3dcompiler.h>
#include <thread>
#include <vector>

#include <lib.h>
#include <render.h>
#include "renderer_dx12.h"
#include "renderer_dx11.h"
#include "renderer_vulkan.h"

#include <nvrhi/utils.h>

#include "font.h"
#include "image.h"
#include "VertexPacked.h"
#include "glm/gtx/euler_angles.hpp"
#include "tracy/Tracy.hpp"

#define RENDER_API directx12
#define RENDER_API_NAME RendererBackendDX12

class DebugMessageCallback;

struct GpuTimer
{
    std::vector<nvrhi::TimerQueryHandle> queries; // size = FramesInFlight (or >)
    uint32_t index = 0;                           // rotating index

    void init(nvrhi::IDevice* device, uint32_t capacity)
    {
        queries.resize(capacity);
        for (auto& q : queries) q = device->createTimerQuery();
    }

    void begin(nvrhi::ICommandList* cmd)
    {
        cmd->beginTimerQuery(queries[index].Get());
    }

    void end(nvrhi::ICommandList* cmd)
    {
        cmd->endTimerQuery(queries[index].Get());
    }

    // Call once per frame after submission; try to read a result from a previous frame
    // Returns true if a result was available and written to outSeconds
    bool tryRead(nvrhi::IDevice* device, float& outSeconds)
    {
        // Probe an older query (e.g., previous frame index)
        uint32_t readIdx = (index + 1) % (uint32_t)queries.size();
        auto* q = queries[readIdx].Get();

        if (device->pollTimerQuery(q))
        {
            outSeconds = device->getTimerQueryTime(q); // non‑blocking now
            device->resetTimerQuery(q);
            return true;
        }
        return false;
    }

    void advance()
    {
        // Reset the just‑used query so it’s ready next time we come back around
        index = (index + 1) % (uint32_t)queries.size();
    }
};

struct DummyRenderPass {
    nvrhi::BufferHandle m_VertexBuffer;
    nvrhi::BufferHandle m_IndexBuffer;
    nvrhi::BufferHandle m_PerFrameConstantBuffer;
    nvrhi::BufferHandle m_PerDrawConstantBuffer;
    nvrhi::TextureHandle m_Texture;
    nvrhi::SamplerHandle m_PointClampSampler;
    nvrhi::ShaderHandle m_VertexShader;
    nvrhi::ShaderHandle m_PixelShader;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSet;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    uint32_t m_IndexCount = 0;
    uint32_t m_AtlasWidth = 0;
    uint32_t m_AtlasHeight = 0;
};

struct PrimitivePass {
    nvrhi::BufferHandle m_VertexBuffer;      // plane VB
    nvrhi::BufferHandle m_IndexBuffer;       // plane IB
    nvrhi::BufferHandle m_PerDrawCB;         // grid params
    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSet;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    uint32_t m_IndexCount = 0;
};

struct UIPass {
    nvrhi::BufferHandle m_VertexBuffer;            // static quad vertices (pos, uv)
    nvrhi::BufferHandle m_IndexBuffer;             // static quad indices
    nvrhi::BufferHandle m_PerFrameCB;              // UIFrame (uOrtho)
    nvrhi::BufferHandle m_InstanceBuffer;          // Structured buffer of UIInstanceCPU
    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSet;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    uint32_t m_IndexCount = 0;
    uint32_t m_MaxInstances = 16384;
};

typedef struct RendererContext {
    nvrhi::DeviceHandle      m_Device;
    nvrhi::CommandQueue      m_CommandQueue;
    nvrhi::CommandListHandle m_CommandList;
    RendererBackend*         m_Backend;
    double                   m_PreviousFrameTimestamp;
    DummyRenderPass          m_RenderPass{};
    PrimitivePass            m_PrimPass{};
    FontAtlas                m_FontAtlas{}; // default font (index 0)
    std::vector<FontAtlas>   m_Fonts;       // additional fonts supported
    UIPass                   m_UIPass{};
    uint32_t                 m_RefUIWidth = 0;   // reference UI resolution (width)
    uint32_t                 m_RefUIHeight = 0;  // reference UI resolution (height)
    int                      m_Width;
    int                      m_Height;
    GpuTimer                 m_GpuTimer;
} RendererContext;

static RendererContext* g_RendererContext = nullptr;

static void create_ui_binding_for_font(FontAtlas& fa)
{
    if (!g_RendererContext) return;
    if (!fa.texture || !fa.sampler) return;
    if (!g_RendererContext->m_UIPass.m_BindingLayout) return;
    nvrhi::BindingSetDesc bsd;
    bsd.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, g_RendererContext->m_UIPass.m_PerFrameCB),        // b0
        nvrhi::BindingSetItem::Texture_SRV(0, fa.texture),                                         // t0
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, g_RendererContext->m_UIPass.m_InstanceBuffer), // t1 SRV
        nvrhi::BindingSetItem::Sampler(0, fa.sampler)                                              // s0
    };
    fa.uiBindingSet = g_RendererContext->m_Device->createBindingSet(bsd, g_RendererContext->m_UIPass.m_BindingLayout);
}

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

struct PrimPerDrawCB {
    glm::vec4 GridColor;
    glm::vec4 AxisXColor;
    glm::vec4 AxisZColor;
    glm::vec2 GridParams;
    glm::vec2 FadeParams;
};
static_assert(sizeof(PrimPerDrawCB) % 16 == 0);

struct UIVertex
{
    float x, y;   // position in pixels
    float u, v;   // texture coordinates
};

struct UIFrameCBData
{
    glm::mat4 Ortho;
};
static_assert(sizeof(UIFrameCBData) % 16 == 0);

struct UIInstanceCPU
{
    glm::mat4 Transform;
    glm::vec4 Color;
    glm::vec4 UVRect; // (scaleX, scaleY, offsetX, offsetY)
    uint32_t Flags;   // bitmask: e.g., 1<<0 = SAMPLE_TEXTURE
    uint32_t _pad[3]; // padding to maintain 16-byte alignment
};
static_assert(sizeof(UIInstanceCPU) % 16 == 0);

static constexpr uint32_t UI_OPT_SAMPLE_TEXTURE = 1u << 0;

enum class TextureAlphaMode
{
    UNKNOWN = 0,
    STRAIGHT = 1,
    PREMULTIPLIED = 2,
    OPAQUE_ = 3,
    CUSTOM = 4,
};

struct LoadedTexture
{
    nvrhi::TextureHandle texture;
    TextureAlphaMode alphaMode = TextureAlphaMode::UNKNOWN;
    uint32_t originalBitsPerPixel = 0;
    std::string path;
    std::string mimeType;
};

struct TextureSubresourceData
{
    size_t rowPitch = 0;
    size_t depthPitch = 0;
    ptrdiff_t dataOffset = 0;
    size_t dataSize = 0;
};

struct TextureData : public LoadedTexture
{
    const char* data;

    nvrhi::Format format = nvrhi::Format::UNKNOWN;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t arraySize = 1;
    uint32_t mipLevels = 1;
    nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Unknown;
    bool isRenderTarget = false;
    bool forceSRGB = false;

    // ArraySlice -> MipLevel -> TextureSubresourceData
    std::vector<std::vector<TextureSubresourceData>> dataLayout;
};

nvrhi::ShaderHandle CreateShader(const char* content, const char* entryName, const char* targetName, const nvrhi::ShaderDesc& desc)
{
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
        entryName, targetName,
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

    if(!bytecode)
        return nullptr;

    nvrhi::ShaderDesc descCopy = desc;
    descCopy.entryName = entryName;

    return g_RendererContext->m_Device->createShader(descCopy, bytecode->GetBufferPointer(), bytecode->GetBufferSize());
}

void fill_vertices(VertexPacked* verts) {
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
            verts[baseIndex + i] = v;
        }
    };

    set_face( 0, nnp, pnp, ppp, npp, faceTiles[0], 0); // Front (+Z)
    set_face( 4, pnp, pnn, ppn, ppp, faceTiles[1], 1); // Right (+X)
    set_face( 8, pnn, nnn, npn, ppn, faceTiles[2], 2); // Back  (-Z)
    set_face(12, nnn, nnp, npp, npn, faceTiles[3], 3); // Left  (-X)
    set_face(16, npp, ppp, ppn, npn, faceTiles[4], 4); // Top   (+Y)
    set_face(20, nnn, pnn, pnp, nnp, faceTiles[5], 5); // Bottom(-Y)
}

void create_cube() {
    SM_ASSERT(g_RendererContext, "Renderer not init");

    // Main shaders (textured cube path)
    g_RendererContext->m_RenderPass.m_VertexShader = CreateShader(G_VS_HLSL, "main_vs", "vs_5_0",
                                                                  nvrhi::ShaderDesc().setShaderType(
                                                                      nvrhi::ShaderType::Vertex));
    g_RendererContext->m_RenderPass.m_PixelShader = CreateShader(G_PS_HLSL, "main_ps", "ps_5_0",
                                                                 nvrhi::ShaderDesc().setShaderType(
                                                                     nvrhi::ShaderType::Pixel));
    // Debug simple blue triangle shaders
    SM_ASSERT(g_RendererContext->m_RenderPass.m_VertexShader && g_RendererContext->m_RenderPass.m_PixelShader,
              "Failed to create NVRHI shaders");

    g_RendererContext->m_RenderPass.m_PerDrawConstantBuffer = g_RendererContext->m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(PerDrawCBData), "PerDrawCBData")
        .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));
    g_RendererContext->m_RenderPass.m_PerFrameConstantBuffer = g_RendererContext->m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(PerFrameCBData), "PerFrameCBData")
        .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));
    SM_ASSERT(g_RendererContext->m_RenderPass.m_PerFrameConstantBuffer, "Failed to create NVRHI constant buffer.");
    SM_ASSERT(g_RendererContext->m_RenderPass.m_PerDrawConstantBuffer, "Failed to create NVRHI constant buffer.");

    nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc()
        .setName("POSITION")
        .setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(VertexPacked, pos))
        .setBufferIndex(0)
        .setElementStride(sizeof(VertexPacked)),

        nvrhi::VertexAttributeDesc()
        .setName("TEXCOORD")
        .setFormat(nvrhi::Format::R32_UINT)
        .setOffset(offsetof(VertexPacked, materialFlags))
        .setBufferIndex(0)
        .setArraySize(3) // use TEXCOORD0,1,2 for our 3 uint attributes
        .setElementStride(sizeof(VertexPacked)),
    };
    g_RendererContext->m_RenderPass.m_InputLayout = g_RendererContext->m_Device->createInputLayout(
        attributes, std::size(attributes), g_RendererContext->m_RenderPass.m_VertexShader);
    SM_ASSERT(g_RendererContext->m_RenderPass.m_InputLayout, "Failed to create input layout");

	const auto commandList = g_RendererContext->m_Device->createCommandList();
    commandList->open();

    VertexPacked vertices[24];
    fill_vertices(vertices);

    nvrhi::BufferDesc vertexBufferDesc;
    vertexBufferDesc.byteSize = sizeof(vertices);
    vertexBufferDesc.isVertexBuffer = true;
    vertexBufferDesc.debugName = "VertexBuffer";
    vertexBufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
    g_RendererContext->m_RenderPass.m_VertexBuffer = g_RendererContext->m_Device->createBuffer(vertexBufferDesc);
    SM_ASSERT(g_RendererContext->m_RenderPass.m_VertexBuffer, "Failed to create vertex buffer");

    commandList->beginTrackingBufferState(g_RendererContext->m_RenderPass.m_VertexBuffer, nvrhi::ResourceStates::CopyDest);
    commandList->writeBuffer(g_RendererContext->m_RenderPass.m_VertexBuffer, vertices, sizeof(vertices));
    commandList->setPermanentBufferState(g_RendererContext->m_RenderPass.m_VertexBuffer, nvrhi::ResourceStates::VertexBuffer);

    static const uint16_t indices[] = {
        0,1,2, 0,2,3,
        4,5,6, 4,6,7,
        8,9,10, 8,10,11,
        12,13,14, 12,14,15,
        16,17,18, 16,18,19,
        20,21,22, 20,22,23,
    };
    g_RendererContext->m_RenderPass.m_IndexCount = std::size(indices);

    nvrhi::BufferDesc indexBufferDesc;
    indexBufferDesc.byteSize = sizeof(indices);
    indexBufferDesc.isIndexBuffer = true;
    indexBufferDesc.debugName = "IndexBuffer";
    indexBufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
    g_RendererContext->m_RenderPass.m_IndexBuffer = g_RendererContext->m_Device->createBuffer(indexBufferDesc);
    SM_ASSERT(g_RendererContext->m_RenderPass.m_IndexBuffer, "Failed to create index buffer");

    commandList->beginTrackingBufferState(g_RendererContext->m_RenderPass.m_IndexBuffer, nvrhi::ResourceStates::CopyDest);
    commandList->writeBuffer(g_RendererContext->m_RenderPass.m_IndexBuffer, indices, sizeof(indices));
    commandList->setPermanentBufferState(g_RendererContext->m_RenderPass.m_IndexBuffer, nvrhi::ResourceStates::IndexBuffer);

    int imageWidth = 0;
    int imageHeight = 0;
    int imageChannels = 0;
    auto* imageData = static_cast<char *>(image_load("assets/block_atlas.png", &imageWidth, &imageHeight, &imageChannels));
    g_RendererContext->m_RenderPass.m_AtlasWidth = imageWidth;
    g_RendererContext->m_RenderPass.m_AtlasHeight = imageHeight;
    SM_ASSERT(imageData, "Failed to load image");
    SM_ASSERT(imageWidth > 0 && imageHeight > 0, "Invalid image dimensions");

    TextureData texture {};
    texture.originalBitsPerPixel = 8;
    texture.width = imageWidth;
    texture.height = imageHeight;
    texture.isRenderTarget = true;
    texture.mipLevels = 1;
    texture.dimension = nvrhi::TextureDimension::Texture2D;
    texture.dataLayout.resize(1);
    texture.dataLayout[0].resize(1);
    texture.dataLayout[0][0].dataOffset = 0;
    texture.dataLayout[0][0].rowPitch = imageWidth * imageChannels;
    texture.dataLayout[0][0].dataSize = imageWidth * imageHeight * imageChannels;
    texture.data = imageData;

    switch (imageChannels) {
        case 1:
            texture.format = nvrhi::Format::R8_UNORM;
            break;
        case 2:
            texture.format = nvrhi::Format::RG8_UNORM;
            break;
        case 4:
            texture.format = nvrhi::Format::RGBA8_UNORM; // SRGBA8_UNORM
    }

    unsigned int scaledWidth = imageWidth;
    unsigned int scaledHeight = imageHeight;

    nvrhi::TextureDesc textureDesc;
    textureDesc.format = texture.format;
    textureDesc.width = scaledWidth;
    textureDesc.height = scaledHeight;
    textureDesc.depth = texture.depth;
    textureDesc.arraySize = texture.arraySize;
    textureDesc.dimension = texture.dimension;
    textureDesc.mipLevels = texture.mipLevels;
    textureDesc.debugName = texture.path;
    textureDesc.isRenderTarget = texture.isRenderTarget;

    texture.texture = g_RendererContext->m_Device->createTexture(textureDesc);
    SM_ASSERT(texture.texture, "Failed to create texture");

    const char* dataPointer = texture.data;

    commandList->beginTrackingTextureState(texture.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);

    for (uint32_t arraySlice = 0; arraySlice < texture.arraySize; arraySlice++)
    {
        for (uint32_t mipLevel = 0; mipLevel < texture.mipLevels; mipLevel++)
        {
            const TextureSubresourceData& layout = texture.dataLayout[arraySlice][mipLevel];

            commandList->writeTexture(texture.texture, arraySlice, mipLevel, dataPointer + layout.dataOffset,
                                                           layout.rowPitch, layout.depthPitch);
        }
    }

    image_free(imageData);
    texture.data = nullptr;

    commandList->setPermanentTextureState(texture.texture, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    g_RendererContext->m_RenderPass.m_Texture = texture.texture;

    auto samplerDesc = nvrhi::SamplerDesc()
            .setAllFilters(false)
            .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    g_RendererContext->m_RenderPass.m_PointClampSampler = g_RendererContext->m_Device->createSampler(samplerDesc);

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, g_RendererContext->m_RenderPass.m_PerFrameConstantBuffer), // b0
        nvrhi::BindingSetItem::ConstantBuffer(1, g_RendererContext->m_RenderPass.m_PerDrawConstantBuffer),  // b1
        nvrhi::BindingSetItem::Texture_SRV(0, texture.texture),                                             // t0
        nvrhi::BindingSetItem::Sampler(0, g_RendererContext->m_RenderPass.m_PointClampSampler)              // s0
    };

    // Create the binding layout and the binding set, store in the render pass
    if (!nvrhi::utils::CreateBindingSetAndLayout(
            g_RendererContext->m_Device,
            nvrhi::ShaderType::All,
            0,
            bindingSetDesc,
            g_RendererContext->m_RenderPass.m_BindingLayout,
            g_RendererContext->m_RenderPass.m_BindingSet))
    {
        SM_ASSERT(false, "Failed to create bindings set or layout");
    }

    commandList->close();
    g_RendererContext->m_Device->executeCommandList(commandList);
}

void load_font_atlas() {
    // Load default font and register as font index 0
    load_font("assets/AtariClassic-gry3.ttf", 12, g_RendererContext->m_FontAtlas, g_RendererContext->m_Device);
    // Keep list coherent: ensure index 0 exists
    g_RendererContext->m_Fonts.clear();
    g_RendererContext->m_Fonts.push_back(g_RendererContext->m_FontAtlas);
    renderer_font_load("assets/LiberationSans-Regular.ttf", 12);
}

static void create_ui_pass()
{
    SM_ASSERT(g_RendererContext, "Renderer not init");
    g_RendererContext->m_UIPass.m_MaxInstances = 16384;

    // Compile shaders
    g_RendererContext->m_UIPass.m_VS = CreateShader(QUAD_VS_HLSL, "main_vs", "vs_5_0",
        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex));
    g_RendererContext->m_UIPass.m_PS = CreateShader(QUAD_PS_HLSL, "main_ps", "ps_5_0",
        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel));

    SM_ASSERT(g_RendererContext->m_UIPass.m_VS && g_RendererContext->m_UIPass.m_PS, "Failed to create UI shaders");

    // Create static quad VB/IB
    const UIVertex quadVerts[4] = {
        // x, y, u, v
        { 0.0f,  0.0f, 0.0f, 0.0f },
        { 1.0f,  0.0f, 1.0f, 0.0f },
        { 1.0f,  1.0f, 1.0f, 1.0f },
        { 0.0f,  1.0f, 0.0f, 1.0f },
    };
    const uint16_t quadIdx[6] = { 0, 1, 2, 0, 2, 3 };

    auto cl = g_RendererContext->m_Device->createCommandList();
    cl->open();

    // VB
    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = sizeof(quadVerts);
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = "UIQuadVB";
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    g_RendererContext->m_UIPass.m_VertexBuffer = g_RendererContext->m_Device->createBuffer(vbDesc);

    cl->beginTrackingBufferState(g_RendererContext->m_UIPass.m_VertexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(g_RendererContext->m_UIPass.m_VertexBuffer, quadVerts, sizeof(quadVerts));
    cl->setPermanentBufferState(g_RendererContext->m_UIPass.m_VertexBuffer, nvrhi::ResourceStates::VertexBuffer);

    // IB
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = sizeof(quadIdx);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "UIQuadIB";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    g_RendererContext->m_UIPass.m_IndexBuffer = g_RendererContext->m_Device->createBuffer(ibDesc);

    cl->beginTrackingBufferState(g_RendererContext->m_UIPass.m_IndexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(g_RendererContext->m_UIPass.m_IndexBuffer, quadIdx, sizeof(quadIdx));
    cl->setPermanentBufferState(g_RendererContext->m_UIPass.m_IndexBuffer, nvrhi::ResourceStates::IndexBuffer);

    g_RendererContext->m_UIPass.m_IndexCount = 6;

    // Per-frame CB (uOrtho)
    g_RendererContext->m_UIPass.m_PerFrameCB = g_RendererContext->m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(UIFrameCBData), "UIFrameCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));

    // Instance buffer (StructuredBuffer SRV)
    {
        nvrhi::BufferDesc instDesc;
        instDesc.debugName = "UIInstanceBuffer";
        instDesc.byteSize = g_RendererContext->m_UIPass.m_MaxInstances * sizeof(UIInstanceCPU);
        instDesc.structStride = sizeof(UIInstanceCPU);
        instDesc.initialState = nvrhi::ResourceStates::CopyDest; // upload then use as SRV
        instDesc.isVertexBuffer = false;
        instDesc.isIndexBuffer = false;
        instDesc.isConstantBuffer = false;
        instDesc.canHaveUAVs = false;
        instDesc.setInitialState(nvrhi::ResourceStates::CopyDest);
        instDesc.setKeepInitialState(true);
        g_RendererContext->m_UIPass.m_InstanceBuffer = g_RendererContext->m_Device->createBuffer(instDesc);
    }

    // Input layout: POSITION (RG32_FLOAT), TEXCOORD (RG32_FLOAT)
    nvrhi::VertexAttributeDesc uiAttrs[] = {
        nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(offsetof(UIVertex, x)).setBufferIndex(0).setElementStride(sizeof(UIVertex)),
        nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(offsetof(UIVertex, u)).setBufferIndex(0).setElementStride(sizeof(UIVertex)),
    };
    g_RendererContext->m_UIPass.m_InputLayout = g_RendererContext->m_Device->createInputLayout(uiAttrs, std::size(uiAttrs), g_RendererContext->m_UIPass.m_VS);

    // Create binding layout for UI pass: b0 (PerFrame), t0 (Font texture), t1 (Instance buffer SRV), s0 (Sampler)
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),  // b0
        nvrhi::BindingLayoutItem::Texture_SRV(0),     // t0
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1), // t1
        nvrhi::BindingLayoutItem::Sampler(0)          // s0
    };
    g_RendererContext->m_UIPass.m_BindingLayout = g_RendererContext->m_Device->createBindingLayout(layoutDesc);

    cl->close();
    g_RendererContext->m_Device->executeCommandList(cl);

    // Create per-font binding sets using the UI layout
    auto createFontUIBinding = [&](FontAtlas& fa)
    {
        if (!fa.texture || !fa.sampler) return;
        nvrhi::BindingSetDesc bsd;
        bsd.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, g_RendererContext->m_UIPass.m_PerFrameCB),        // b0
            nvrhi::BindingSetItem::Texture_SRV(0, fa.texture),                                         // t0
            nvrhi::BindingSetItem::StructuredBuffer_SRV(1, g_RendererContext->m_UIPass.m_InstanceBuffer), // t1 SRV
            nvrhi::BindingSetItem::Sampler(0, fa.sampler)                                              // s0
        };
        fa.uiBindingSet = g_RendererContext->m_Device->createBindingSet(bsd, g_RendererContext->m_UIPass.m_BindingLayout);
    };

    // Default font
    createFontUIBinding(g_RendererContext->m_FontAtlas);
    // Additional fonts
    for (auto& fa : g_RendererContext->m_Fonts)
        createFontUIBinding(fa);

    // Default UI binding set is font 0
    g_RendererContext->m_UIPass.m_BindingSet = g_RendererContext->m_FontAtlas.uiBindingSet;
}

static void create_primitives_pass()
{
    SM_ASSERT(g_RendererContext, "Renderer not init");

    g_RendererContext->m_PrimPass.m_PerDrawCB = g_RendererContext->m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(PrimPerDrawCB), "PrimPerDrawCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    // Compile primitive shaders (procedural grid)
    g_RendererContext->m_PrimPass.m_VS = CreateShader(PRIM_VS_HLSL, "main_vs", "vs_5_0",
        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex));
    g_RendererContext->m_PrimPass.m_PS = CreateShader(PRIM_PS_HLSL, "main_ps", "ps_5_0",
        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel));

    SM_ASSERT(g_RendererContext->m_PrimPass.m_VS && g_RendererContext->m_PrimPass.m_PS, "Failed to create primitive shaders");

    // Create big plane geometry on Y=0 (two triangles)
    struct PrimVertex { float x,y,z; };
    const float S = 10000.0f;
    PrimVertex verts[4] = { {-S,0,-S}, {+S,0,-S}, {+S,0,+S}, {-S,0,+S} };
    uint16_t inds[6] = { 0,1,2, 0,2,3 };

    auto cl = g_RendererContext->m_Device->createCommandList();
    cl->open();

    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = sizeof(verts);
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = "PrimPlaneVB";
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    g_RendererContext->m_PrimPass.m_VertexBuffer = g_RendererContext->m_Device->createBuffer(vbDesc);

    cl->beginTrackingBufferState(g_RendererContext->m_PrimPass.m_VertexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(g_RendererContext->m_PrimPass.m_VertexBuffer, verts, sizeof(verts));
    cl->setPermanentBufferState(g_RendererContext->m_PrimPass.m_VertexBuffer, nvrhi::ResourceStates::VertexBuffer);

    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = sizeof(inds);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "PrimPlaneIB";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    g_RendererContext->m_PrimPass.m_IndexBuffer = g_RendererContext->m_Device->createBuffer(ibDesc);

    cl->beginTrackingBufferState(g_RendererContext->m_PrimPass.m_IndexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(g_RendererContext->m_PrimPass.m_IndexBuffer, inds, sizeof(inds));
    cl->setPermanentBufferState(g_RendererContext->m_PrimPass.m_IndexBuffer, nvrhi::ResourceStates::IndexBuffer);

    cl->close();

    g_RendererContext->m_PrimPass.m_IndexCount = 6;

    g_RendererContext->m_Device->executeCommandList(cl);

    // Create input layout (POSITION only)
    nvrhi::VertexAttributeDesc attr;
    attr.setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0).setBufferIndex(0).setElementStride(sizeof(PrimVertex));
    g_RendererContext->m_PrimPass.m_InputLayout = g_RendererContext->m_Device->createInputLayout(&attr, 1, g_RendererContext->m_PrimPass.m_VS);

    // Create binding layout/set (b0 = PerFrame from main pass, b2 = PrimPerDraw)
    nvrhi::BindingSetDesc bs;
    bs.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, g_RendererContext->m_RenderPass.m_PerFrameConstantBuffer),
        nvrhi::BindingSetItem::ConstantBuffer(2, g_RendererContext->m_PrimPass.m_PerDrawCB)
    };

    if (!nvrhi::utils::CreateBindingSetAndLayout(
            g_RendererContext->m_Device,
            nvrhi::ShaderType::All,
            0,
            bs,
            g_RendererContext->m_PrimPass.m_BindingLayout,
            g_RendererContext->m_PrimPass.m_BindingSet))
    {
        SM_ASSERT(false, "Failed to create Primitive binding set/layout");
    }
}

void renderer_init(const int width, const int height, void* handle, BumpAllocator* persistentStorage) {

    void* memBlock = bump_alloc(persistentStorage, sizeof(RendererContext));
    if (!memBlock) {
        SM_ERROR("Failed to allocate RendererContext");
        return;
    }
    g_RendererContext = new (memBlock) RendererContext();
    if (!g_RendererContext) {
        SM_ERROR("Failed to allocate RendererContext");
        return;
    }

    // Store the initial window size as the reference UI resolution
    g_RendererContext->m_RefUIWidth = static_cast<uint32_t>(std::max(1, width));
    g_RendererContext->m_RefUIHeight = static_cast<uint32_t>(std::max(1, height));
    g_RendererContext->m_Width = width;
    g_RendererContext->m_Height = height;
    g_RendererContext->m_PreviousFrameTimestamp =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    void* backendMem = bump_alloc(persistentStorage, sizeof(RENDER_API_NAME));
    if (!backendMem) {
        SM_ERROR("Failed to allocate RendererBackend");
        return;
    }
    auto* backend = new (backendMem) RENDER_API_NAME();
    g_RendererContext->m_Backend = reinterpret_cast<RendererBackend *>(backend);
    if (!g_RendererContext->m_Backend) {
        SM_ERROR("Failed to allocate renderer backend");
        return;
    }

    RENDER_API::create_internal_instance(g_RendererContext->m_Backend);

    g_RendererContext->m_Device = RENDER_API::create_device(g_RendererContext->m_Backend, handle);
    if (!g_RendererContext->m_Device) {
        SM_ERROR("Failed to create graphics device");
        return;
    }

    RENDER_API::create_swapchain(g_RendererContext->m_Backend, static_cast<HWND>(handle), width, height);

    g_RendererContext->m_CommandList = RENDER_API::create_command_list(g_RendererContext->m_Backend);

    g_RendererContext->m_GpuTimer.init(g_RendererContext->m_Device, 256);

    create_cube();
    load_font_atlas();
    create_ui_pass();
    create_primitives_pass();
}

void renderer_shutdown() {
    if (g_RendererContext && g_RendererContext->m_Backend) {
        RENDER_API::renderer_backend_shutdown(g_RendererContext->m_Backend);
        std::destroy_at(reinterpret_cast<RENDER_API_NAME*>(g_RendererContext->m_Backend));
        g_RendererContext->m_Backend = nullptr;
    }
    if (g_RendererContext) {
        std::destroy_at(g_RendererContext);
        g_RendererContext = nullptr;
    }
    SM_TRACE("Renderer context destroyed");
}

float render(float dt, RenderData* renderData, BumpAllocator* transientStorage, pfnRenderUIOverlay uiOverlay) {

    if (!g_RendererContext || !g_RendererContext->m_Backend) {
        SM_ASSERT(false, "RendererContext or RendererBackend is null");
        return false;
    }

    float secs = 0;
    {
        ZoneScopedN("GpuTimer.TryRead");
        // Read GPU timer from last frame
        if (g_RendererContext->m_GpuTimer.tryRead(g_RendererContext->m_Device, secs)) {
            secs = secs * 1000.0f;
        }
    }


    std::chrono::high_resolution_clock::time_point currentTime = std::chrono::high_resolution_clock::now();
    double curTime = std::chrono::duration<double>(currentTime.time_since_epoch()).count();
    double elapsedTime = curTime - g_RendererContext->m_PreviousFrameTimestamp;

    uint32_t* backendFrameIndex = RENDER_API::renderer_get_frame_index(g_RendererContext->m_Backend);;

    if (*backendFrameIndex > 0) {
        if (RENDER_API::renderer_begin_frame(g_RendererContext->m_Backend)) {

            const auto frameBuffer = RENDER_API::renderer_get_framebuffer(g_RendererContext->m_Backend);

            {
                ZoneScopedN("BeginRecord");
                g_RendererContext->m_CommandList->open();
                g_RendererContext->m_GpuTimer.begin(g_RendererContext->m_CommandList);

                nvrhi::utils::ClearColorAttachment(g_RendererContext->m_CommandList, frameBuffer, 0,
                                                   nvrhi::Color(renderData->clearColor.r, renderData->clearColor.g,
                                                                renderData->clearColor.b, renderData->clearColor.a));
            }

            {
                ZoneScopedN("Pipeline Creation");
                // --- Primitives (Grid Plane) Pipeline ---
                if (!g_RendererContext->m_PrimPass.m_Pipeline)
                {
                    const auto fbi = frameBuffer->getFramebufferInfo();
                    nvrhi::GraphicsPipelineDesc pso;
                    pso.VS = g_RendererContext->m_PrimPass.m_VS;
                    pso.PS = g_RendererContext->m_PrimPass.m_PS;
                    pso.inputLayout = g_RendererContext->m_PrimPass.m_InputLayout;
                    pso.bindingLayouts = { g_RendererContext->m_PrimPass.m_BindingLayout };
                    pso.primType = nvrhi::PrimitiveType::TriangleList;
                    pso.renderState.depthStencilState.depthTestEnable = false; // no depth buffer yet
                    pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
                    g_RendererContext->m_PrimPass.m_Pipeline = g_RendererContext->m_Device->createGraphicsPipeline(pso, fbi);
                }

                // --- Cube Pipeline ---
                if (!g_RendererContext->m_RenderPass.m_Pipeline) {
                    const auto fbi = frameBuffer->getFramebufferInfo();

                    nvrhi::GraphicsPipelineDesc psoDesc;
                    psoDesc.VS = g_RendererContext->m_RenderPass.m_VertexShader;
                    psoDesc.PS = g_RendererContext->m_RenderPass.m_PixelShader;
                    psoDesc.inputLayout = g_RendererContext->m_RenderPass.m_InputLayout;
                    psoDesc.bindingLayouts = { g_RendererContext->m_RenderPass.m_BindingLayout };
                    psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
                    psoDesc.renderState.depthStencilState.depthTestEnable = false;
                    psoDesc.renderState.rasterState.setFrontCounterClockwise(true);
                    g_RendererContext->m_RenderPass.m_Pipeline = g_RendererContext->m_Device->createGraphicsPipeline(psoDesc, fbi);
                }

                // --- UI Pipeline ---
                if (!g_RendererContext->m_UIPass.m_Pipeline)
                {
                    const auto fbi2 = frameBuffer->getFramebufferInfo();
                    nvrhi::GraphicsPipelineDesc uiDesc;
                    uiDesc.VS = g_RendererContext->m_UIPass.m_VS;
                    uiDesc.PS = g_RendererContext->m_UIPass.m_PS;
                    uiDesc.inputLayout = g_RendererContext->m_UIPass.m_InputLayout;
                    uiDesc.bindingLayouts = { g_RendererContext->m_UIPass.m_BindingLayout };
                    uiDesc.primType = nvrhi::PrimitiveType::TriangleList;
                    // UI states
                    uiDesc.renderState.depthStencilState.depthTestEnable = false;
                    uiDesc.renderState.depthStencilState.stencilEnable = false;
                    // Enable straight alpha blending for UI/text using setter API
                    nvrhi::BlendState::RenderTarget rt;
                    rt.setBlendEnable(true)
                      .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                      .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                      .setBlendOp(nvrhi::BlendOp::Add)
                      .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                      .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                      .setBlendOpAlpha(nvrhi::BlendOp::Add)
                      .setColorWriteMask(nvrhi::ColorMask::All);
                    uiDesc.renderState.blendState.setRenderTarget(0, rt);

                    g_RendererContext->m_UIPass.m_Pipeline = g_RendererContext->m_Device->createGraphicsPipeline(uiDesc, fbi2);
                }
            }

            {
                ZoneScopedN("Render Passes");

                // Primitive Pass
                {
                    ZoneScopedN("Primitive Pass");
                    if (renderData && renderData->primitives.count > 0)
                    {
                        // Common VP for this frame
                        glm::mat4 V = renderData->gameCamera.get_view_matrix();
                        glm::mat4 P = renderData->gameCamera.get_projection_matrix();

                        // Defaults for the grid
                        PrimPerDrawCB prim{};
                        prim.GridColor  = {0.35f, 0.35f, 0.35f, 1.0f};
                        prim.AxisXColor = {1.0f, 0.2f, 0.2f, 1.0f};
                        prim.AxisZColor = {0.2f, 0.6f, 1.0f, 1.0f};
                        prim.GridParams = {0.5f, 2.0f};   // 1 unit per cell, 1px thickness
                        prim.FadeParams = {500.0f, 1000.0f};
                        g_RendererContext->m_CommandList->writeBuffer(g_RendererContext->m_PrimPass.m_PerDrawCB, &prim, sizeof(prim));

                        // State
                        nvrhi::GraphicsState primState;
                        primState.pipeline = g_RendererContext->m_PrimPass.m_Pipeline.Get();
                        primState.framebuffer = frameBuffer;
                        primState.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
                        primState.bindings = { g_RendererContext->m_PrimPass.m_BindingSet };
                        primState.vertexBuffers = { nvrhi::VertexBufferBinding(g_RendererContext->m_PrimPass.m_VertexBuffer, 0, 0) };
                        primState.indexBuffer = nvrhi::IndexBufferBinding(g_RendererContext->m_PrimPass.m_IndexBuffer, nvrhi::Format::R16_UINT, 0);

                        for (int i = 0; i < renderData->primitives.count; ++i)
                        {
                            const auto& inst = renderData->primitives[i];
                            if (inst.type != PrimitiveType::Plane) continue;

                            PerFrameCBData pf{};
                            pf.Model = inst.transform;
                            pf.VP = P * V;
                            pf.CameraPos = renderData->gameCamera.position;
                            pf.SunColor = glm::vec3(1.0f);
                            pf.Ambient = 0.08f;
                            g_RendererContext->m_CommandList->writeBuffer(g_RendererContext->m_RenderPass.m_PerFrameConstantBuffer, &pf, sizeof(pf));

                            g_RendererContext->m_CommandList->setGraphicsState(primState);

                            nvrhi::DrawArguments args{};
                            args.vertexCount = g_RendererContext->m_PrimPass.m_IndexCount; // index count for indexed draw
                            args.instanceCount = 1;
                            args.startIndexLocation = 0;
                            args.startVertexLocation = 0;
                            g_RendererContext->m_CommandList->drawIndexed(args);
                        }

                        renderData->primitives.clear();
                    }
                }

                // Cube Packed Pass
                {
                    ZoneScopedN("Cube Pass");
                    // Update constant buffers
                    if (g_RendererContext->m_RenderPass.m_PerFrameConstantBuffer)
                    {
                        PerFrameCBData perFrame = {};
                        glm::mat4 translationMat = glm::translate(glm::mat4(1.0f), renderData->modelPosition);
                        glm::mat4 rotationMat = glm::yawPitchRoll(
                            glm::radians(renderData->modelRotation.y),
                            glm::radians(renderData->modelRotation.x),
                            glm::radians(renderData->modelRotation.z));
                        glm::mat4 scaleMat = glm::scale(glm::mat4(1.0f), renderData->modelScale);
                        renderData->modelMatrix3D = translationMat * rotationMat * scaleMat;
                        perFrame.Model = renderData->modelMatrix3D;
                        {
                            glm::mat4 V = renderData->gameCamera.get_view_matrix();
                            glm::mat4 P = renderData->gameCamera.get_projection_matrix();
                            perFrame.VP = P * V;
                        }
                        perFrame.CameraPos = renderData->gameCamera.position;
                        perFrame.SunColor = glm::vec3(1.0f);
                        perFrame.Ambient = 0.8f;

                        g_RendererContext->m_CommandList->writeBuffer(g_RendererContext->m_RenderPass.m_PerFrameConstantBuffer, &perFrame, sizeof(perFrame));
                    }

                    if (g_RendererContext->m_RenderPass.m_PerDrawConstantBuffer)
                    {
                        PerDrawCBData perDraw = {};
                        constexpr float tilePixelW = 16.0f;
                        constexpr float tilePixelH = 16.0f;
                        perDraw.chunkOffset = glm::vec3(0.0f);
                        perDraw.tileSizeUV = glm::vec2(tilePixelW / static_cast<float>(std::max(1u, g_RendererContext->m_RenderPass.m_AtlasWidth)),
                                                       tilePixelH / static_cast<float>(std::max(1u, g_RendererContext->m_RenderPass.m_AtlasHeight)));
                        perDraw.tileTexelOffset = glm::vec2(0.5f / static_cast<float>(std::max(1u, g_RendererContext->m_RenderPass.m_AtlasWidth)),
                                                            0.5f / static_cast<float>(std::max(1u, g_RendererContext->m_RenderPass.m_AtlasHeight)));
                        perDraw.materialTint = glm::vec4(1.0f);

                        g_RendererContext->m_CommandList->writeBuffer(g_RendererContext->m_RenderPass.m_PerDrawConstantBuffer, &perDraw, sizeof(perDraw));
                    }

                    // Set graphics state and bindings
                    nvrhi::GraphicsState state;
                    state.pipeline = g_RendererContext->m_RenderPass.m_Pipeline.Get();
                    state.framebuffer = frameBuffer;
                    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
                    state.bindings = { g_RendererContext->m_RenderPass.m_BindingSet };
                    state.vertexBuffers = { nvrhi::VertexBufferBinding(g_RendererContext->m_RenderPass.m_VertexBuffer, 0, 0) };
                    state.indexBuffer = nvrhi::IndexBufferBinding(g_RendererContext->m_RenderPass.m_IndexBuffer, nvrhi::Format::R16_UINT, 0);

                    g_RendererContext->m_CommandList->setGraphicsState(state);

                    // Draw indexed cube
                    nvrhi::DrawArguments drawArgs;
                    drawArgs.vertexCount = g_RendererContext->m_RenderPass.m_IndexCount; // for indexed: this is the index count
                    drawArgs.instanceCount = 1;
                    drawArgs.startIndexLocation = 0;
                    drawArgs.startVertexLocation = 0;
                    g_RendererContext->m_CommandList->drawIndexed(drawArgs);
                }

                // UI Pass
                {
                    ZoneScopedN("UI Pass");
                    // Update UI frame CB with orthographic matrix (pixels -> clip space)
                    if (g_RendererContext->m_UIPass.m_PerFrameCB)
                    {
                        const auto vp = frameBuffer->getFramebufferInfo().getViewport();
                        const float w = vp.width();
                        const float h = vp.height();
                        // Map (0,0) top-left to (-1,+1), (w,h) to (+1,-1)
                        // Matrix that does: x' = 2/w*x - 1, y' = 1 - 2/h*y
                        glm::mat4 ortho(1.0f);
                        ortho[0][0] = 2.0f / w;  ortho[1][0] = 0.0f;       ortho[2][0] = 0.0f; ortho[3][0] = -1.0f; // column 0..3
                        ortho[0][1] = 0.0f;       ortho[1][1] = -2.0f / h; ortho[2][1] = 0.0f; ortho[3][1] =  1.0f;
                        ortho[0][2] = 0.0f;       ortho[1][2] = 0.0f;       ortho[2][2] = 1.0f; ortho[3][2] =  0.0f;
                        ortho[0][3] = 0.0f;       ortho[1][3] = 0.0f;       ortho[2][3] = 0.0f; ortho[3][3] =  1.0f;

                        UIFrameCBData uiFrame{};
                        uiFrame.Ortho = ortho;
                        g_RendererContext->m_CommandList->writeBuffer(g_RendererContext->m_UIPass.m_PerFrameCB, &uiFrame, sizeof(uiFrame));
                    }

                    nvrhi::GraphicsState state;
                    state.pipeline = g_RendererContext->m_UIPass.m_Pipeline.Get();
                    state.framebuffer = frameBuffer;
                    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
                    state.vertexBuffers = { nvrhi::VertexBufferBinding(g_RendererContext->m_UIPass.m_VertexBuffer, 0, 0) };
                    state.indexBuffer = nvrhi::IndexBufferBinding(g_RendererContext->m_UIPass.m_IndexBuffer, nvrhi::Format::R16_UINT, 0);

                    // 1) Rects / lines (solid color, no sampling)
                    const uint32_t rectCount = static_cast<uint32_t>(renderData->uiRects.count);
                    if (rectCount > 0)
                    {
                        const uint32_t capped = std::min(rectCount, g_RendererContext->m_UIPass.m_MaxInstances);
                        UIInstanceCPU* rectInstances = reinterpret_cast<UIInstanceCPU*>(bump_alloc(transientStorage, capped * sizeof(UIInstanceCPU)));
                        SM_ASSERT(rectInstances, "Out of transient memory for UI rect instances");
                        uint32_t out = 0;
                        for (uint32_t i = 0; i < capped; ++i)
                        {
                            const UIRectCmd& rc = renderData->uiRects.elements[i];
                            const glm::vec2 pos = rc.pos;
                            const glm::vec2 size = rc.size;
                            UIInstanceCPU inst{};
                            inst.Transform = glm::mat4(1.0f);
                            inst.Transform[0][0] = size.x;
                            inst.Transform[1][1] = size.y;
                            inst.Transform[3][0] = pos.x;
                            inst.Transform[3][1] = pos.y;
                            inst.Color = rc.color;
                            inst.UVRect = glm::vec4(1.f, 1.f, 0.f, 0.f);
                            inst.Flags = 0u;
                            rectInstances[out++] = inst;
                        }
                        if (out > 0)
                        {
                            g_RendererContext->m_CommandList->writeBuffer(g_RendererContext->m_UIPass.m_InstanceBuffer, rectInstances, out * sizeof(UIInstanceCPU));
                            state.bindings = { g_RendererContext->m_FontAtlas.uiBindingSet ? g_RendererContext->m_FontAtlas.uiBindingSet : g_RendererContext->m_UIPass.m_BindingSet };
                            g_RendererContext->m_CommandList->setGraphicsState(state);
                            nvrhi::DrawArguments uiDrawArgs;
                            uiDrawArgs.vertexCount = g_RendererContext->m_UIPass.m_IndexCount;
                            uiDrawArgs.instanceCount = out;
                            uiDrawArgs.startIndexLocation = 0;
                            uiDrawArgs.startVertexLocation = 0;
                            g_RendererContext->m_CommandList->drawIndexed(uiDrawArgs);
                        }
                    }

                    // 2) Texts (per font)
                    if (renderData->uiTexts.count > 0)
                    {
                        // Gather used font indices
                        uint16_t used[64] = {};
                        uint32_t usedCount = 0;
                        for (int ti = 0; ti < renderData->uiTexts.count && usedCount < 64; ++ti)
                        {
                            const uint16_t fi = renderData->uiTexts.elements[ti].fontIndex;
                            bool seen = false;
                            for (uint32_t j = 0; j < usedCount; ++j) if (used[j] == fi) { seen = true; break; }
                            if (!seen) used[usedCount++] = fi;
                        }

                        for (uint32_t ui = 0; ui < usedCount; ++ui)
                        {
                            const uint16_t fontIdx = used[ui];
                            const FontAtlas* fa = nullptr;
                            if (fontIdx < g_RendererContext->m_Fonts.size())
                                fa = &g_RendererContext->m_Fonts[fontIdx];
                            else if (fontIdx == 0)
                                fa = &g_RendererContext->m_FontAtlas;
                            if (!fa || !fa->texture) continue;

                            // Count glyphs for this font
                            uint32_t glyphCount = 0;
                            for (int ti = 0; ti < renderData->uiTexts.count; ++ti)
                                if (renderData->uiTexts.elements[ti].fontIndex == fontIdx)
                                    glyphCount += renderData->uiTexts.elements[ti].glyphCount;
                            if (glyphCount == 0) continue;
                            glyphCount = std::min(glyphCount, g_RendererContext->m_UIPass.m_MaxInstances);

                            auto* glyphInstances = reinterpret_cast<UIInstanceCPU*>(bump_alloc(transientStorage, glyphCount * sizeof(UIInstanceCPU)));
                            SM_ASSERT(glyphInstances, "Out of transient memory for UI glyph instances");

                            const auto aw = static_cast<float>(fa->width);
                            const auto ah = static_cast<float>(fa->height);
                            uint32_t out = 0;
                            for (int ti = 0; ti < renderData->uiTexts.count && out < glyphCount; ++ti)
                            {
                                const UITextCmd& tc = renderData->uiTexts.elements[ti];
                                if (tc.fontIndex != fontIdx) continue;
                                const uint32_t off = tc.textOffset;
                                const uint32_t len = tc.textLength;
                                if (off + len > UI_TEXT_BUFFER_BYTES) continue;
                                const char* str = renderData->uiTextBuffer + off;
                                glm::vec2 pen = tc.pos;
                                for (uint32_t k = 0; k < len && out < glyphCount; ++k)
                                {
                                    const auto c = static_cast<unsigned char>(str[k]);
                                    if (c >= 128u) continue;
                                    const Glyph& g = fa->glyphs[c];
                                    glm::vec2 pos;
                                    pos.x = pen.x + g.offset.x;
                                    pos.y = pen.y - g.offset.y;
                                    const glm::vec2 size = g.size;
                                    UIInstanceCPU inst{};
                                    inst.Transform = glm::mat4(1.0f);
                                    inst.Transform[0][0] = size.x;
                                    inst.Transform[1][1] = size.y;
                                    inst.Transform[3][0] = pos.x;
                                    inst.Transform[3][1] = pos.y;
                                    inst.Color = tc.color;
                                    inst.Flags = UI_OPT_SAMPLE_TEXTURE;
                                    const auto uvScale = glm::vec2(g.size.x / aw, g.size.y / ah);
                                    const auto uvOffset = glm::vec2(g.textureCoords.x / aw, g.textureCoords.y / ah);
                                    inst.UVRect = glm::vec4(uvScale.x, uvScale.y, uvOffset.x, uvOffset.y);
                                    glyphInstances[out++] = inst;
                                    pen.x += g.advance.x;
                                }
                            }
                            if (out > 0)
                            {
                                g_RendererContext->m_CommandList->writeBuffer(g_RendererContext->m_UIPass.m_InstanceBuffer, glyphInstances, out * sizeof(UIInstanceCPU));
                                state.bindings = { fa->uiBindingSet ? fa->uiBindingSet : g_RendererContext->m_UIPass.m_BindingSet };
                                g_RendererContext->m_CommandList->setGraphicsState(state);
                                nvrhi::DrawArguments uiDrawArgs;
                                uiDrawArgs.vertexCount = g_RendererContext->m_UIPass.m_IndexCount;
                                uiDrawArgs.instanceCount = out;
                                uiDrawArgs.startIndexLocation = 0;
                                uiDrawArgs.startVertexLocation = 0;
                                g_RendererContext->m_CommandList->drawIndexed(uiDrawArgs);
                            }
                        }
                    }
                }
            }


            {
                ZoneScopedN("Submit");
                g_RendererContext->m_GpuTimer.end(g_RendererContext->m_CommandList);
                g_RendererContext->m_CommandList->close();
                g_RendererContext->m_Device->executeCommandList(g_RendererContext->m_CommandList, nvrhi::CommandQueue::Graphics);

                g_RendererContext->m_GpuTimer.advance();
            }

            if (uiOverlay) {
                ZoneScopedN("Overlay");
                uiOverlay(renderData, frameBuffer);
            }

            renderData->uiRects.clear();
            renderData->uiTexts.clear();
            renderData->uiTextBufferCount = 0;

            {
                ZoneScopedN("Present");
                const bool presentSuccess = RENDER_API::renderer_present(g_RendererContext->m_Backend);
                if (!presentSuccess) {
                    SM_ERROR("renderer_present failed");
                    return 0;
                }
            }
        }
    }

    {
        ZoneScopedN("GC");
        g_RendererContext->m_Device->runGarbageCollection();
    }

    RENDER_API::renderer_update_avg_frame_time(g_RendererContext->m_Backend, elapsedTime);
    g_RendererContext->m_PreviousFrameTimestamp = curTime;

    ++*backendFrameIndex;

    return secs;
}

void renderer_resize(int width, int height) {
    RENDER_API::renderer_resize_swapchain(g_RendererContext->m_Backend, width, height);
    g_RendererContext->m_RenderPass.m_Pipeline = nullptr;
    g_RendererContext->m_UIPass.m_Pipeline = nullptr;
    g_RendererContext->m_Width = width;
    g_RendererContext->m_Height = height;
}

void renderer_toggle_vsync() {
    if (!g_RendererContext || !g_RendererContext->m_Backend) {
        SM_WARN("renderer_set_vsync called before renderer_init");
        return;
    }

    // The backend type is selected via RENDER_API_NAME (DX11/DX12). Update its settings flag.
    auto* backend = reinterpret_cast<RENDER_API_NAME*>(g_RendererContext->m_Backend);
    const bool prev = backend->m_Settings.vsyncEnabled;
    backend->m_Settings.vsyncEnabled = !prev;

    SM_TRACE("VSync %s", backend->m_Settings.vsyncEnabled ? "ENABLED" : "DISABLED");
}

void* renderer_get_device_context() {
    return g_RendererContext->m_Device;
}

int renderer_font_load(const char* filePath, int fontSize)
{
    if (!g_RendererContext || !g_RendererContext->m_Device) return -1;
    FontAtlas atlas{};
    load_font(filePath, fontSize, atlas, g_RendererContext->m_Device);
    if (!atlas.texture)
        return -1;

    // Register in list
    int index = 0;
    if (g_RendererContext->m_Fonts.empty())
    {
        // ensure default at 0
        g_RendererContext->m_Fonts.push_back(g_RendererContext->m_FontAtlas);
        index = static_cast<int>(g_RendererContext->m_Fonts.size());
        g_RendererContext->m_Fonts.push_back(atlas);
        create_ui_binding_for_font(g_RendererContext->m_Fonts[1]);
        return 1;
    }
    else
    {
        index = static_cast<int>(g_RendererContext->m_Fonts.size());
        g_RendererContext->m_Fonts.push_back(atlas);
        create_ui_binding_for_font(g_RendererContext->m_Fonts.back());
        return index;
    }
}

glm::vec2 renderer_measure_text(uint16_t fontIndex, const char* text)
{
    if (!g_RendererContext || !text) return {0,0};
    const FontAtlas* fa = nullptr;
    if (fontIndex < g_RendererContext->m_Fonts.size()) fa = &g_RendererContext->m_Fonts[fontIndex];
    else if (fontIndex == 0) fa = &g_RendererContext->m_FontAtlas;
    if (!fa) return {0,0};

    float width = 0.0f;
    float maxAscent = 0.0f;
    float maxDescent = 0.0f;
    const size_t len = strlen(text);
    for (size_t i = 0; i < len; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 128u) continue;
        const Glyph& g = fa->glyphs[c];
        width += g.advance.x;
        // Estimate ascent/descent from offset and size
        maxAscent = std::max(maxAscent, g.offset.y);
        maxDescent = std::max(maxDescent, g.size.y - g.offset.y);
    }
    float height = std::max({ fa->pixelHeight * 1.0f, maxAscent + maxDescent, 0.0f });
    return { width, height };
}