#include "MeshRenderPass.h"

#include "Renderer.h"
#include <nvrhi/utils.h>
#include "lib.h"
#include <glm/gtc/matrix_transform.hpp>

// HLSL shaders for mesh pass
static const char* MESH_VS_HLSL = R"(
struct DirectionalLight {
    float4 Direction; // xyz = light direction
    float4 Color;
};

cbuffer PerFrame : register(b0)
{
    float4x4 uP;
    float4x4 uVP;
    DirectionalLight uDirLight;
    uint uPointLightCount;
    float uAmbient;
    float2 _pfPad;
};

cbuffer PerDraw : register(b1)
{
    float4x4 uModel;
    float4x4 uNormalMatrix;
    float4   uBaseColor;
    uint     uFlags; // bit0: sample texture
    uint3    _pad;
};

struct VSIn
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
};

struct VSOut
{
    float4 PosH   : SV_POSITION;
    float3 Normal : NORMAL;
    float2 UV     : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
};

VSOut main_vs(VSIn vin)
{
    VSOut o;
    float4 lp = float4(vin.Position, 1.0);
    float4 wp = mul(uModel, lp);
    o.PosH = mul(uVP, wp);

    //o.Normal = mul((float3x3)uModel, vin.Normal);
    o.Normal = mul((float3x3)uNormalMatrix, vin.Normal);

    o.UV = vin.UV;
    o.WorldPos = wp.xyz;
    return o;
}
)";

static const char* MESH_PS_HLSL = R"(
struct DirectionalLight {
    float4 Direction; // xyz = light direction
    float4 Color;
};

struct PointLight {
    float4 Position;
    float4 Color;
    float Intensity;
    float Range;
    float2 _pad;
};

cbuffer PerFrame : register(b0)
{
    float4x4 uP;
    float4x4 uVP;
    DirectionalLight uDirLight;
    uint uPointLightCount;
    float uAmbient;
    float2 _pfPad;
};

cbuffer PerDraw : register(b1)
{
    float4x4 uModel;
    float4x4 uNormalMatrix;
    float4   uBaseColor;
    uint     uFlags; // bit0: sample texture
    uint3    _pad;
};

Texture2D uTexture : register(t2);
SamplerState uSampler : register(s3);
StructuredBuffer<PointLight> gPointLights : register(t4);

static const uint OPT_SAMPLE_TEXTURE = 1u << 0;
static const uint OPT_UNLIT          = 1u << 1;

struct PSIn
{
    float4 PosH   : SV_POSITION;
    float3 Normal : NORMAL;
    float2 UV     : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
};

float4 main_ps(PSIn i) : SV_Target
{
    // Use dynamic light direction from constant buffer
    float3 lightDir = normalize(uDirLight.Direction.xyz);

    float3 N = normalize(i.Normal);
    float diffuse = max(dot(N, -lightDir), 0.0);

    float ambient = uAmbient;
    float3 lighting = ambient * uDirLight.Color.rgb; // or just float3(ambient) if you don’t want ambient tinted by sun
    // Apply light color to diffuse and ambient lightning
    lighting += diffuse * uDirLight.Color.rgb;

    // Add point lights contribution
    [loop]
    for (uint idx = 0; idx < uPointLightCount; ++idx)
    {
        PointLight pl = gPointLights[idx];
        float3 L = pl.Position.xyz - i.WorldPos;
        float dist = length(L);
        if (pl.Range > 0.0001)
        {
            float3 Ldir = L / max(dist, 1e-5);
            float NdotL = max(dot(N, Ldir), 0.0);
            // Smooth falloff: saturate(1 - (d/r)^2)
            float falloff = saturate(1.0 - (dist / pl.Range) * (dist / pl.Range));
            float contrib = pl.Intensity * NdotL * falloff;
            lighting += pl.Color.rgb * contrib;
        }
    }

    if ((uFlags & OPT_UNLIT) != 0u)
    {
        float4 c = ((uFlags & OPT_SAMPLE_TEXTURE) != 0)
            ? uTexture.Sample(uSampler, i.UV)
            : float4(uBaseColor.rgb, uBaseColor.a);
        return c; // bypass lighting
    }

    float4 finalColor;
    if ((uFlags & OPT_SAMPLE_TEXTURE) != 0) {
        finalColor = uTexture.Sample(uSampler, i.UV);
        finalColor.rgb *= lighting;
    } else {
        finalColor.rgb = uBaseColor.rgb * lighting;
        finalColor.a = uBaseColor.a;
    }
    return finalColor;
}
)";

