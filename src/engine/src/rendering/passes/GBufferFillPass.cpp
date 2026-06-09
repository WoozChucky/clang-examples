#include "GBufferFillPass.h"

#include "Renderer.h"
#include "MeshBatching.h"
#include "Frustum.h"
#include "RenderStats.h"
#include <nvrhi/utils.h>
#include "lib.h"
#include "TransformMath.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include "SkinningComputePass.h"

// Deferred geometry shaders: write albedo / world-normal / world-position into the
// G-buffer MRTs. Registers mirror the mesh-pass scheme (CB b0, texture t2, sampler s3,
// instances t5) so the Vulkan flat-binding offsets line up.
static const char* GBUF_VS_HLSL = R"(
struct InstanceData { float4x4 Model; float4x4 NormalMatrix; float4x4 PrevModel; float4 BaseColor; uint Flags; uint IsSkinned; uint PrevSkinnedOffset; uint _pad; };
struct MeshVertex { float3 Position; float3 Normal; float2 UV; };
cbuffer PerFrame : register(b0) { float4x4 uVP; float4x4 uPrevVP; };
StructuredBuffer<InstanceData> gInstances : register(t5);
StructuredBuffer<MeshVertex>   gPrevSkinned : register(t6);   // previous-frame skinned verts (Task 2 binds; t6 unused when IsSkinned==0)
struct VSIn  { float3 Position:POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; uint VertexID:SV_VertexID; uint InstanceID:SV_InstanceID; };
struct VSOut { float4 PosH:SV_POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; float3 WorldPos:TEXCOORD1; uint InstanceID:TEXCOORD2; float4 CurClip:TEXCOORD3; float4 PrevClip:TEXCOORD4; };
VSOut main_vs(VSIn vin){
    InstanceData inst = gInstances[vin.InstanceID];
    float4 wp = mul(inst.Model, float4(vin.Position,1.0));
    float3 prevPos = (inst.IsSkinned != 0) ? gPrevSkinned[vin.VertexID + inst.PrevSkinnedOffset].Position : vin.Position;
    float4 prevWp = mul(inst.PrevModel, float4(prevPos,1.0));
    VSOut o;
    o.PosH    = mul(uVP, wp);
    o.CurClip = o.PosH;
    o.PrevClip = mul(uPrevVP, prevWp);
    o.Normal = mul((float3x3)inst.NormalMatrix, vin.Normal);
    o.UV = vin.UV; o.WorldPos = wp.xyz; o.InstanceID = vin.InstanceID; return o;
}
)";

static const char* GBUF_PS_HLSL = R"(
struct InstanceData { float4x4 Model; float4x4 NormalMatrix; float4x4 PrevModel; float4 BaseColor; uint Flags; uint IsSkinned; uint PrevSkinnedOffset; uint _pad; };
Texture2D uTexture : register(t2);
SamplerState uSampler : register(s3);
StructuredBuffer<InstanceData> gInstances : register(t5);
static const uint OPT_SAMPLE_TEXTURE = 1u << 0;
struct PSIn { float4 PosH:SV_POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; float3 WorldPos:TEXCOORD1; uint InstanceID:TEXCOORD2; float4 CurClip:TEXCOORD3; float4 PrevClip:TEXCOORD4; };
struct PSOut { float4 Albedo:SV_Target0; float4 Normal:SV_Target1; float4 WorldPos:SV_Target2; float2 Velocity:SV_Target3; };
PSOut main_ps(PSIn i){
    InstanceData inst = gInstances[i.InstanceID];
    float3 albedo = ((inst.Flags & OPT_SAMPLE_TEXTURE) != 0)
        ? uTexture.Sample(uSampler, i.UV).rgb
        : inst.BaseColor.rgb;
    PSOut o;
    o.Albedo   = float4(albedo, 1.0);
    o.Normal   = float4(normalize(i.Normal), 0.0);
    o.WorldPos = float4(i.WorldPos, 1.0);
    float2 curUV  = i.CurClip.xy  / i.CurClip.w  * float2(0.5,-0.5) + 0.5;
    float2 prevUV = i.PrevClip.xy / i.PrevClip.w * float2(0.5,-0.5) + 0.5;
    o.Velocity = prevUV - curUV;
    return o;
}
)";

