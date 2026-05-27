#include "navigation/NavMeshSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>

#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "ECS.h"
#include "lib.h"

namespace {

// Derive '<path>.navmesh.bin' from a world.json path by replacing the trailing
// '.json' extension. If path doesn't end in '.json' (or has no extension),
// just appends '.navmesh.bin'. Plain string op — no fs dep.
std::string DeriveBakePath(const std::string& worldPath) {
    constexpr const char* kSuffix = ".navmesh.bin";
    if (worldPath.size() >= 5 &&
        worldPath.compare(worldPath.size() - 5, 5, ".json") == 0) {
        return worldPath.substr(0, worldPath.size() - 5) + kSuffix;
    }
    return worldPath + kSuffix;
}

// fs::last_write_time → seconds since system_clock epoch (uint64).
// Returns 0 on missing file or any fs error (also our "no bake" sentinel).
uint64_t GetFileMtimeEpoch(const std::string& path) {
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    // file_time_type is implementation-defined epoch; convert via clock_cast
    // (C++20). MSVC supports clock_cast in C++20+; project is C++23 per CLAUDE.md.
    const auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            sysTime.time_since_epoch()).count());
}

} // namespace

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

    // Spec 4: auto-bake to disk so subsequent startups can skip Rebuild.
    // Skips silently when no world path is known (e.g., test harness calling
    // Rebuild without prior SetWorldPath).
    if (!m_LastWorldPath.empty()) {
        const std::string bakePath = DeriveBakePath(m_LastWorldPath);
        const uint64_t worldMtime = GetFileMtimeEpoch(m_LastWorldPath);
        if (!shared->SaveToFile(bakePath, worldMtime)) {
            SM_WARN("NavMeshSystem::Rebuild: failed to write disk bake to '%s'", bakePath.c_str());
        }
    }
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

void NavMeshSystem::SetWorldPath(const std::string& worldPath) {
    m_LastWorldPath = worldPath;
}

const std::string& NavMeshSystem::GetWorldPath() const {
    return m_LastWorldPath;
}

bool NavMeshSystem::SaveCurrentToDisk() {
    auto cur = std::atomic_load(&m_Current);
    if (!cur) {
        SM_WARN("NavMeshSystem::SaveCurrentToDisk: no current NavMesh; nothing to save");
        return false;
    }
    if (m_LastWorldPath.empty()) {
        SM_WARN("NavMeshSystem::SaveCurrentToDisk: no world path set; cannot derive bake path");
        return false;
    }
    const std::string bakePath = DeriveBakePath(m_LastWorldPath);
    const uint64_t worldMtime = GetFileMtimeEpoch(m_LastWorldPath);
    if (!cur->SaveToFile(bakePath, worldMtime)) {
        SM_WARN("NavMeshSystem::SaveCurrentToDisk: SaveToFile failed for '%s'", bakePath.c_str());
        return false;
    }
    SM_TRACE("NavMeshSystem::SaveCurrentToDisk: wrote '%s'", bakePath.c_str());
    return true;
}

bool NavMeshSystem::TryLoadFromDisk(const std::string& worldPath) {
    const std::string bakePath = DeriveBakePath(worldPath);
    if (!std::filesystem::exists(bakePath)) {
        SM_TRACE("NavMeshSystem::TryLoadFromDisk: no bake at '%s'; will Rebuild", bakePath.c_str());
        return false;
    }

    uint64_t storedWorldMtime = 0;
    auto fresh = NavMesh::LoadFromFile(bakePath, &storedWorldMtime);
    if (!fresh) {
        // LoadFromFile already SM_WARNed with cause.
        return false;
    }

    const uint64_t currentWorldMtime = GetFileMtimeEpoch(worldPath);
    if (storedWorldMtime != currentWorldMtime) {
        SM_TRACE("NavMeshSystem::TryLoadFromDisk: '%s' stale "
                 "(stored mtime %llu != current %llu); will Rebuild",
                 bakePath.c_str(), storedWorldMtime, currentWorldMtime);
        return false;  // fresh NavMesh discarded; minor waste, edge case
    }

    m_EntityToObstacle.clear();   // same invariant as Rebuild — new tilecache, old refs invalid
    m_LastWorldPath = worldPath;
    std::shared_ptr<const NavMesh> shared(std::move(fresh));
    std::atomic_store(&m_Current, shared);
    SM_TRACE("NavMeshSystem::TryLoadFromDisk: loaded '%s'", bakePath.c_str());
    return true;
}
