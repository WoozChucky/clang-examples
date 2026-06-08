#pragma once

#include <nvrhi/nvrhi.h>
#include <vector>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <string>
#include <utility>

#include "Engine.h"
#include "ApplicationContext.h"
#include "Skinning.h"

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
    // Returns MeshHandle with Index = UINT64_MAX on failure
    MeshHandle AddMesh(std::string key,
                       const MeshVertex* vertices, uint32_t vertexCount,
                       const uint32_t* indices, uint32_t indexCount,
                       SubMesh* subMeshes = nullptr, uint32_t subMeshCount = 0,
                       const SkinnedVertex* boneData = nullptr);

    void AssociateMeshMaterial(MeshHandle meshHandle, MaterialHandle materialHandle, uint32_t materialIndex);

    // Stable logical key (virtual path) <-> runtime handle. KeyForHandle is the reverse lookup for
    // serialization/editor display; forward (key->handle) is just AssetKeyHash.
    std::string KeyForHandle(uint64_t handle) const;
    std::vector<std::pair<uint64_t, std::string>> GetAssetList() const; // (handle, key) per loaded mesh

    // Query GPU resources by mesh ID
    struct MeshResources {
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        std::span<const SubMesh> subMeshes; // non-owning view into the MeshEntry's vector
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        bool valid = false;
        bool isSkinned = false;
        nvrhi::IBuffer* boneBuffer = nullptr;
    };

    static constexpr uint64_t MissingMesh = { 0 }; // Reserved default mesh handle

    MeshResources GetMeshResources(uint64_t meshId) const;

    // CPU-side vertex/index view for nav build, mesh decimation, picking, etc.
    // Span is valid until the MeshSystem is mutated (AddMesh / Shutdown / RecreateGpuResources).
    struct MeshCpuData {
        std::span<const MeshVertex> vertices;
        std::span<const uint32_t>   indices;
        bool                        valid = false;
    };
    MeshCpuData GetMeshCpuData(uint64_t meshId) const;

    // Query mesh count and validity (for UI/editor purposes)
    uint32_t GetMeshCount() const;
    bool IsValidMeshId(uint64_t meshId) const;

    // Get bounding box for mesh visualization
    struct BoundingBox {
        glm::vec3 min{0.0f};
        glm::vec3 max{0.0f};
        bool valid = false;
    };
    BoundingBox GetMeshBounds(uint64_t meshId) const;

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
        std::string key;
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
        bool isSkinned = false;
        nvrhi::BufferHandle boneBuffer;            // {uvec4 idx, vec4 weight} per vertex; null if !isSkinned
        std::vector<SkinnedVertex> cpuSkinning;    // hot-swap replay
    };

    nvrhi::IDevice* m_Device = nullptr;
    std::vector<MeshEntry> m_Meshes;
    std::unordered_map<uint64_t, uint32_t> m_SlotByHandle; // stable handle (hash) -> vector slot

    int32_t SlotForHandle(uint64_t handle) const;
};
