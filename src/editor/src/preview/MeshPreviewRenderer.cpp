#include "MeshPreviewRenderer.h"

#include "MeshSystem.h"
#include "Renderer.h"
#include "lib.h"

#include <nvrhi/utils.h>
#include <glm/gtc/matrix_transform.hpp>

// Simple HLSL shaders for mesh preview rendering
static const char* PREVIEW_VS_HLSL = R"(
cbuffer PreviewCB : register(b0)
{
    float4x4 uMVP;
    float4x4 uModel;
    float3 uCameraPos;
    float _pad0;
    float3 uLightDir;
    float _pad1;
    float4 uLightColor;
    float uAmbient;
    float3 _pad2;
};

struct VSIn
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
};

struct VSOut
{
    float4 Position : SV_POSITION;
    float3 Normal   : NORMAL;
    float3 WorldPos : TEXCOORD0;
};

VSOut main(VSIn input)
{
    VSOut output;
    float4 worldPos = mul(uModel, float4(input.Position, 1.0));
    output.Position = mul(uMVP, float4(input.Position, 1.0));
    output.WorldPos = worldPos.xyz;
    output.Normal = mul((float3x3)uModel, input.Normal);
    return output;
}
)";

static const char* PREVIEW_PS_HLSL = R"(
cbuffer PreviewCB : register(b0)
{
    float4x4 uMVP;
    float4x4 uModel;
    float3 uCameraPos;
    float _pad0;
    float3 uLightDir;
    float _pad1;
    float4 uLightColor;
    float uAmbient;
    float3 _pad2;
};

struct PSIn
{
    float4 Position : SV_POSITION;
    float3 Normal   : NORMAL;
    float3 WorldPos : TEXCOORD0;
};

float4 main(PSIn input) : SV_Target
{
    // Normalize interpolated normal
    float3 N = normalize(input.Normal);

    // Simple directional lighting
    float3 L = normalize(-uLightDir);
    float NdotL = max(dot(N, L), 0.0);

    // Basic material color (gray with slight blue tint)
    float3 baseColor = float3(0.7, 0.75, 0.95);

    // Combine ambient + diffuse
    float3 ambient = baseColor * uAmbient;
    float3 diffuse = baseColor * uLightColor.rgb * NdotL;
    float3 finalColor = ambient + diffuse;

    return float4(finalColor, 1.0);
}
)";

bool MeshPreviewRenderer::Initialize(nvrhi::IDevice* device, Renderer* renderer, uint32_t width, uint32_t height)
{
    if (!device || !renderer)
    {
        SM_ERROR("MeshPreviewRenderer::Initialize: Invalid device or renderer");
        return false;
    }

    m_Device = device;
    m_Renderer = renderer;
    m_Width = width;
    m_Height = height;

    // Create command list
    m_CommandList = m_Device->createCommandList();
    if (!m_CommandList)
    {
        SM_ERROR("MeshPreviewRenderer::Initialize: Failed to create command list");
        return false;
    }

    // Create render targets
    CreateRenderTargets();

    // Create shaders
    CreateShaders();

    // Create constant buffer
    m_ConstantBuffer = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(PreviewCB), "MeshPreview CB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    if (!m_ConstantBuffer)
    {
        SM_ERROR("MeshPreviewRenderer::Initialize: Failed to create constant buffer");
        return false;
    }

    // Create binding layout
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0)
    };
    nvrhi::VulkanBindingOffsets& offsets =
       nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0);

    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        layoutDesc.setBindingOffsets(offsets);
    }

    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    if (!m_BindingLayout)
    {
        SM_ERROR("MeshPreviewRenderer::Initialize: Failed to create binding layout");
        return false;
    }

    // Create binding set
    nvrhi::BindingSetDesc bindingDesc;
    bindingDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_ConstantBuffer)
    };
    m_BindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

    if (!m_BindingSet)
    {
        SM_ERROR("MeshPreviewRenderer::Initialize: Failed to create binding set");
        return false;
    }

    SM_TRACE("MeshPreviewRenderer::Initialize: Success (%ux%u)", m_Width, m_Height);
    return true;
}

