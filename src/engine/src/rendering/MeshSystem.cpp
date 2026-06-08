#include "MeshSystem.h"
#include "lib.h"
#include "AssetKey.h"

void MeshSystem::Initialize(nvrhi::IDevice* device)
{
    m_Device = device;
    m_Meshes.clear();
    m_SlotByHandle.clear();

    {
        // Add a default mesh to avoid invalid handle issues
        std::vector<MeshVertex> vertices =
        {
            // +Z (Front)
            {-1.0f,-1.0f, 1.0f,  0.0f, 0.0f, 1.0f, 0.0f,0.0f},
            { 1.0f,-1.0f, 1.0, 0.0f, 0.0f, 1.f, 1.f,0.0f},
            { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,1.0f},
            {-1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f,1.0f},
            // -Z (Back)
            { 1.0f,-1.0f,-1.0f, 0.0f, 0.0f,-1.0f, 0.0f,0.0f},
            {-1.0f,-1.0f,-1.0f, 0.0f, 0.0f,-1.0f, 1.0f,0.0f},
            {-1.0f, 1.0f,-1.0f, 0.0f, 0.0f,-1.0f, 1.0f,1.0f},
            { 1.0f, 1.0f,-1.0f, 0.0f, 0.0f,-1.0f, 0.0f,1.0f},
            // +X (Right)
            { 1.0f,-1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,0.0f},
            { 1.0f,-1.0f,-1.0f, 1.0f, 0.0f, 0.0f, 1.0f,0.0f},
            { 1.0f, 1.0f,-1.0f, 1.0f, 0.0f, 0.0f, 1.0f,1.0f},
            { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,1.0f},
            // -X (Left)
            {-1.0f,-1.0f,-1.0f, 1.0f, 0.0f, 0.0f, 0.0f,0.0f},
            {-1.0f,-1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,0.0f},
            {-1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,1.0f},
            {-1.0f, 1.0f,-1.0f, 1.0f, 0.0f, 0.0f, 0.0f,1.0f},
            // +Y (Top)
            {-1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f,0.0f},
            { 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f,0.0f},
            { 1.0f, 1.0f,-1.0f, 0.0f, 1.0f, 0.0f, 1.0f,1.0f},
            {-1.0f, 1.0f,-1.0f, 0.0f, 1.0f, 0.0f, 0.0f,1.0f},
            // -Y (Bottom)
            {-1.0f,-1.0f,-1.0f, 0.0f,-1.0f, 0.0f, 0.0f,0.0f},
            { 1.0f,-1.0f,-1.0f, 0.0f,-1.0f, 0.0f, 1.0f,0.0f},
            { 1.0f,-1.0f, 1.0f, 0.0f,-1.0f, 0.0f, 1.0f,1.0f},
            {-1.0f,-1.0f, 1.0f, 0.0f,-1.0f, 0.0f, 0.0f,1.0f},
        };

        std::vector<uint32_t> indices =
        {
           // front
           0, 1, 2, 2, 3, 0,
           // back
           4, 5, 6, 6, 7, 4,
           // right
           8, 9,10,10,11, 8,
           // left
           12,13,14,14,15,12,
           // top
           16,17,18,18,19,16,
           // bottom
           20,21,22,22,23,20
        };;

        AddMesh("", vertices.data(), static_cast<uint32_t>(vertices.size()),
                 indices.data(), static_cast<uint32_t>(indices.size()), nullptr, 0);
    }
}