bool MeshRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer)
        return false;

    // Create constant buffers
    m_PerFrameCB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(PerFrameCB), "MeshRenderPass PerFrameCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    m_PerDrawCB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(PerDrawCB), "MeshRenderPass PerDrawCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    // Compile shaders
    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, MESH_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel,  MESH_PS_HLSL, 0, "main_ps", "ps_6_1");
    if (!m_VS || !m_PS)
        return false;

    // Input layout: POSITION (RGB32F), TEXCOORD (RG32F)
    nvrhi::VertexAttributeDesc attrs[3];
    attrs[0].setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(MeshVertex, px)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    attrs[1].setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(MeshVertex, nx)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    attrs[2].setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT)
        .setOffset(offsetof(MeshVertex, u)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    m_InputLayout = m_Device->createInputLayout(attrs, 3, m_VS);

    // Binding layout: b0 (PerFrame), b1 (PerDraw), t2 (Texture), s3 (Sampler), t4 (PointLights)
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),
        nvrhi::BindingLayoutItem::ConstantBuffer(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Sampler(3),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4)
    };
    nvrhi::VulkanBindingOffsets& offsets =
        nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0);

    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        layoutDesc.setBindingOffsets(offsets);
    }

    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    // Sampler
    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true);
    sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    m_Sampler = m_Device->createSampler(sd);

    // Default white texture (1x1 RGBA8)
    {
        nvrhi::TextureDesc td;
        td.debugName = "MeshRenderPass DefaultWhite";
        td.width = 1; td.height = 1; td.depth = 1;
        td.arraySize = 1; td.mipLevels = 1;
        td.sampleCount = 1;
        td.dimension = nvrhi::TextureDimension::Texture2D;
        td.format = nvrhi::Format::RGBA8_UNORM;
        td.initialState = nvrhi::ResourceStates::CopyDest;
        //td.keepInitialState = true;
        td.isShaderResource = true;
        m_DefaultWhite = m_Device->createTexture(td);

        uint32_t pixel = 0xFFFFFFFFu; // white
        const auto cl = m_Device->createCommandList();
        cl->open();
        cl->beginTrackingTextureState(m_DefaultWhite, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
        cl->writeTexture(m_DefaultWhite, 0, 0, &pixel, sizeof(uint32_t));
        cl->setPermanentTextureState(m_DefaultWhite, nvrhi::ResourceStates::ShaderResource);
        cl->close();
        m_Device->executeCommandList(cl);
    }

    // Create PointLight structured buffer (SRV)
    {
        nvrhi::BufferDesc bd;
        bd.debugName = "MeshRenderPass PointLights";
        bd.byteSize = m_MaxPointLights * sizeof(PointLightCPU);
        bd.structStride = sizeof(PointLightCPU);
        bd.initialState = nvrhi::ResourceStates::CopyDest; // upload each frame
        bd.isIndexBuffer = false;
        bd.isVertexBuffer = false;
        bd.isConstantBuffer = false;
        bd.canHaveUAVs = false;
        bd.keepInitialState = true;
        m_PointLightBuffer = m_Device->createBuffer(bd);
    }

    return true;
}

