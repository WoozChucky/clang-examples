#pragma once

#include <nvrhi/nvrhi.h>
#include <vector>
#include <cstdint>
#include <span>

#include "Engine.h"
#include "ApplicationContext.h"

// MeshSystem manages GPU mesh resources (vertex buffers, index buffers)
// and provides lookup by MeshHandle/MeshId
class ENGINE_API MeshSystem
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

    void AssociateMeshMaterial(MeshHandle meshHandle, MaterialHandle materialHandle, uint32_t materialIndex);

    // Query GPU resources by mesh ID
    struct MeshResources {
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        std::span<const SubMesh> subMeshes; // non-owning view into the MeshEntry's vector
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        bool valid = false;
    };

    static constexpr uint32_t MissingMesh = { 0 }; // Reserved default mesh handle

    MeshResources GetMeshResources(uint32_t meshId) const;

    // CPU-side vertex/index view for nav build, mesh decimation, picking, etc.
    // Span is valid until the MeshSystem is mutated (AddMesh / Shutdown / RecreateGpuResources).
    struct MeshCpuData {
        std::span<const MeshVertex> vertices;
        std::span<const uint32_t>   indices;
        bool                        valid = false;
    };
    MeshCpuData GetMeshCpuData(uint32_t meshId) const;

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

    void SetDevice(nvrhi::IDevice* device) { m_Device = device; }

    // Hot-swap support: release GPU buffers but keep entries + CPU caches.
    void DestroyGpuResources();
    // Hot-swap support: rebuild GPU buffers from CPU caches against m_Device.
    // m_Device must already point to the new device. Preserves slot indices.
    // Returns false if any mesh failed to rebuild (that slot is left null).
    bool RecreateGpuResources();

private:
    struct MeshEntry {
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        std::vector<SubMesh> subMeshes;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};

        // CPU-side copies retained for backend hot-swap replay.
        std::vector<MeshVertex> cpuVertices;
        std::vector<uint32_t>   cpuIndices;
    };

    nvrhi::IDevice* m_Device = nullptr;
    std::vector<MeshEntry> m_Meshes;
};
