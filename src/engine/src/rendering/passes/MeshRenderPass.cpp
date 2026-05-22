#include "MeshRenderPass.h"

#include "Renderer.h"
#include "MeshBatching.h"
#include "Frustum.h"
#include "RenderStats.h"
#include <nvrhi/utils.h>
#include "lib.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

// HLSL shaders for mesh pass
static const char* MESH_VS_HLSL = R"(
struct DirectionalLight {
    float4 Direction; // xyz = light direction
    float4 Color;
};

struct InstanceData {
    float4x4 Model;
    float4x4 NormalMatrix;
    float4 BaseColor;
    uint Flags;
    uint3 _pad;
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

StructuredBuffer<InstanceData> gInstances : register(t5);

struct VSIn
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    uint InstanceID : SV_InstanceID;
};

struct VSOut
{
    float4 PosH   : SV_POSITION;
    float3 Normal : NORMAL;
    float2 UV     : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    uint InstanceID : TEXCOORD2;
};

VSOut main_vs(VSIn vin)
{
    VSOut o;

    // Fetch per-instance data
    InstanceData inst = gInstances[vin.InstanceID];

    float4 lp = float4(vin.Position, 1.0);
    float4 wp = mul(inst.Model, lp);
    o.PosH = mul(uVP, wp);

    o.Normal = mul((float3x3)inst.NormalMatrix, vin.Normal);

    o.UV = vin.UV;
    o.WorldPos = wp.xyz;
    o.InstanceID = vin.InstanceID;
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

struct InstanceData {
    float4x4 Model;
    float4x4 NormalMatrix;
    float4 BaseColor;
    uint Flags;
    uint3 _pad;
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
StructuredBuffer<InstanceData> gInstances : register(t5);

static const uint OPT_SAMPLE_TEXTURE = 1u << 0;
static const uint OPT_UNLIT          = 1u << 1;

struct PSIn
{
    float4 PosH   : SV_POSITION;
    float3 Normal : NORMAL;
    float2 UV     : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    uint InstanceID : TEXCOORD2;
};

float4 main_ps(PSIn i) : SV_Target
{
    // Fetch per-instance data
    InstanceData inst = gInstances[i.InstanceID];
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

    if ((inst.Flags & OPT_UNLIT) != 0u)
    {
        float4 c = ((inst.Flags & OPT_SAMPLE_TEXTURE) != 0)
            ? uTexture.Sample(uSampler, i.UV)
            : float4(inst.BaseColor.rgb, inst.BaseColor.a);
        return c; // bypass lighting
    }

    float4 finalColor;
    if ((inst.Flags & OPT_SAMPLE_TEXTURE) != 0) {
        finalColor = uTexture.Sample(uSampler, i.UV);
        finalColor.rgb *= lighting;
    } else {
        finalColor.rgb = inst.BaseColor.rgb * lighting;
        finalColor.a = inst.BaseColor.a;
    }
    return finalColor;
}
)";

bool MeshRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer || !renderer->GetMeshSystem() || !renderer->GetMaterialSystem())
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

    // Binding layout: b0 (PerFrame), b1 (PerDraw), t2 (Texture), s3 (Sampler), t4 (PointLights), t5 (Instances)
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),
        nvrhi::BindingLayoutItem::ConstantBuffer(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Sampler(3),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5)
    };
    nvrhi::VulkanBindingOffsets& offsets =
        nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0);

    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        layoutDesc.setBindingOffsets(offsets);
    }

    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

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

    // Create instance buffer (structured buffer SRV)
    {
        nvrhi::BufferDesc bd;
        bd.debugName = "MeshRenderPass InstanceBuffer";
        bd.byteSize = m_MaxInstances * sizeof(MeshInstanceCPU);
        bd.structStride = sizeof(MeshInstanceCPU);
        bd.initialState = nvrhi::ResourceStates::CopyDest;
        bd.isIndexBuffer = false;
        bd.isVertexBuffer = false;
        bd.isConstantBuffer = false;
        bd.canHaveUAVs = false;
        bd.keepInitialState = true;
        m_InstanceBuffer = m_Device->createBuffer(bd);
    }

    return true;
}