ModelHandle MeshRenderPass::AddModel(const MeshVertex* vertices, uint32_t vertexCount,
                                                    const uint32_t* indices, uint32_t indexCount,
                                                    bool useTexture,
                                                    const uint32_t* textureRgba8,
                                                    uint32_t texWidth, uint32_t texHeight)
{
    Model model{};
    model.indexCount = indexCount;
    model.useTexture = useTexture;

    // Upload VB/IB
    auto cl = m_Device->createCommandList(nvrhi::CommandListParameters().setQueueType(nvrhi::CommandQueue::Graphics));
    cl->open();

    // VB
    nvrhi::BufferDesc vb;
    vb.debugName = "MeshPass VB";
    vb.byteSize = sizeof(MeshVertex) * vertexCount;
    vb.isVertexBuffer = true;
    vb.initialState = nvrhi::ResourceStates::CopyDest;
    model.vertexBuffer = m_Device->createBuffer(vb);
    cl->beginTrackingBufferState(model.vertexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(model.vertexBuffer, vertices, vb.byteSize);
    cl->setPermanentBufferState(model.vertexBuffer, nvrhi::ResourceStates::VertexBuffer);

    // IB (assume 32-bit indices)
    nvrhi::BufferDesc ib;
    ib.debugName = "MeshPass IB";
    ib.byteSize = sizeof(uint32_t) * indexCount;
    ib.isIndexBuffer = true;
    ib.initialState = nvrhi::ResourceStates::CopyDest;
    model.indexBuffer = m_Device->createBuffer(ib);
    cl->beginTrackingBufferState(model.indexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(model.indexBuffer, indices, ib.byteSize);
    cl->setPermanentBufferState(model.indexBuffer, nvrhi::ResourceStates::IndexBuffer);

    // Texture (optional)
    nvrhi::TextureHandle tex = m_DefaultWhite;
    if (useTexture && textureRgba8 && texWidth > 0 && texHeight > 0)
    {
        nvrhi::TextureDesc td;
        td.debugName = "MeshPass ModelTexture";
        td.width = texWidth; td.height = texHeight; td.depth = 1;
        td.arraySize = 1; td.mipLevels = 1; td.sampleCount = 1;
        td.dimension = nvrhi::TextureDimension::Texture2D;
        td.format = nvrhi::Format::RGBA8_UNORM;
        td.initialState = nvrhi::ResourceStates::CopyDest;
        td.keepInitialState = true;
        tex = m_Device->createTexture(td);

        // Row pitch is width * 4
        cl->beginTrackingTextureState(tex, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
        cl->writeTexture(tex, 0, 0, textureRgba8, texWidth * 4);
        cl->setPermanentTextureState(tex, nvrhi::ResourceStates::ShaderResource);
    }
    model.texture = tex;

    cl->close();
    m_Device->executeCommandList(cl);

    // Create binding set for this model (per-frame, per-draw, texture, sampler, point lights)
    nvrhi::BindingSetDesc bs;
    bs.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_PerFrameCB),
        nvrhi::BindingSetItem::ConstantBuffer(1, m_PerDrawCB),
        nvrhi::BindingSetItem::Texture_SRV(2, model.texture),
        nvrhi::BindingSetItem::Sampler(3, m_Sampler),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_PointLightBuffer)
    };
    model.bindingSet = m_Device->createBindingSet(bs, m_BindingLayout);

    m_Models.push_back(model);
    return ModelHandle{ static_cast<uint32_t>(m_Models.size() - 1) };
}