MeshHandle MeshSystem::AddMesh(std::string key,
                                const MeshVertex* vertices, uint32_t vertexCount,
                                const uint32_t* indices, uint32_t indexCount,
                                SubMesh* subMeshes, uint32_t subMeshCount,
                                const SkinnedVertex* boneData)
{
    if (!m_Device || !vertices || vertexCount == 0 || !indices || indexCount == 0)
    {
        SM_ERROR("MeshSystem::AddMesh: Invalid parameters");
        return MeshHandle{ UINT64_MAX };
    }

    const uint64_t handle = AssetKeyHash(key);   // key=="" -> kMissingAssetHandle (0)
    if (auto it = m_SlotByHandle.find(handle); it != m_SlotByHandle.end()) {
        return MeshHandle{ handle }; // already loaded (de-dup): return existing stable handle
    }

    MeshEntry entry{};
    entry.key = std::move(key);
    entry.vertexCount = vertexCount;
    entry.indexCount = indexCount;
    if (subMeshes && subMeshCount > 0)
    {
        // A submesh is just a pair of (indexStart, indexCount) into the index buffer
        entry.subMeshes.assign(subMeshes, subMeshes + subMeshCount);
    }

    // Compute bounding box from vertices
    entry.boundsMin = glm::vec3(FLT_MAX);
    entry.boundsMax = glm::vec3(-FLT_MAX);
    for (uint32_t i = 0; i < vertexCount; ++i)
    {
        const glm::vec3 pos(vertices[i].px, vertices[i].py, vertices[i].pz);
        entry.boundsMin = glm::min(entry.boundsMin, pos);
        entry.boundsMax = glm::max(entry.boundsMax, pos);
    }

    // Retain CPU-side copies for hot-swap replay.
    entry.cpuVertices.assign(vertices, vertices + vertexCount);
    entry.cpuIndices.assign(indices, indices + indexCount);

    if (boneData) {
        entry.isSkinned = true;
        entry.cpuSkinning.assign(boneData, boneData + vertexCount);
    }

    // Create command list for upload
    auto cl = m_Device->createCommandList(nvrhi::CommandListParameters().setQueueType(nvrhi::CommandQueue::Graphics));
    cl->open();

    // Create and upload vertex buffer
    nvrhi::BufferDesc vbDesc;
    vbDesc.debugName = "MeshSystem VB " + std::to_string(m_Meshes.size());
    vbDesc.byteSize = sizeof(MeshVertex) * vertexCount;
    vbDesc.isVertexBuffer = true;
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    entry.vertexBuffer = m_Device->createBuffer(vbDesc);

    if (!entry.vertexBuffer)
    {
        SM_ERROR("MeshSystem::AddMesh: Failed to create vertex buffer");
        cl->close();
        return MeshHandle{ UINT64_MAX };
    }

    cl->beginTrackingBufferState(entry.vertexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(entry.vertexBuffer, vertices, vbDesc.byteSize);
    cl->setPermanentBufferState(entry.vertexBuffer, nvrhi::ResourceStates::VertexBuffer);

    // Create and upload index buffer
    nvrhi::BufferDesc ibDesc;
    ibDesc.debugName = "MeshSystem IB " + std::to_string(m_Meshes.size());
    ibDesc.byteSize = sizeof(uint32_t) * indexCount;
    ibDesc.isIndexBuffer = true;
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    entry.indexBuffer = m_Device->createBuffer(ibDesc);

    if (!entry.indexBuffer)
    {
        SM_ERROR("MeshSystem::AddMesh: Failed to create index buffer");
        cl->close();
        return MeshHandle{ UINT64_MAX };
    }

    cl->beginTrackingBufferState(entry.indexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(entry.indexBuffer, indices, ibDesc.byteSize);
    cl->setPermanentBufferState(entry.indexBuffer, nvrhi::ResourceStates::IndexBuffer);

    if (entry.isSkinned) {
        nvrhi::BufferDesc bbDesc;
        bbDesc.debugName = "MeshSystem BoneVB " + std::to_string(m_Meshes.size());
        bbDesc.byteSize = sizeof(SkinnedVertex) * vertexCount;
        bbDesc.isVertexBuffer = true;
        bbDesc.initialState = nvrhi::ResourceStates::CopyDest;
        entry.boneBuffer = m_Device->createBuffer(bbDesc);
        if (!entry.boneBuffer) {
            SM_ERROR("MeshSystem::AddMesh: Failed to create bone buffer");
            cl->close();
            return MeshHandle{ UINT64_MAX };
        }
        cl->beginTrackingBufferState(entry.boneBuffer, nvrhi::ResourceStates::CopyDest);
        cl->writeBuffer(entry.boneBuffer, entry.cpuSkinning.data(), bbDesc.byteSize);
        cl->setPermanentBufferState(entry.boneBuffer, nvrhi::ResourceStates::VertexBuffer);
    }

    // Execute upload commands
    cl->close();
    m_Device->executeCommandList(cl);

    // Store mesh entry and return handle
    const uint32_t slot = static_cast<uint32_t>(m_Meshes.size());
    m_Meshes.push_back(std::move(entry));
    m_SlotByHandle[handle] = slot;
    SM_TRACE("MeshSystem::AddMesh: '%s' -> handle %llu (slot %u, %u verts %u idx)",
             m_Meshes[slot].key.c_str(), (unsigned long long)handle, slot, vertexCount, indexCount);
    return MeshHandle{ handle };
}

void MeshSystem::AssociateMeshMaterial(MeshHandle meshHandle, MaterialHandle materialHandle, uint32_t materialIndex) {

    const int32_t slot = SlotForHandle(meshHandle.Index);
    if (slot < 0)
    {
        SM_WARN("MeshSystem::AssociateMeshMaterial: Invalid mesh ID %llu", (unsigned long long)meshHandle.Index);
        return;
    }

    MeshEntry& entry = m_Meshes[slot];

    for (auto &subMesh: entry.subMeshes) {
        if (subMesh.MaterialIndex == materialIndex) {
            subMesh.MaterialIndex = materialHandle.Index;
            SM_TRACE("MeshSystem::AssociateMeshMaterial: Associated material %llu with mesh %llu sub-mesh %u",
                     (unsigned long long)materialHandle.Index, (unsigned long long)meshHandle.Index, materialIndex);
            return;
        }
    }

}

int32_t MeshSystem::SlotForHandle(uint64_t handle) const {
    auto it = m_SlotByHandle.find(handle);
    return it == m_SlotByHandle.end() ? -1 : static_cast<int32_t>(it->second);
}

MeshSystem::MeshResources MeshSystem::GetMeshResources(uint64_t meshId) const
{
    MeshResources resources{};
    const int32_t slot = SlotForHandle(meshId);
    if (slot < 0) { SM_WARN("MeshSystem::GetMeshResources: unknown mesh handle %llu", (unsigned long long)meshId); return resources; }
    const MeshEntry& entry = m_Meshes[slot];
    resources.vertexBuffer = entry.vertexBuffer;
    resources.indexBuffer  = entry.indexBuffer;
    resources.vertexCount  = entry.vertexCount;
    resources.indexCount   = entry.indexCount;
    resources.subMeshes    = std::span<const SubMesh>(entry.subMeshes);
    resources.valid        = true;
    resources.isSkinned  = entry.isSkinned;
    resources.boneBuffer = entry.boneBuffer;
    return resources;
}

MeshSystem::MeshCpuData MeshSystem::GetMeshCpuData(uint64_t meshId) const
{
    MeshCpuData out{};
    const int32_t slot = SlotForHandle(meshId);
    if (slot < 0) return out;
    const auto& e = m_Meshes[slot];
    if (e.cpuVertices.empty() || e.cpuIndices.empty()) return out;
    out.vertices = std::span<const MeshVertex>(e.cpuVertices.data(), e.cpuVertices.size());
    out.indices  = std::span<const uint32_t>(e.cpuIndices.data(), e.cpuIndices.size());
    out.valid    = true;
    return out;
}

uint32_t MeshSystem::GetMeshCount() const
{
    return static_cast<uint32_t>(m_Meshes.size());
}

bool MeshSystem::IsValidMeshId(const uint64_t meshId) const { return SlotForHandle(meshId) >= 0; }

std::string MeshSystem::KeyForHandle(uint64_t handle) const {
    const int32_t slot = SlotForHandle(handle);
    return slot < 0 ? std::string() : m_Meshes[slot].key;
}

std::vector<std::pair<uint64_t,std::string>> MeshSystem::GetAssetList() const {
    std::vector<std::pair<uint64_t,std::string>> out;
    out.reserve(m_SlotByHandle.size());
    for (const auto& [h, slot] : m_SlotByHandle) out.emplace_back(h, m_Meshes[slot].key);
    return out;
}

MeshSystem::BoundingBox MeshSystem::GetMeshBounds(const uint64_t meshId) const
{
    BoundingBox bounds{};

    const int32_t slot = SlotForHandle(meshId);
    if (slot < 0)
    {
        SM_WARN("MeshSystem::GetMeshBounds: Invalid mesh ID %llu", (unsigned long long)meshId);
        return bounds;
    }

    const MeshEntry& entry = m_Meshes[slot];
    bounds.min = entry.boundsMin;
    bounds.max = entry.boundsMax;
    bounds.valid = true;

    return bounds;
}

void MeshSystem::Shutdown()
{
    for (auto& entry : m_Meshes)
    {
        entry.vertexBuffer = nullptr;
        entry.indexBuffer = nullptr;
        entry.subMeshes.clear();
    }
    m_Meshes.clear();
    m_SlotByHandle.clear();
    m_Device = nullptr;
}

void MeshSystem::DestroyGpuResources()
{
    // Release GPU buffers but keep m_Meshes entries (and their CPU caches).
    for (auto& entry : m_Meshes)
    {
        entry.vertexBuffer = nullptr;
        entry.indexBuffer = nullptr;
    }
    // Keep m_Device as-is; caller updates it before RecreateGpuResources().
}

bool MeshSystem::RecreateGpuResources()
{
    if (!m_Device)
    {
        SM_ERROR("MeshSystem::RecreateGpuResources: no device");
        return false;
    }

    bool allOk = true;
    for (size_t i = 0; i < m_Meshes.size(); ++i)
    {
        MeshEntry& entry = m_Meshes[i];
        if (entry.cpuVertices.empty() || entry.cpuIndices.empty())
        {
            SM_WARN("MeshSystem::RecreateGpuResources: mesh %zu has no CPU cache; skipping", i);
            allOk = false;
            continue;
        }

        auto cl = m_Device->createCommandList(
            nvrhi::CommandListParameters().setQueueType(nvrhi::CommandQueue::Graphics));
        cl->open();

        nvrhi::BufferDesc vbDesc;
        vbDesc.debugName = "MeshSystem VB " + std::to_string(i);
        vbDesc.byteSize = sizeof(MeshVertex) * entry.cpuVertices.size();
        vbDesc.isVertexBuffer = true;
        vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
        entry.vertexBuffer = m_Device->createBuffer(vbDesc);
        if (!entry.vertexBuffer)
        {
            SM_ERROR("MeshSystem::RecreateGpuResources: mesh %zu vertex buffer failed", i);
            cl->close();
            allOk = false;
            continue;
        }
        cl->beginTrackingBufferState(entry.vertexBuffer, nvrhi::ResourceStates::CopyDest);
        cl->writeBuffer(entry.vertexBuffer, entry.cpuVertices.data(), vbDesc.byteSize);
        cl->setPermanentBufferState(entry.vertexBuffer, nvrhi::ResourceStates::VertexBuffer);

        nvrhi::BufferDesc ibDesc;
        ibDesc.debugName = "MeshSystem IB " + std::to_string(i);
        ibDesc.byteSize = sizeof(uint32_t) * entry.cpuIndices.size();
        ibDesc.isIndexBuffer = true;
        ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
        entry.indexBuffer = m_Device->createBuffer(ibDesc);
        if (!entry.indexBuffer)
        {
            SM_ERROR("MeshSystem::RecreateGpuResources: mesh %zu index buffer failed", i);
            cl->close();
            entry.vertexBuffer = nullptr;
            allOk = false;
            continue;
        }
        cl->beginTrackingBufferState(entry.indexBuffer, nvrhi::ResourceStates::CopyDest);
        cl->writeBuffer(entry.indexBuffer, entry.cpuIndices.data(), ibDesc.byteSize);
        cl->setPermanentBufferState(entry.indexBuffer, nvrhi::ResourceStates::IndexBuffer);

        if (entry.isSkinned && !entry.cpuSkinning.empty()) {
            nvrhi::BufferDesc bbDesc;
            bbDesc.debugName = "MeshSystem BoneVB " + std::to_string(i);
            bbDesc.byteSize = sizeof(SkinnedVertex) * entry.cpuSkinning.size();
            bbDesc.isVertexBuffer = true;
            bbDesc.initialState = nvrhi::ResourceStates::CopyDest;
            entry.boneBuffer = m_Device->createBuffer(bbDesc);
            if (entry.boneBuffer) {
                cl->beginTrackingBufferState(entry.boneBuffer, nvrhi::ResourceStates::CopyDest);
                cl->writeBuffer(entry.boneBuffer, entry.cpuSkinning.data(), bbDesc.byteSize);
                cl->setPermanentBufferState(entry.boneBuffer, nvrhi::ResourceStates::VertexBuffer);
            }
        }

        cl->close();
        m_Device->executeCommandList(cl);
    }

    SM_TRACE("MeshSystem::RecreateGpuResources: rebuilt %zu meshes", m_Meshes.size());
    return allOk;
}
