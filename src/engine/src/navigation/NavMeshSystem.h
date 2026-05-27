#pragma once

#include <memory>
#include <unordered_map>

#include <glm/vec3.hpp>

#include "Engine.h"
#include "ECS.h"   // EntityId

class  ECS;
class  MeshSystem;
class  NavMesh;
struct NavMeshConfigComponent;

// Engine-side service holding the current navmesh. Build runs on GameThread; the
// resulting shared_ptr is atomic-published so any reader (RenderThread debug viz,
// game-side queries on the same GameThread) can grab a stable snapshot.
class ENGINE_API NavMeshSystem {
public:
    using ObstacleHandle = uint32_t;  // wraps dtObstacleRef

    static NavMeshSystem& Instance();

    // Build navmesh from current ECS state. GameThread only.
    // Clears the EntityId->ObstacleHandle map (old refs invalid against new dtTileCache).
    void Rebuild(const ECS& world, const NavMeshConfigComponent& cfg, const MeshSystem* meshSystem);

    // Get current navmesh. Any thread. May be null before first build.
    std::shared_ptr<const NavMesh> Current() const;

    // ---- Spec 2 additions ----

    // Drive dtTileCache::update on the current NavMesh. GameThread only. No-op if Current() is null.
    void Tick(float dt);

    // Forwarders to NavMesh add/remove. Return 0 on failure (no current navmesh, or dtTileCache failure).
    ObstacleHandle AddCylinderObstacle(const glm::vec3& pos, float radius, float height);
    ObstacleHandle AddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax);
    void           RemoveObstacle(ObstacleHandle h);

    // EntityId -> ObstacleHandle mapping. Side table so dtObstacleRef stays engine-side
    // (snapshot-thread mismatches if we stored it in a component).
    void           TrackObstacleForEntity(EntityId e, ObstacleHandle h);
    ObstacleHandle FindObstacleForEntity(EntityId e) const;  // returns 0 if not tracked
    void           UntrackEntity(EntityId e);

    int            ObstacleCount() const;  // for Navigation panel + tests

private:
    NavMeshSystem() = default;
    std::shared_ptr<const NavMesh>               m_Current;
    std::unordered_map<EntityId, ObstacleHandle> m_EntityToObstacle;
};