void MeshPreviewRenderer::CreateRenderTargets()
{
    // Create color texture
    nvrhi::TextureDesc colorDesc;
    colorDesc.width = m_Width;
    colorDesc.height = m_Height;
    colorDesc.depth = 1;
    colorDesc.arraySize = 1;
    colorDesc.mipLevels = 1;
    colorDesc.sampleCount = 1;
    colorDesc.dimension = nvrhi::TextureDimension::Texture2D;
    colorDesc.format = nvrhi::Format::RGBA8_UNORM;
    colorDesc.debugName = "MeshPreview Color";
    colorDesc.isRenderTarget = true;
    colorDesc.isShaderResource = true;
    colorDesc.initialState = nvrhi::ResourceStates::RenderTarget;
    colorDesc.keepInitialState = true;

    m_ColorTexture = m_Device->createTexture(colorDesc);

    // Create depth texture
    nvrhi::TextureDesc depthDesc;
    depthDesc.width = m_Width;
    depthDesc.height = m_Height;
    depthDesc.depth = 1;
    depthDesc.arraySize = 1;
    depthDesc.mipLevels = 1;
    depthDesc.sampleCount = 1;
    depthDesc.dimension = nvrhi::TextureDimension::Texture2D;
    depthDesc.format = nvrhi::Format::D24S8;
    depthDesc.debugName = "MeshPreview Depth";
    depthDesc.isRenderTarget = true;
    depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
    depthDesc.keepInitialState = true;

    m_DepthTexture = m_Device->createTexture(depthDesc);

    // Create framebuffer
    nvrhi::FramebufferDesc fbDesc;
    fbDesc.addColorAttachment(m_ColorTexture);
    fbDesc.setDepthAttachment(m_DepthTexture);
    m_Framebuffer = m_Device->createFramebuffer(fbDesc);

    SM_TRACE("MeshPreviewRenderer::CreateRenderTargets: Created %ux%u targets", m_Width, m_Height);
}

void MeshPreviewRenderer::CreateShaders()
{
    // Compile vertex shader using Renderer's shader compilation
    m_VS = m_Renderer->CreateShader(
        nvrhi::ShaderType::Vertex,
        PREVIEW_VS_HLSL,
        strlen(PREVIEW_VS_HLSL),
        "main",
        "vs_6_1"
    );

    if (!m_VS)
    {
        SM_ERROR("MeshPreviewRenderer::CreateShaders: Failed to compile vertex shader");
        return;
    }

    // Compile pixel shader using Renderer's shader compilation
    m_PS = m_Renderer->CreateShader(
        nvrhi::ShaderType::Pixel,
        PREVIEW_PS_HLSL,
        strlen(PREVIEW_PS_HLSL),
        "main",
        "ps_6_1"
    );

    if (!m_PS)
    {
        SM_ERROR("MeshPreviewRenderer::CreateShaders: Failed to compile pixel shader");
        return;
    }

    // Create input layout
    nvrhi::VertexAttributeDesc attrs[3];
    attrs[0].setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(0).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    attrs[1].setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(12).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    attrs[2].setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT)
        .setOffset(24).setBufferIndex(0).setElementStride(sizeof(MeshVertex));

    m_InputLayout = m_Device->createInputLayout(attrs, 3, m_VS);

    SM_TRACE("MeshPreviewRenderer::CreateShaders: Shaders compiled successfully");
}

void MeshPreviewRenderer::CreatePipeline()
{
    if (m_Pipeline)
        return; // Already created

    if (!m_Framebuffer)
    {
        SM_ERROR("MeshPreviewRenderer::CreatePipeline: Framebuffer not ready");
        return;
    }

    nvrhi::GraphicsPipelineDesc psoDesc;
    psoDesc.VS = m_VS;
    psoDesc.PS = m_PS;
    psoDesc.inputLayout = m_InputLayout;
    psoDesc.bindingLayouts = { m_BindingLayout };
    psoDesc.primType = nvrhi::PrimitiveType::TriangleList;

    // Enable depth testing
    psoDesc.renderState.depthStencilState.depthTestEnable = true;
    psoDesc.renderState.depthStencilState.depthWriteEnable = true;
    psoDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Less;

    // Backface culling
    psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
    psoDesc.renderState.rasterState.setFrontCounterClockwise(true);

    // No blending (opaque) - blending is disabled by default, no need to explicitly set it

    m_Pipeline = m_Device->createGraphicsPipeline(psoDesc, m_Framebuffer->getFramebufferInfo());

    SM_TRACE("MeshPreviewRenderer::CreatePipeline: Pipeline created");
}

