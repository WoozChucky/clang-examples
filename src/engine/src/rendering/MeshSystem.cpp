#include "MeshSystem.h"
#include "lib.h"

void MeshSystem::Initialize(nvrhi::IDevice* device)
{
    m_Device = device;
    m_Meshes.clear();

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

        AddMesh(vertices.data(), static_cast<uint32_t>(vertices.size()),
                 indices.data(), static_cast<uint32_t>(indices.size()),
                 nullptr, 0);
    }
}

MeshHandle MeshSystem::AddMesh(const MeshVertex* vertices, uint32_t vertexCount,
                                const uint32_t* indices, uint32_t indexCount,
                                SubMesh* subMeshes, uint32_t subMeshCount)
{
    if (!m_Device || !vertices || vertexCount == 0 || !indices || indexCount == 0)
    {
        SM_ERROR("MeshSystem::AddMesh: Invalid parameters");
        return MeshHandle{ UINT64_MAX };
    }

    MeshEntry entry{};
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

    // Execute upload commands
    cl->close();
    m_Device->executeCommandList(cl);

    // Store mesh entry and return handle
    const uint32_t meshId = static_cast<uint32_t>(m_Meshes.size());
    m_Meshes.push_back(entry);

    SM_TRACE("MeshSystem::AddMesh: Created mesh %u with %u vertices, %u indices",
             meshId, vertexCount, indexCount);

    return MeshHandle{ meshId };
}

void MeshSystem::AssociateMeshMaterial(MeshHandle meshHandle, MaterialHandle materialHandle, uint32_t materialIndex) {

    if (meshHandle.Index >= m_Meshes.size())
    {
        SM_WARN("MeshSystem::AssociateMeshMaterial: Invalid mesh ID %u", meshHandle.Index);
        return;
    }

    MeshEntry& entry = m_Meshes[meshHandle.Index];

    for (auto &subMesh: entry.subMeshes) {
        if (subMesh.MaterialIndex == materialIndex) {
            subMesh.MaterialIndex = materialHandle.Index;
            SM_TRACE("MeshSystem::AssociateMeshMaterial: Associated material %u with mesh %u sub-mesh %u",
                     materialHandle.Index, meshHandle.Index, materialIndex);
            return;
        }
    }

}

MeshSystem::MeshResources MeshSystem::GetMeshResources(uint64_t meshId) const
{
    MeshResources resources{};

    if (meshId >= m_Meshes.size())
    {
        SM_WARN("MeshSystem::GetMeshResources: Invalid mesh ID %u", meshId);
        return resources;
    }

    const MeshEntry& entry = m_Meshes[meshId];
    resources.vertexBuffer = entry.vertexBuffer;
    resources.indexBuffer = entry.indexBuffer;
    resources.vertexCount = entry.vertexCount;
    resources.indexCount = entry.indexCount;
    // Non-owning view into the entry's vector. Valid only while m_Meshes is not
    // mutated; mesh adds are drained before render passes run, so the span is
    // valid for the duration of a frame's Render calls.
    resources.subMeshes = std::span<const SubMesh>(entry.subMeshes);
    resources.valid = true;

    return resources;
}

MeshSystem::MeshCpuData MeshSystem::GetMeshCpuData(uint64_t meshId) const
{
    MeshCpuData out{};
    if (meshId >= m_Meshes.size()) return out;
    const auto& e = m_Meshes[meshId];
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

bool MeshSystem::IsValidMeshId(const uint64_t meshId) const
{
    return meshId < m_Meshes.size();
}

MeshSystem::BoundingBox MeshSystem::GetMeshBounds(const uint64_t meshId) const
{
    BoundingBox bounds{};

    if (meshId >= m_Meshes.size())
    {
        SM_WARN("MeshSystem::GetMeshBounds: Invalid mesh ID %u", meshId);
        return bounds;
    }

    const MeshEntry& entry = m_Meshes[meshId];
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

        cl->close();
        m_Device->executeCommandList(cl);
    }

    SM_TRACE("MeshSystem::RecreateGpuResources: rebuilt %zu meshes", m_Meshes.size());
    return allOk;
}
