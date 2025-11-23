#pragma once

#include <nvrhi/nvrhi.h>
#include <vector>
#include <cstdint>

#include "ApplicationContext.h"

// MeshSystem manages GPU mesh resources (vertex buffers, index buffers)
// and provides lookup by MeshHandle/MeshId
class MeshSystem
{
public:
    MeshSystem() = default;
    ~MeshSystem() = default;

    // Initialize the system with device reference
    void Initialize(nvrhi::IDevice* device);

    // Upload mesh data and return a handle
    // Returns MeshHandle with Index = UINT32_MAX on failure
    MeshHandle AddMesh(const MeshVertex* vertices, uint32_t vertexCount,
                       const uint32_t* indices, uint32_t indexCount,
                       SubMesh* subMeshes = nullptr, uint32_t subMeshCount = 0);

    // Query GPU resources by mesh ID
    struct MeshResources {
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        bool valid = false;
    };

    MeshResources GetMeshResources(uint32_t meshId) const;

    // Query mesh count and validity (for UI/editor purposes)
    uint32_t GetMeshCount() const;
    bool IsValidMeshId(uint32_t meshId) const;

    // Get bounding box for mesh visualization
    struct BoundingBox {
        glm::vec3 min{0.0f};
        glm::vec3 max{0.0f};
        bool valid = false;
    };
    BoundingBox GetMeshBounds(uint32_t meshId) const;

    // Cleanup all resources
    void Shutdown();

private:
    struct MeshEntry {
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        std::vector<SubMesh> subMeshes;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
    };

    nvrhi::IDevice* m_Device = nullptr;
    std::vector<MeshEntry> m_Meshes;
};