nvrhi::ITexture* MeshPreviewRenderer::RenderMeshPreview(
    MeshSystem* meshSystem,
    uint64_t meshId,
    float cameraDistance,
    float cameraYaw,
    float cameraPitch)
{
    if (!meshSystem || !m_Device || !m_Framebuffer)
        return nullptr;

    // Get mesh resources
    auto meshResources = meshSystem->GetMeshResources(meshId);
    if (!meshResources.valid)
        return nullptr;

    auto meshBounds = meshSystem->GetMeshBounds(meshId);
    if (!meshBounds.valid)
        return nullptr;

    // Create pipeline if needed
    if (!m_Pipeline)
        CreatePipeline();

    if (!m_Pipeline)
        return nullptr;

    // Calculate mesh center and size for camera framing
    glm::vec3 meshCenter = (meshBounds.min + meshBounds.max) * 0.5f;
    glm::vec3 meshSize = meshBounds.max - meshBounds.min;
    float maxExtent = glm::max(glm::max(meshSize.x, meshSize.y), meshSize.z);

    // Clamp camera distance based on mesh size
    float minDistance = maxExtent * 0.5f;
    float maxDistance = maxExtent * 10.0f;
    float clampedDistance = glm::clamp(cameraDistance, minDistance, maxDistance);

    // Build camera transform (orbital camera around mesh center)
    float camX = meshCenter.x + clampedDistance * std::cos(cameraPitch) * std::sin(cameraYaw);
    float camY = meshCenter.y + clampedDistance * std::sin(cameraPitch);
    float camZ = meshCenter.z + clampedDistance * std::cos(cameraPitch) * std::cos(cameraYaw);
    glm::vec3 cameraPos(camX, camY, camZ);

    glm::mat4 view = glm::lookAt(cameraPos, meshCenter, glm::vec3(0.0f, 1.0f, 0.0f));
    float aspect = static_cast<float>(m_Width) / static_cast<float>(m_Height);
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 1000.0f);

    // Model matrix (identity - mesh is already in world space)
    glm::mat4 model = glm::mat4(1.0f);

    // Update constant buffer
    PreviewCB cb{};
    cb.MVP = proj * view * model;
    cb.Model = model;
    cb.CameraPos = cameraPos;
    cb.LightDir = glm::normalize(glm::vec3(0.3f, -1.0f, 0.5f));
    cb.LightColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    cb.Ambient = 0.3f;

    m_CommandList->open();
    m_CommandList->writeBuffer(m_ConstantBuffer, &cb, sizeof(cb));

    // Clear render targets
    nvrhi::utils::ClearColorAttachment(m_CommandList, m_Framebuffer, 0, nvrhi::Color(0.15f, 0.15f, 0.18f, 1.0f));
    m_CommandList->clearDepthStencilTexture(m_DepthTexture, nvrhi::AllSubresources, true, 1.0f, false, 0);

    // Set graphics state
    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = m_Framebuffer;
    state.viewport.addViewportAndScissorRect(m_Framebuffer->getFramebufferInfo().getViewport());
    state.bindings = { m_BindingSet };
    state.vertexBuffers = { nvrhi::VertexBufferBinding(meshResources.vertexBuffer, 0, 0) };
    state.indexBuffer = nvrhi::IndexBufferBinding(meshResources.indexBuffer, nvrhi::Format::R32_UINT, 0);

    m_CommandList->setGraphicsState(state);

    // Draw mesh
    nvrhi::DrawArguments args{};
    args.vertexCount = meshResources.indexCount;
    args.instanceCount = 1;
    args.startIndexLocation = 0;
    args.startVertexLocation = 0;
    m_CommandList->drawIndexed(args);

    m_CommandList->close();
    m_Device->executeCommandList(m_CommandList);

    return m_ColorTexture.Get();
}

void MeshPreviewRenderer::Resize(uint32_t width, uint32_t height)
{
    if (m_Width == width && m_Height == height)
        return;

    m_Width = width;
    m_Height = height;

    // Recreate render targets
    m_ColorTexture = nullptr;
    m_DepthTexture = nullptr;
    m_Framebuffer = nullptr;
    m_Pipeline = nullptr; // Will be recreated on next render

    CreateRenderTargets();

    SM_TRACE("MeshPreviewRenderer::Resize: Resized to %ux%u", m_Width, m_Height);
}

void MeshPreviewRenderer::Shutdown()
{
    m_Pipeline = nullptr;
    m_BindingSet = nullptr;
    m_BindingLayout = nullptr;
    m_ConstantBuffer = nullptr;
    m_InputLayout = nullptr;
    m_PS = nullptr;
    m_VS = nullptr;
    m_Framebuffer = nullptr;
    m_DepthTexture = nullptr;
    m_ColorTexture = nullptr;
    m_CommandList = nullptr;
    m_Device = nullptr;

    SM_TRACE("MeshPreviewRenderer::Shutdown: Cleanup complete");
}
