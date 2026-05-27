#include "navigation/NavMeshSystem.h"

#include <atomic>

#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "ECS.h"
#include "lib.h"

NavMeshSystem& NavMeshSystem::Instance() {
    static NavMeshSystem s;
    return s;
}

void NavMeshSystem::Rebuild(const ECS& world,
                            const NavMeshConfigComponent& cfg,
                            const MeshSystem* meshSystem)
{
    const NavMeshTriangleSoup soup = NavMeshBuilder::CollectTriangles(world, meshSystem);
    if (soup.Empty || soup.Tris.empty()) {
        SM_WARN("NavMeshSystem::Rebuild: no NavMeshSource entities; publishing empty navmesh");
        m_EntityToObstacle.clear();   // old dtObstacleRefs invalid against new dtTileCache
        std::atomic_store(&m_Current, std::shared_ptr<const NavMesh>{});
        return;
    }
    auto fresh = NavMesh::Build(soup, cfg);
    if (!fresh) {
        SM_WARN("NavMeshSystem::Rebuild: NavMesh::Build returned null; keeping previous navmesh");
        return;
    }
    m_EntityToObstacle.clear();   // old dtObstacleRefs invalid against new dtTileCache
    std::shared_ptr<const NavMesh> shared(std::move(fresh));
    std::atomic_store(&m_Current, shared);
}

std::shared_ptr<const NavMesh> NavMeshSystem::Current() const {
    return std::atomic_load(&m_Current);
}

void NavMeshSystem::Tick(float dt) {
    auto cur = std::atomic_load(&m_Current);
    if (!cur) return;
    // const_cast: NavMesh::Tick is a mutating operation on the dtTileCache.
    // shared_ptr<const NavMesh> is the snapshot type for cross-thread reads;
    // the GameThread owner needs the mutable view to drive dtTileCache::update.
    const_cast<NavMesh*>(cur.get())->Tick(dt);
}

NavMeshSystem::ObstacleHandle NavMeshSystem::AddCylinderObstacle(
    const glm::vec3& pos, float radius, float height)
{
    auto cur = std::atomic_load(&m_Current);
    if (!cur) return 0;
    return const_cast<NavMesh*>(cur.get())->AddCylinderObstacle(pos, radius, height);
}

NavMeshSystem::ObstacleHandle NavMeshSystem::AddBoxObstacle(
    const glm::vec3& bmin, const glm::vec3& bmax)
{
    auto cur = std::atomic_load(&m_Current);
    if (!cur) return 0;
    return const_cast<NavMesh*>(cur.get())->AddBoxObstacle(bmin, bmax);
}

void NavMeshSystem::RemoveObstacle(ObstacleHandle h) {
    auto cur = std::atomic_load(&m_Current);
    if (!cur || h == 0) return;
    const_cast<NavMesh*>(cur.get())->RemoveObstacle(h);
}

void NavMeshSystem::TrackObstacleForEntity(EntityId e, ObstacleHandle h) {
    if (h == 0) return;
    m_EntityToObstacle[e] = h;
}

NavMeshSystem::ObstacleHandle NavMeshSystem::FindObstacleForEntity(EntityId e) const {
    auto it = m_EntityToObstacle.find(e);
    return (it != m_EntityToObstacle.end()) ? it->second : 0;
}

void NavMeshSystem::UntrackEntity(EntityId e) {
    m_EntityToObstacle.erase(e);
}

int NavMeshSystem::ObstacleCount() const {
    return static_cast<int>(m_EntityToObstacle.size());
}