bool GBufferFillPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer || !renderer->GetMeshSystem() || !renderer->GetMaterialSystem())
        return false;

    // Compile shaders
    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, GBUF_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel,  GBUF_PS_HLSL, 0, "main_ps", "ps_6_1");
    if (!m_VS || !m_PS)
        return false;

    // Input layout: POSITION (RGB32F), NORMAL (RGB32F), TEXCOORD (RG32F) — identical to MeshRenderPass.
    nvrhi::VertexAttributeDesc attrs[3];
    attrs[0].setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(MeshVertex, px)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    attrs[1].setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(MeshVertex, nx)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    attrs[2].setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT)
        .setOffset(offsetof(MeshVertex, u)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    m_InputLayout = m_Device->createInputLayout(attrs, 3, m_VS);

    // Binding layout: b0 (PerFrame), t2 (Texture), s3 (Sampler), t5 (Instances).
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Sampler(3),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6)  // gPrevSkinned (prev-frame skinned verts)
    };
    nvrhi::VulkanBindingOffsets& offsets =
        nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0);

    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        layoutDesc.setBindingOffsets(offsets);
    }

    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    // Per-frame constant buffer (just VP).
    m_FrameCB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(GBufFrameCB), "GBufferFillPass FrameCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    // Instance buffer (structured buffer SRV).
    {
        nvrhi::BufferDesc bd;
        bd.debugName = "GBufferFillPass InstanceBuffer";
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

    // 1-element dummy prev-skinned structured buffer (MeshVertex layout). Bound at t6 on every
    // draw so the layout slot is always satisfied; the VS only reads it when IsSkinned!=0 (Task 2).
    {
        nvrhi::BufferDesc bd;
        bd.debugName = "GBufferFillPass DummyPrevSkinned";
        bd.byteSize = sizeof(MeshVertex);
        bd.structStride = sizeof(MeshVertex);
        bd.initialState = nvrhi::ResourceStates::ShaderResource;
        bd.isIndexBuffer = false;
        bd.isVertexBuffer = false;
        bd.isConstantBuffer = false;
        bd.canHaveUAVs = false;
        bd.keepInitialState = true;
        m_DummyPrevSkinned = m_Device->createBuffer(bd);
    }

    return true;
}