static glm::mat4 BuildWorldMatrix(const TransformComponent& t)
{
    glm::mat4 T  = glm::translate(glm::mat4(1.0f), t.Position);
    glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), t.Rotation.x, glm::vec3(1.f, 0.f, 0.f));
    glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), t.Rotation.y, glm::vec3(0.f, 1.f, 0.f));
    glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), t.Rotation.z, glm::vec3(0.f, 0.f, 1.f));
    glm::mat4 S  = glm::scale(glm::mat4(1.0f), t.Scale);
    return T * Rz * Ry * Rx * S;
}

void MeshRenderPass::Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* frameBuffer,
                            SimulationSnapshot& snapshot,
                            const ECS* world,
                            double /*deltaTime*/,
                            FrameAllocator* frameAllocator)
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

    commandList->beginMarker("MeshRenderPass");

    commandList->clearDepthStencilTexture(frameBuffer->getDesc().depthAttachment.texture, nvrhi::AllSubresources, true, 1.0f, false, 0);

    // ECS-driven rendering: TransformComponent + MeshComponent are required; MaterialComponent is optional
    if (world)
    {
        // Gather lights
        glm::vec4 lightningDirection(0.0f, -1.0f, 0.0f, 0.0f); // default arbitrary direction
        glm::vec4 lightningColor(0.0f);

        // Collect point lights into an arena array (capped to m_MaxPointLights)
        auto* pointLights = frameAllocator->AllocateArray<PointLightCPU>(m_MaxPointLights);
        uint32_t pointLightCount = 0;
        if (!pointLights)
            SM_WARN("MeshRenderPass: frame arena exhausted, point-light buffer not allocated");

        world->Each<TransformComponent, LightningComponent>(
            [&](EntityId, const TransformComponent& transform, const LightningComponent& lightning)
        {
            if (lightning.Type == LightningType::Directional)
            {
                lightningDirection = lightning.Direction;
                lightningColor = lightning.Color;
                // keep scanning for points in case; do not break
            }
            else if (lightning.Type == LightningType::Point)
            {
                if (pointLights && pointLightCount < m_MaxPointLights)
                {
                    PointLightCPU pl{};
                    pl.Position = glm::vec4(transform.Position, 1.0f);
                    pl.Color = lightning.Color;
                    pl.Intensity = lightning.Intensity;
                    pl.Range = lightning.Range;
                    pointLights[pointLightCount++] = pl;
                }
            }
        });

        // Update per-frame CB
        PerFrameCB perFrame{};
        glm::mat4 V(1.0f), P(1.0f); glm::vec3 camPos(0.0f);
        if (const auto* cam = world ? world->GetSingleton<WorldCameraComponent>() : nullptr) {
            V = cam->View; P = cam->Projection; camPos = cam->Position;
        }
        perFrame.P  = P;
        perFrame.VP = P * V;
        const Frustum cullFrustum = ExtractFrustum(perFrame.VP);
        const bool cullEnabled = GetCullingSettings().Enabled;
        MeshSystem* meshSystem = m_Renderer->GetMeshSystem();
        uint32_t culledCount = 0;
        perFrame.DirectionalLight.Direction = lightningDirection;
        perFrame.DirectionalLight.Color = lightningColor;
        perFrame.PointLightCount = pointLightCount;
        perFrame.Ambient = 0.1f; // hardcoded ambient for now
        commandList->writeBuffer(m_PerFrameCB, &perFrame, sizeof(perFrame));

        // Upload point lights data (if any)
        if (m_PointLightBuffer && pointLightCount > 0)
        {
            commandList->writeBuffer(m_PointLightBuffer, pointLights, pointLightCount * sizeof(PointLightCPU));
        }

        // Group entities by (MeshId, MaterialId) into a flat arena array, then
        // sort into contiguous runs (one run == one draw batch). No node-based map.
        auto* entries = frameAllocator->AllocateArray<BatchEntry>(world->GetEntityCount());
        uint32_t entryCount = 0;
        if (entries)
        {
            world->Each<TransformComponent, MeshComponent>(
                [&](EntityId e, const TransformComponent& transform, const MeshComponent& meshComp)
            {
                if (!meshComp.Visible)
                    return;

                if (cullEnabled && meshSystem)
                {
                    const auto bounds = meshSystem->GetMeshBounds(meshComp.MeshId);
                    if (bounds.valid) // invalid/unloaded bounds -> never cull
                    {
                        glm::vec3 wMin, wMax;
                        TransformAABB(BuildWorldMatrix(transform), bounds.min, bounds.max, wMin, wMax);
                        if (!IsAABBVisible(cullFrustum, wMin, wMax))
                        {
                            ++culledCount;
                            return;
                        }
                    }
                }

                const auto* materialComp = world->GetComponent<MaterialComponent>(e);
                uint32_t materialId = materialComp ? materialComp->MaterialId : MaterialSystem::MissingMaterial;

                entries[entryCount++] = BatchEntry{ meshComp.MeshId, materialId, e };
            });
        }
        else if (world->GetEntityCount() > 0)
        {
            char warn[128];
            snprintf(warn, sizeof(warn),
                     "MeshRenderPass: frame arena exhausted, dropped up to %zu entities (no meshes drawn)",
                     world->GetEntityCount());
            SM_WARN(warn);
        }

        BatchRun* runs = (entryCount > 0) ? frameAllocator->AllocateArray<BatchRun>(entryCount) : nullptr;
        if (entryCount > 0 && entries && !runs)
            SM_WARN("MeshRenderPass: frame arena exhausted, batch runs not allocated (no meshes drawn)");
        uint32_t runCount = (entries && runs) ? BuildBatchRuns(entries, entryCount, runs, entryCount) : 0;

        // Render each batch (run) with instancing
        nvrhi::GraphicsState state;
        state.pipeline = m_Pipeline;
        state.framebuffer = frameBuffer;
        state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());

        uint32_t instancesDrawn = 0;

        for (uint32_t r = 0; r < runCount; ++r)
        {
            const BatchRun& run = runs[r];
            const BatchEntry& head = entries[run.begin];

            // Query systems for GPU resources
            auto meshResources = m_Renderer->GetMeshSystem()->GetMeshResources(head.meshId);
            if (!meshResources.valid)
            {
                SM_WARN("MeshRenderPass: Invalid mesh ID %u", head.meshId);
                meshResources = m_Renderer->GetMeshSystem()->GetMeshResources(MeshSystem::MissingMesh);
            }

            const uint32_t instanceCount = std::min(run.count, m_MaxInstances);

            // Build instance data into an arena array. Mark the arena so this run's
            // instances can be reclaimed after they're uploaded (writeBuffer copies
            // immediately), keeping peak arena usage flat across many batches.
            const auto instanceMark = frameAllocator->GetMarker();
            auto* instances = frameAllocator->AllocateArray<MeshInstanceCPU>(instanceCount);
            if (!instances)
            {
                char warn[128];
                snprintf(warn, sizeof(warn),
                         "MeshRenderPass: frame arena exhausted, dropped %u instances (mesh %u)",
                         instanceCount, head.meshId);
                SM_WARN(warn);
                continue;
            }
            uint32_t instanceOut = 0;

            for (uint32_t i = 0; i < instanceCount; ++i)
            {
                EntityId entity = entries[run.begin + i].entity;
                const auto* transform = world->GetComponent<TransformComponent>(entity);
                const auto* material = world->GetComponent<MaterialComponent>(entity);

                if (!transform)
                    continue;

                // Build world transform (shared with the cull test so they cannot diverge)
                glm::mat4 M = BuildWorldMatrix(*transform);

                glm::mat3 M3(M);
                glm::mat3 N3 = glm::transpose(glm::inverse(M3));

                // Material properties
                glm::vec4 baseColor = material ? material->BaseColor : glm::vec4(1.0f);
                uint32_t flags = (material && (material->Flags & 1u)) ? 1u : 0u;

                MeshInstanceCPU inst{};
                inst.Model = M;
                inst.NormalMatrix = glm::mat4(N3);
                inst.BaseColor = baseColor;
                inst.Flags = flags;

                instances[instanceOut++] = inst;
            }

            if (instanceOut == 0)
            {
                frameAllocator->RewindTo(instanceMark);
                continue;
            }

            instancesDrawn += instanceOut;

            // Upload instance data
            commandList->writeBuffer(m_InstanceBuffer, instances, instanceOut * sizeof(MeshInstanceCPU));

            if (meshResources.subMeshes.size() > 0) {

                nvrhi::DrawArguments args{};
                for (const auto &subMesh: meshResources.subMeshes)
                {
                    auto materialResources = m_Renderer->GetMaterialSystem()->GetMaterialResources(subMesh.MaterialIndex);
                    if (!materialResources.valid)
                    {
                        SM_WARN("MeshRenderPass: Invalid material ID %u", head.materialId);
                        materialResources = m_Renderer->GetMaterialSystem()->GetMaterialResources(MaterialSystem::MissingMaterial);
                    }

                    // Create binding set dynamically for this batch
                    nvrhi::BindingSetDesc bindingDesc;
                    bindingDesc.bindings = {
                        nvrhi::BindingSetItem::ConstantBuffer(0, m_PerFrameCB),
                        nvrhi::BindingSetItem::ConstantBuffer(1, m_PerDrawCB),
                        nvrhi::BindingSetItem::Texture_SRV(2, materialResources.texture),
                        nvrhi::BindingSetItem::Sampler(3, materialResources.sampler),
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_PointLightBuffer),
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_InstanceBuffer)
                    };
                    nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

                    // Set state and draw ALL instances in one call
                    state.bindings = { bindingSet };
                    state.vertexBuffers = { nvrhi::VertexBufferBinding(meshResources.vertexBuffer, 0, 0) };
                    state.indexBuffer = nvrhi::IndexBufferBinding(meshResources.indexBuffer, nvrhi::Format::R32_UINT, 0);

                    commandList->setGraphicsState(state);

                    args.vertexCount = subMesh.IndexCount;
                    args.instanceCount = instanceOut;
                    args.startIndexLocation = subMesh.IndexStart;
                    args.startVertexLocation = 0;
                    commandList->drawIndexed(args);
                }

            } else {

                auto materialResources = m_Renderer->GetMaterialSystem()->GetMaterialResources(head.materialId);
                if (!materialResources.valid)
                {
                    SM_WARN("MeshRenderPass: Invalid material ID %u", head.materialId);
                    materialResources = m_Renderer->GetMaterialSystem()->GetMaterialResources(MaterialSystem::MissingMaterial);
                }

                // Create binding set dynamically for this batch
                nvrhi::BindingSetDesc bindingDesc;
                bindingDesc.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(0, m_PerFrameCB),
                    nvrhi::BindingSetItem::ConstantBuffer(1, m_PerDrawCB),
                    nvrhi::BindingSetItem::Texture_SRV(2, materialResources.texture),
                    nvrhi::BindingSetItem::Sampler(3, materialResources.sampler),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_PointLightBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_InstanceBuffer)
                };
                nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

                // Set state and draw ALL instances in one call
                state.bindings = { bindingSet };
                state.vertexBuffers = { nvrhi::VertexBufferBinding(meshResources.vertexBuffer, 0, 0) };
                state.indexBuffer = nvrhi::IndexBufferBinding(meshResources.indexBuffer, nvrhi::Format::R32_UINT, 0);

                commandList->setGraphicsState(state);

                nvrhi::DrawArguments args{};
                args.vertexCount = meshResources.indexCount;
                args.instanceCount = instanceOut;
                args.startIndexLocation = 0;
                args.startVertexLocation = 0;
                commandList->drawIndexed(args);
            }

            // This run's instances have been uploaded + drawn; reclaim the arena space.
            frameAllocator->RewindTo(instanceMark);
        }

        RenderStats& rs = GetRenderStats();
        rs.MeshEntitiesDrawn  = entryCount;
        rs.MeshEntitiesCulled = culledCount;
        rs.MeshEntitiesTotal  = entryCount + culledCount;
        rs.InstancesDrawn     = instancesDrawn;
        rs.BatchesDrawn       = runCount;
    }

    commandList->endMarker();
}

void MeshRenderPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_InputLayout = nullptr;
    m_BindingLayout = nullptr;

    m_PerFrameCB = nullptr;
    m_PerDrawCB = nullptr;
    m_PointLightBuffer = nullptr;
    m_InstanceBuffer = nullptr;
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
