#include "renderer.h"

#include <chrono>
#include <d3dcompiler.h>
#include <thread>

#include "lib.h"
#include "render.h"
#include "renderer_dx12.h"
#include "renderer_dx11.h"

#include <nvrhi/utils.h>

#include "font.h"
#include "image.h"
#include "VertexPacked.h"

#define RENDER_API directx12
#define RENDER_API_NAME RendererBackendDX12

class DebugMessageCallback;

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

typedef struct RendererContext {
    nvrhi::DeviceHandle      m_Device;
    nvrhi::CommandQueue      m_CommandQueue;
    nvrhi::CommandListHandle m_CommandList;
    RendererBackend*         m_Backend;
    double                   m_PreviousFrameTimestamp;
    DummyRenderPass          m_RenderPass{};
    FontAtlas                m_FontAtlas{};
} RendererContext;

static RendererContext* g_RendererContext = nullptr;

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
    load_font("assets/AtariClassic-gry3.ttf", 12, g_RendererContext->m_FontAtlas, g_RendererContext->m_Device);
}

void renderer_init(const int width, const int height, void* handle, BumpAllocator* persistentStorage) {

    g_RendererContext = reinterpret_cast<RendererContext *>(bump_alloc(persistentStorage, sizeof(RendererContext)));
    if (!g_RendererContext) {
        SM_ERROR("Failed to allocate RendererContext");
        return;
    }

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

    g_RendererContext->m_Device = RENDER_API::create_device(g_RendererContext->m_Backend);
    if (!g_RendererContext->m_Device) {
        SM_ERROR("Failed to create graphics device");
        return;
    }

    RENDER_API::create_swapchain(g_RendererContext->m_Backend, static_cast<HWND>(handle), width, height);

    g_RendererContext->m_CommandList = g_RendererContext->m_Device->createCommandList();

    create_cube();
    load_font_atlas();
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

bool render(float dt, RenderData* renderData, pfnRenderUIOverlay uiOverlay) {

    if (!g_RendererContext || !g_RendererContext->m_Backend) {
        SM_ASSERT(false, "RendererContext or RendererBackend is null");
        return false;
    }

    std::chrono::high_resolution_clock::time_point currentTime = std::chrono::high_resolution_clock::now();
    double curTime = std::chrono::duration<double>(currentTime.time_since_epoch()).count();
    double elapsedTime = curTime - g_RendererContext->m_PreviousFrameTimestamp;

    uint32_t* backendFrameIndex = RENDER_API::renderer_get_frame_index(g_RendererContext->m_Backend);

    if (*backendFrameIndex > 0) {
        if (RENDER_API::renderer_begin_frame(g_RendererContext->m_Backend)) {

            const auto frameBuffer = RENDER_API::renderer_get_framebuffer(g_RendererContext->m_Backend);

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

            g_RendererContext->m_CommandList->open();

            nvrhi::utils::ClearColorAttachment(g_RendererContext->m_CommandList, frameBuffer, 0, nvrhi::Color(0.8f, 0.2f, 0.3f, 1.0f));

            // Update constant buffers
            if (g_RendererContext->m_RenderPass.m_PerFrameConstantBuffer && renderData)
            {
                PerFrameCBData perFrame = {};
                perFrame.Model = renderData->modelMatrix3D;
                {
                    glm::mat4 V = renderData->gameCamera.get_view_matrix();
                    glm::mat4 P = renderData->gameCamera.get_projection_matrix();
                    perFrame.VP = P * V;
                }
                perFrame.CameraPos = renderData->gameCamera.position;
                perFrame.SunColor = glm::vec3(1.0f);
                perFrame.Ambient = 0.08f;

                //g_RendererContext->m_CommandList->beginTrackingBufferState(g_RendererContext->m_RenderPass.m_PerFrameConstantBuffer, nvrhi::ResourceStates::ConstantBuffer);
                g_RendererContext->m_CommandList->writeBuffer(g_RendererContext->m_RenderPass.m_PerFrameConstantBuffer, &perFrame, sizeof(perFrame));
            }

            if (g_RendererContext->m_RenderPass.m_PerDrawConstantBuffer)
            {
                PerDrawCBData perDraw = {};
                constexpr float tilePixelW = 16.0f;
                constexpr float tilePixelH = 16.0f;
                perDraw.chunkOffset = glm::vec3(0.0f);
                perDraw.tileSizeUV = glm::vec2(tilePixelW / (float)std::max(1u, g_RendererContext->m_RenderPass.m_AtlasWidth),
                                               tilePixelH / (float)std::max(1u, g_RendererContext->m_RenderPass.m_AtlasHeight));
                perDraw.tileTexelOffset = glm::vec2(0.5f / (float)std::max(1u, g_RendererContext->m_RenderPass.m_AtlasWidth),
                                                    0.5f / (float)std::max(1u, g_RendererContext->m_RenderPass.m_AtlasHeight));
                perDraw.materialTint = glm::vec4(1.0f);

                //g_RendererContext->m_CommandList->beginTrackingBufferState(g_RendererContext->m_RenderPass.m_PerDrawConstantBuffer, nvrhi::ResourceStates::ConstantBuffer);
                g_RendererContext->m_CommandList->writeBuffer(g_RendererContext->m_RenderPass.m_PerDrawConstantBuffer, &perDraw, sizeof(perDraw));
            }

            // g_RendererContext->m_CommandList->commitBarriers();

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

            g_RendererContext->m_CommandList->close();
            g_RendererContext->m_Device->executeCommandList(g_RendererContext->m_CommandList, nvrhi::CommandQueue::Graphics);

            if (uiOverlay) {
                uiOverlay();
            }

            const bool presentSuccess = RENDER_API::renderer_present(g_RendererContext->m_Backend);
            if (!presentSuccess) {
                SM_ERROR("renderer_present failed");
                return false;
            }
        }
    }

    g_RendererContext->m_Device->runGarbageCollection();

    RENDER_API::renderer_update_avg_frame_time(g_RendererContext->m_Backend, elapsedTime);
    g_RendererContext->m_PreviousFrameTimestamp = curTime;

    ++*backendFrameIndex;
    return true;
}

void renderer_resize(int width, int height) {
    RENDER_API::renderer_resize_swapchain(g_RendererContext->m_Backend, width, height);
}

void renderer_set_vsync(bool enabled) {

}

void* renderer_get_device() {
    return nullptr;
}

void* renderer_get_device_context() {
    return g_RendererContext->m_Backend;
}