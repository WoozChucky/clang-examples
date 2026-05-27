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
        std::atomic_store(&m_Current, std::shared_ptr<const NavMesh>{});
        return;
    }
    auto fresh = NavMesh::Build(soup, cfg);
    if (!fresh) {
        SM_WARN("NavMeshSystem::Rebuild: NavMesh::Build returned null; keeping previous navmesh");
        return;
    }
    std::shared_ptr<const NavMesh> shared(std::move(fresh));
    std::atomic_store(&m_Current, shared);
}

std::shared_ptr<const NavMesh> NavMeshSystem::Current() const {
    return std::atomic_load(&m_Current);
}
