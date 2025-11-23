#include "MeshSystem.h"
#include "lib.h"

void MeshSystem::Initialize(nvrhi::IDevice* device)
{
    m_Device = device;
    m_Meshes.clear();
}

MeshHandle MeshSystem::AddMesh(const MeshVertex* vertices, uint32_t vertexCount,
                                const uint32_t* indices, uint32_t indexCount,
                                SubMesh* subMeshes, uint32_t subMeshCount)
{
    if (!m_Device || !vertices || vertexCount == 0 || !indices || indexCount == 0)
    {
        SM_ERROR("MeshSystem::AddMesh: Invalid parameters");
        return MeshHandle{ UINT32_MAX };
    }

    MeshEntry entry{};
    entry.vertexCount = vertexCount;
    entry.indexCount = indexCount;
    if (subMeshes && subMeshCount > 0)
    {
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
        return MeshHandle{ UINT32_MAX };
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
        return MeshHandle{ UINT32_MAX };
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

MeshSystem::MeshResources MeshSystem::GetMeshResources(uint32_t meshId) const
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
    resources.valid = true;

    return resources;
}

uint32_t MeshSystem::GetMeshCount() const
{
    return static_cast<uint32_t>(m_Meshes.size());
}

bool MeshSystem::IsValidMeshId(uint32_t meshId) const
{
    return meshId < m_Meshes.size();
}

MeshSystem::BoundingBox MeshSystem::GetMeshBounds(uint32_t meshId) const
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