void GBufferFillPass::Render(nvrhi::ICommandList* commandList,
                             nvrhi::IFramebuffer* /*frameBuffer*/,
                             SimulationSnapshot& /*snapshot*/,
                             const ECS* world,
                             double /*deltaTime*/,
                             FrameAllocator* frameAllocator)
{
    nvrhi::IFramebuffer* gfb = m_Renderer->GetGBufferFramebuffer();
    if (!gfb || !world)
        return;

    if (!m_Pipeline)
    {
        const auto fbi = gfb->getFramebufferInfo();
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
        // Blend DISABLED on all three MRTs — G-buffer writes are opaque overwrites.
        nvrhi::BlendState::RenderTarget rt;
        rt.setBlendEnable(false)
          .setColorWriteMask(nvrhi::ColorMask::All);
        pso.renderState.blendState.setRenderTarget(0, rt);
        pso.renderState.blendState.setRenderTarget(1, rt);
        pso.renderState.blendState.setRenderTarget(2, rt);
        pso.renderState.blendState.setRenderTarget(3, rt); // velocity MRT3
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);

        pso.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Wireframe;
        m_WireframePipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    commandList->beginMarker("GBufferFillPass");

    // Clear the three color targets + shared depth.
    nvrhi::utils::ClearColorAttachment(commandList, gfb, 0, nvrhi::Color(0.f));
    nvrhi::utils::ClearColorAttachment(commandList, gfb, 1, nvrhi::Color(0.f)); // normal=0 -> sky mask later
    nvrhi::utils::ClearColorAttachment(commandList, gfb, 2, nvrhi::Color(0.f));
    nvrhi::utils::ClearColorAttachment(commandList, gfb, 3, nvrhi::Color(0.f)); // velocity = 0 (background/sky)
    commandList->clearDepthStencilTexture(gfb->getDesc().depthAttachment.texture, nvrhi::AllSubresources, true, 1.0f, false, 0);

    // Per-frame: only VP is needed (no lighting/shadow/fog in the geometry pass).
    GBufFrameCB cb{};
    const CameraView& cam = m_Renderer->GetActiveCamera();
    cb.VP = cam.Projection * cam.View;
    cb.PrevVP = m_Renderer->GetPrevViewProj();
    commandList->writeBuffer(m_FrameCB, &cb, sizeof(cb));

    // Skinned entities are drawn through the STATIC pipeline reading the per-frame compute-skinned
    // VB (posed mesh-local vertices written by SkinningComputePass, MeshVertex layout) at the
    // entity's vertex offset. No VS skinning here — there is one skinning implementation (the
    // compute pass). The static VS still applies inst.Model, giving the correct world result.
    SkinningComputePass* skinningPass = m_Renderer->GetSkinningPass();

    const Frustum cullFrustum = ExtractFrustum(cb.VP);
    const bool cullEnabled = GetCullingSettings().Enabled;
    MeshSystem* meshSystem = m_Renderer->GetMeshSystem();
    uint32_t culledCount = 0;

    // Group entities by (MeshId, MaterialId) into a flat arena array, then sort into
    // contiguous runs (one run == one draw batch). Same logic as MeshRenderPass.
    auto* entries = frameAllocator->AllocateArray<BatchEntry>(world->GetEntityCount());
    uint32_t entryCount = 0;
    if (entries)
    {
        world->Each<TransformComponent, MeshComponent>(
            [&](EntityId e, const TransformComponent& transform, const MeshComponent& meshComp)
        {
            if (!meshComp.Visible)
                return;

            if (cullEnabled && meshSystem && meshSystem->IsValidMeshId(meshComp.MeshId))
            {
                const auto bounds = meshSystem->GetMeshBounds(meshComp.MeshId);
                if (bounds.valid) // unloaded bounds -> never cull
                {
                    glm::vec3 wMin, wMax;
                    TransformAABB(ModelMatrix(transform), bounds.min, bounds.max, wMin, wMax);
                    if (!IsAABBVisible(cullFrustum, wMin, wMax))
                    {
                        ++culledCount;
                        return;
                    }
                }
            }

            const auto* materialComp = world->GetComponent<MaterialComponent>(e);
            uint64_t materialId = materialComp ? materialComp->MaterialId : MaterialSystem::MissingMaterial;

            entries[entryCount++] = BatchEntry{ meshComp.MeshId, materialId, e };
        });
    }
    else if (world->GetEntityCount() > 0)
    {
        char warn[128];
        snprintf(warn, sizeof(warn),
                 "GBufferFillPass: frame arena exhausted, dropped up to %zu entities (no meshes drawn)",
                 world->GetEntityCount());
        SM_WARN(warn);
    }

    BatchRun* runs = (entryCount > 0) ? frameAllocator->AllocateArray<BatchRun>(entryCount) : nullptr;
    if (entryCount > 0 && entries && !runs)
        SM_WARN("GBufferFillPass: frame arena exhausted, batch runs not allocated (no meshes drawn)");
    uint32_t runCount = (entries && runs) ? BuildBatchRuns(entries, entryCount, runs, entryCount) : 0;

    // Render each batch (run) with instancing into the G-buffer.
    nvrhi::GraphicsState state;
    state.pipeline = (GetDebugDrawSettings().Wireframe && m_WireframePipeline)
                         ? m_WireframePipeline : m_Pipeline;
    state.framebuffer = gfb;
    state.viewport.addViewportAndScissorRect(gfb->getFramebufferInfo().getViewport());

    uint32_t instancesDrawn = 0;

    for (uint32_t r = 0; r < runCount; ++r)
    {
        const BatchRun& run = runs[r];
        const BatchEntry& head = entries[run.begin];

        auto meshResources = m_Renderer->GetMeshSystem()->GetMeshResources(head.meshId);
        if (!meshResources.valid)
        {
            SM_WARN("GBufferFillPass: Invalid mesh ID %llu", (unsigned long long)head.meshId);
            meshResources = m_Renderer->GetMeshSystem()->GetMeshResources(MeshSystem::MissingMesh);
        }

        const bool runSkinned = meshResources.isSkinned;

        const uint32_t instanceCount = std::min(run.count, m_MaxInstances);

        const auto instanceMark = frameAllocator->GetMarker();
        auto* instances = frameAllocator->AllocateArray<MeshInstanceCPU>(instanceCount);
        if (!instances)
        {
            char warn[128];
            snprintf(warn, sizeof(warn),
                     "GBufferFillPass: frame arena exhausted, dropped %u instances (mesh %llu)",
                     instanceCount, (unsigned long long)head.meshId);
            SM_WARN(warn);
            continue;
        }
        uint32_t instanceOut = 0;

        // Parallel entity list aligned to `instances` (skinned path needs per-instance entity ids
        // to look up the compute-skinned vertex offset).
        auto* instanceEntities = frameAllocator->AllocateArray<EntityId>(instanceCount);
        if (!instanceEntities)
        {
            SM_WARN("GBufferFillPass: frame arena exhausted, dropped run (no entity table)");
            frameAllocator->RewindTo(instanceMark);
            continue;
        }

        for (uint32_t i = 0; i < instanceCount; ++i)
        {
            EntityId entity = entries[run.begin + i].entity;
            const auto* transform = world->GetComponent<TransformComponent>(entity);
            const auto* material = world->GetComponent<MaterialComponent>(entity);

            if (!transform)
                continue;

            glm::mat4 M = ModelMatrix(*transform);

            glm::mat3 M3(M);
            glm::mat3 N3 = glm::transpose(glm::inverse(M3));

            glm::vec4 baseColor = material ? material->BaseColor : glm::vec4(1.0f);
            uint32_t flags = (material && (material->Flags & 1u)) ? 1u : 0u;

            MeshInstanceCPU inst{};
            inst.Model = M;
            inst.NormalMatrix = glm::mat4(N3);
            inst.PrevModel = m_PrevModel.count((uint32_t)entity) ? m_PrevModel[(uint32_t)entity] : M; // new entity -> zero velocity
            inst.BaseColor = baseColor;
            inst.Flags = flags;
            inst.IsSkinned = 0u;            // Task 2 sets 1 for skinned draws
            inst.PrevSkinnedOffset = 0u;

            instanceEntities[instanceOut] = entity;
            instances[instanceOut++] = inst;
        }

        if (instanceOut == 0)
        {
            frameAllocator->RewindTo(instanceMark);
            continue;
        }

        instancesDrawn += instanceOut;

        // Resolve material resources once per run (single-material common case); per-submesh
        // material is resolved inside the submesh loop below.
        auto resolveMaterial = [&](uint64_t matIndex) {
            auto mr = m_Renderer->GetMaterialSystem()->GetMaterialResources(matIndex);
            if (!mr.valid)
            {
                SM_WARN("GBufferFillPass: Invalid material ID %llu", (unsigned long long)matIndex);
                mr = m_Renderer->GetMaterialSystem()->GetMaterialResources(MaterialSystem::MissingMaterial);
            }
            return mr;
        };

        const nvrhi::GraphicsPipelineHandle solidPipeline =
            (GetDebugDrawSettings().Wireframe && m_WireframePipeline) ? m_WireframePipeline : m_Pipeline;

        if (!runSkinned)
        {
            // Static path: one instanced draw per submesh (unchanged).
            commandList->writeBuffer(m_InstanceBuffer, instances, instanceOut * sizeof(MeshInstanceCPU));

            if (meshResources.subMeshes.size() > 0)
            {
                for (const auto& subMesh : meshResources.subMeshes)
                {
                    auto materialResources = resolveMaterial(subMesh.MaterialIndex);

                    nvrhi::BindingSetDesc bindingDesc;
                    bindingDesc.bindings = {
                        nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
                        nvrhi::BindingSetItem::Texture_SRV(2, materialResources.texture),
                        nvrhi::BindingSetItem::Sampler(3, materialResources.sampler),
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_InstanceBuffer),
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_DummyPrevSkinned)
                    };
                    nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

                    state.pipeline = solidPipeline;
                    state.bindings = { bindingSet };
                    state.vertexBuffers = { nvrhi::VertexBufferBinding(meshResources.vertexBuffer, 0, 0) };
                    state.indexBuffer = nvrhi::IndexBufferBinding(meshResources.indexBuffer, nvrhi::Format::R32_UINT, 0);
                    commandList->setGraphicsState(state);

                    nvrhi::DrawArguments args{};
                    args.vertexCount = subMesh.IndexCount;
                    args.instanceCount = instanceOut;
                    args.startIndexLocation = subMesh.IndexStart;
                    args.startVertexLocation = 0;
                    commandList->drawIndexed(args);
                }
            }
            else
            {
                auto materialResources = resolveMaterial(head.materialId);

                nvrhi::BindingSetDesc bindingDesc;
                bindingDesc.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
                    nvrhi::BindingSetItem::Texture_SRV(2, materialResources.texture),
                    nvrhi::BindingSetItem::Sampler(3, materialResources.sampler),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_InstanceBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_DummyPrevSkinned)
                };
                nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

                state.pipeline = solidPipeline;
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
        }
        else
        {
            // Skinned path: STATIC pipeline + the per-frame compute-skinned VB (MeshVertex layout,
            // posed mesh-local). SV_InstanceID is 0-based per draw on D3D, so we draw ONE entity at a
            // time with instanceCount=1 and write that entity's InstanceData to element 0 of the
            // instance buffer right before its draw -> the static VS's gInstances[0] resolves to the
            // correct entity (provably correct: each draw reads element 0, just written for it).
            // startVertexLocation shifts indices into the entity's range within the shared skinned VB.
            // If an entity has no compute-skinned range this frame, fall back to its static VB at
            // bind pose (baseVertex 0).
            nvrhi::IBuffer* skinnedVB = skinningPass ? skinningPass->GetSkinnedVertexBuffer() : nullptr;

            for (uint32_t i = 0; i < instanceOut; ++i)
            {
                EntityId entity = instanceEntities[i];
                const int64_t skinnedOff = skinningPass ? skinningPass->GetSkinnedVertexOffset(entity) : -1;
                const bool useSkinned = (skinnedOff >= 0) && skinnedVB;
                nvrhi::IBuffer* vb = useSkinned ? skinnedVB : meshResources.vertexBuffer.Get();
                const uint32_t baseVertex = useSkinned ? (uint32_t)skinnedOff : 0u;

                // This entity's InstanceData at element 0 -> read by gInstances[SV_InstanceID==0].
                commandList->writeBuffer(m_InstanceBuffer, &instances[i], sizeof(MeshInstanceCPU));

                if (meshResources.subMeshes.size() > 0)
                {
                    for (const auto& subMesh : meshResources.subMeshes)
                    {
                        auto materialResources = resolveMaterial(subMesh.MaterialIndex);

                        nvrhi::BindingSetDesc bindingDesc;
                        bindingDesc.bindings = {
                            nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
                            nvrhi::BindingSetItem::Texture_SRV(2, materialResources.texture),
                            nvrhi::BindingSetItem::Sampler(3, materialResources.sampler),
                            nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_InstanceBuffer),
                            nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_DummyPrevSkinned)
                        };
                        nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

                        state.pipeline = solidPipeline;
                        state.bindings = { bindingSet };
                        state.vertexBuffers = { nvrhi::VertexBufferBinding(vb, 0, 0) };
                        state.indexBuffer = nvrhi::IndexBufferBinding(meshResources.indexBuffer, nvrhi::Format::R32_UINT, 0);
                        commandList->setGraphicsState(state);

                        nvrhi::DrawArguments args{};
                        args.vertexCount = subMesh.IndexCount;
                        args.instanceCount = 1;
                        args.startIndexLocation = subMesh.IndexStart;
                        args.startVertexLocation = baseVertex;
                        commandList->drawIndexed(args);
                    }
                }
                else
                {
                    auto materialResources = resolveMaterial(head.materialId);

                    nvrhi::BindingSetDesc bindingDesc;
                    bindingDesc.bindings = {
                        nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
                        nvrhi::BindingSetItem::Texture_SRV(2, materialResources.texture),
                        nvrhi::BindingSetItem::Sampler(3, materialResources.sampler),
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_InstanceBuffer),
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_DummyPrevSkinned)
                    };
                    nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

                    state.pipeline = solidPipeline;
                    state.bindings = { bindingSet };
                    state.vertexBuffers = { nvrhi::VertexBufferBinding(vb, 0, 0) };
                    state.indexBuffer = nvrhi::IndexBufferBinding(meshResources.indexBuffer, nvrhi::Format::R32_UINT, 0);
                    commandList->setGraphicsState(state);

                    nvrhi::DrawArguments args{};
                    args.vertexCount = meshResources.indexCount;
                    args.instanceCount = 1;
                    args.startIndexLocation = 0;
                    args.startVertexLocation = baseVertex;
                    commandList->drawIndexed(args);
                }
            }
        }

        frameAllocator->RewindTo(instanceMark);
    }

    // Snapshot this frame's models as next frame's "prev"; drop entities no longer present.
    {
        std::unordered_map<uint32_t, glm::mat4> next;
        world->Each<TransformComponent, MeshComponent>(
            [&](EntityId e, const TransformComponent& t, const MeshComponent& m)
        {
            if (m.Visible) next[(uint32_t)e] = ModelMatrix(t);
        });
        m_PrevModel.swap(next);
    }
    m_Renderer->SetPrevViewProj(cb.VP);

    RenderStats& rs = GetRenderStats();
    rs.MeshEntitiesDrawn  = entryCount;
    rs.MeshEntitiesCulled = culledCount;
    rs.MeshEntitiesTotal  = entryCount + culledCount;
    rs.InstancesDrawn     = instancesDrawn;
    rs.BatchesDrawn       = runCount;

    commandList->endMarker();
}

void GBufferFillPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_WireframePipeline = nullptr;
    m_InputLayout = nullptr;
    m_BindingLayout = nullptr;
    m_FrameCB = nullptr;
    m_InstanceBuffer = nullptr;
    m_DummyPrevSkinned = nullptr;
    m_PrevModel.clear();
    m_VS = nullptr;
    m_PS = nullptr;
    m_Device = nullptr;
    m_Renderer = nullptr;
}

void GBufferFillPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    // Rebuild the pipeline against the resized G-buffer framebuffer on next render.
    m_Pipeline = nullptr;
    m_WireframePipeline = nullptr;
}
