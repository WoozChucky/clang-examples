#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include "Engine.h"
#include "ECS.h"   // EntityId
#include "ApplicationContext.h"   // MeshVertex

class  ECS;
class  MeshSystem;
class  NavMesh;
struct NavMeshConfigComponent;

class ENGINE_API NavMeshSystem {
public:
    using ObstacleHandle = uint32_t;

    static NavMeshSystem& Instance();

    // Build + atomic-publish (Spec 1). Auto-bakes to disk after successful
    // publish if SetWorldPath has been called (Spec 4 addition).
    void Rebuild(const ECS& world, const NavMeshConfigComponent& cfg,
                 const MeshSystem* meshSystem);

    std::shared_ptr<const NavMesh> Current() const;

    // ---- Spec 2: obstacles ----
    void Tick(float dt);
    ObstacleHandle AddCylinderObstacle(const glm::vec3& pos, float radius, float height);
    ObstacleHandle AddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax);
    void           RemoveObstacle(ObstacleHandle h);
    void           TrackObstacleForEntity(EntityId e, ObstacleHandle h);
    ObstacleHandle FindObstacleForEntity(EntityId e) const;
    void           UntrackEntity(EntityId e);
    int            ObstacleCount() const;

    // ---- Spec 4: disk bake ----

    // Tell NavMeshSystem the world file path it should bake alongside. Empty
    // string disables auto-bake. WorldManager calls this after successful
    // LoadWorldSnapshot; TryLoadFromDisk also sets it on successful load.
    void SetWorldPath(const std::string& worldPath);
    const std::string& GetWorldPath() const;

    // Save the currently-published NavMesh to disk (sibling of m_LastWorldPath
    // suffixed .navmesh.bin). Returns false on null Current() or write error.
    // GameThread only. Used by editor "Bake to Disk" button.
    bool SaveCurrentToDisk();

    // Attempt to load + publish a NavMesh from the on-disk bake corresponding
    // to worldPath. Validates staleness (stored WorldMtimeAtBakeTime ==
    // current fs::mtime). Returns true on success (NavMesh published, map
    // cleared, m_LastWorldPath set). False on missing/stale/corrupt — caller
    // should fall back to Rebuild. GameThread only.
    bool TryLoadFromDisk(const std::string& worldPath);

    // ---- Mesh CPU-data cache (Spec 5: navigation-mesh-input) ----

    // Append-only cache populated by GameThread when a MeshUpload response
    // arrives from RenderThread. Mirrors MeshSystem's append-only behavior —
    // entries are never removed (MeshSystem has no RemoveMesh today). Takes
    // ownership of the buffers via move. GameThread only.
    void StoreMeshCpuData(uint32_t meshId,
                          std::vector<MeshVertex>&& vertices,
                          std::vector<uint32_t>&& indices);

    // Cache lookup for NavMeshBuilder's Mesh-source branch. Returns false on
    // cache miss (caller falls back to its existing SM_WARN + skip path).
    // Spans valid until the next cache mutation; readers are single-threaded
    // GameThread per the NavMeshSystem contract.
    bool GetMeshCpuData(uint32_t meshId,
                        std::span<const MeshVertex>& outVerts,
                        std::span<const uint32_t>& outIndices) const;

private:
    NavMeshSystem() = default;
    std::shared_ptr<const NavMesh>               m_Current;
    std::unordered_map<EntityId, ObstacleHandle> m_EntityToObstacle;
    std::string                                  m_LastWorldPath;

    struct CachedMesh {
        std::vector<MeshVertex> Vertices;
        std::vector<uint32_t>   Indices;
    };
    std::unordered_map<uint32_t, CachedMesh> m_MeshCpuData;
};