void MeshRenderPass::Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* frameBuffer,
                            SimulationSnapshot& snapshot,
                            double /*deltaTime*/,
                            FrameAllocator* /*frameAllocator*/)
{
    if (!m_Pipeline)
    {
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = m_PS;
        pso.inputLayout = m_InputLayout;
        pso.bindingLayouts = { m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        // Depth disabled for now (no depth buffer in target)
        pso.renderState.depthStencilState.depthTestEnable = true;
        pso.renderState.depthStencilState.depthWriteEnable = true;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
        pso.renderState.rasterState.setFrontCounterClockwise(true);
        nvrhi::BlendState::RenderTarget rt;
        rt.setBlendEnable(true)
          .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
          .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
          .setBlendOp(nvrhi::BlendOp::Add)
          .setSrcBlendAlpha(nvrhi::BlendFactor::One)
          .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
          .setBlendOpAlpha(nvrhi::BlendOp::Add)
          .setColorWriteMask(nvrhi::ColorMask::All);
        pso.renderState.blendState.setRenderTarget(0, rt);
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    commandList->clearDepthStencilTexture(
        frameBuffer->getDesc().depthAttachment.texture,
        nvrhi::TextureSubresourceSet(),
        true,
        1.0f,
        false,
        0);

    // ECS-driven rendering: TransformComponent + MeshComponent are required; MaterialComponent is optional
    if (snapshot.WorldSnapshotPtr)
    {
        // Gather lights
        glm::vec4 lightningDirection(0.0f, -1.0f, 0.0f, 0.0f); // default arbitrary direction
        glm::vec4 lightningColor(0.0f);

        // Collect point lights into a temporary CPU array (capped to m_MaxPointLights)
        std::vector<PointLightCPU> pointLights;
        pointLights.reserve(16);

        for (EntityId entity : snapshot.WorldSnapshotPtr->View<TransformComponent, LightningComponent>()) {
            const auto* transform = snapshot.WorldSnapshotPtr->GetComponent<TransformComponent>(entity);
            const auto* lightning = snapshot.WorldSnapshotPtr->GetComponent<LightningComponent>(entity);
            if (!transform || !lightning) continue;

            if (lightning->Type == LightningType::Directional)
            {
                lightningDirection = lightning->Direction;
                lightningColor = lightning->Color;
                // keep scanning for points in case; do not break
            }
            else if (lightning->Type == LightningType::Point)
            {
                if (pointLights.size() < m_MaxPointLights)
                {
                    PointLightCPU pl{};
                    pl.Position = glm::vec4(transform->Position, 1.0f);
                    pl.Color = lightning->Color;
                    pl.Intensity = lightning->Intensity;
                    pl.Range = lightning->Range;
                    pointLights.push_back(pl);
                }
            }
        }

        // Update per-frame CB
        PerFrameCB perFrame{};
        const glm::mat4 V = snapshot.GameCamera.get_view_matrix();
        const glm::mat4 P = snapshot.GameCamera.get_projection_matrix();
        perFrame.P  = P;
        perFrame.VP = P * V;
        perFrame.DirectionalLight.Direction = lightningDirection;
        perFrame.DirectionalLight.Color = lightningColor;
        perFrame.PointLightCount = static_cast<uint32_t>(pointLights.size());
        perFrame.Ambient = 0.01f; // hardcoded ambient for now
        commandList->writeBuffer(m_PerFrameCB, &perFrame, sizeof(perFrame));

        // Upload point lights data (if any)
        if (m_PointLightBuffer && !pointLights.empty())
        {
            commandList->writeBuffer(m_PointLightBuffer, pointLights.data(), static_cast<uint32_t>(pointLights.size() * sizeof(PointLightCPU)));
        }

        nvrhi::GraphicsState state;
        state.pipeline = m_Pipeline;
        state.framebuffer = frameBuffer;
        state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());

        for (EntityId entity : snapshot.WorldSnapshotPtr->View<TransformComponent, MeshComponent>())
        {
            const auto* transform = snapshot.WorldSnapshotPtr->GetComponent<TransformComponent>(entity);
            const auto* mesh = snapshot.WorldSnapshotPtr->GetComponent<MeshComponent>(entity);
            if (!transform || !mesh || !mesh->Visible)
                continue;

            // Validate mesh handle
            if (mesh->MeshId >= m_Models.size())
                continue;

            const Model& model = m_Models[mesh->MeshId];

            // Build world transform: T * Rz * Ry * Rx * S (Euler order chosen arbitrarily)
            glm::mat4 T = glm::translate(glm::mat4(1.0f), transform->Position);
            glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), transform->Rotation.x, glm::vec3(1.f, 0.f, 0.f));
            glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), transform->Rotation.y, glm::vec3(0.f, 1.f, 0.f));
            glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), transform->Rotation.z, glm::vec3(0.f, 0.f, 1.f));
            glm::mat4 S = glm::scale(glm::mat4(1.0f), transform->Scale);
            glm::mat4 M = T * Rz * Ry * Rx * S;

            glm::mat3 M3(M);
            glm::mat3 N3 = glm::transpose(glm::inverse(M3));

            // Material defaults
            glm::vec4 baseColor(1.0f);
            uint32_t flags = model.useTexture ? 1u : 0u; // default to model's texture preference

            if (snapshot.WorldSnapshotPtr->HasComponent<MaterialComponent>(entity))
            {
                const auto* material = snapshot.WorldSnapshotPtr->GetComponent<MaterialComponent>(entity);
                if (material)
                {
                    baseColor = material->BaseColor;
                    // Interpret bit 0 as "UseTexture"; if set, use the model's bound texture for now
                    if ((material->Flags & 1u) != 0u)
                        flags |= 1u;
                    else
                        flags &= ~1u;
                }
            }

            // Update per-draw data
            PerDrawCB pd{};
            pd.Model = M;
            pd.NormalMatrix = glm::mat4(N3);
            pd.BaseColor = baseColor;
            pd.Flags = flags;
            commandList->writeBuffer(m_PerDrawCB, &pd, sizeof(pd));

            // Bind and draw
            state.bindings = { model.bindingSet };
            state.vertexBuffers = { nvrhi::VertexBufferBinding(model.vertexBuffer, 0, 0) };
            state.indexBuffer = nvrhi::IndexBufferBinding(model.indexBuffer, nvrhi::Format::R32_UINT, 0);

            commandList->setGraphicsState(state);

            nvrhi::DrawArguments args{};
            args.vertexCount = model.indexCount; // for indexed draws, NVRHI uses vertexCount as index count
            args.instanceCount = 1;
            args.startIndexLocation = 0;
            args.startVertexLocation = 0;
            commandList->drawIndexed(args);
        }
    }
}

void MeshRenderPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_InputLayout = nullptr;
    m_BindingLayout = nullptr;

    for (auto& m : m_Models)
    {
        m.bindingSet = nullptr;
        m.texture = nullptr;
        m.vertexBuffer = nullptr;
        m.indexBuffer = nullptr;
    }
    m_Models.clear();

    m_PerFrameCB = nullptr;
    m_PerDrawCB = nullptr;
    m_Sampler = nullptr;
    m_PointLightBuffer = nullptr;
    m_DefaultWhite = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;
    m_Device = nullptr;
    m_Renderer = nullptr;
}

void MeshRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    // Recreate pipeline on next render with new framebuffer info
    m_Pipeline = nullptr;
}
