#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <glm/vec3.hpp>

#include "Engine.h"
#include "ECS.h"   // EntityId

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

private:
    NavMeshSystem() = default;
    std::shared_ptr<const NavMesh>               m_Current;
    std::unordered_map<EntityId, ObstacleHandle> m_EntityToObstacle;
    std::string                                  m_LastWorldPath;
};
