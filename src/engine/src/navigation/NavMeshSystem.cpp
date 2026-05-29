#include "navigation/NavMeshSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>

#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "ECS.h"
#include "NavClass.h"
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
                            const NavMeshConfigComponent& cfg)
{
    const NavMeshTriangleSoup soup = NavMeshBuilder::CollectTriangles(world);
    const uint8_t liveCount = NavLiveClassCount(cfg);

    if (soup.Empty || soup.Tris.empty()) {
        SM_WARN("NavMeshSystem::Rebuild: no NavMeshSource entities; publishing empty navmesh");
        m_EntityToObstacle.clear();   // old dtObstacleRefs invalid against new dtTileCache
        m_Obstacles.clear();          // per-class refs invalid against freshly-built tilecaches
        for (uint8_t i = 0; i < kMaxNavClasses; ++i) PublishNavMesh(i, {});
        m_ClassCount = 0;
        return;
    }

    bool anyBuilt = false;
    for (uint8_t i = 0; i < liveCount; ++i) {
        auto fresh = NavMesh::Build(soup, cfg, cfg.Classes[i]);
        if (!fresh) {
            SM_WARN("NavMeshSystem::Rebuild: NavMesh::Build returned null for class %u", (unsigned)i);
            continue;
        }
        PublishNavMesh(i, std::shared_ptr<const NavMesh>(std::move(fresh)));
        anyBuilt = true;
    }
    for (uint8_t i = liveCount; i < kMaxNavClasses; ++i) PublishNavMesh(i, {});   // clear stale slots

    if (!anyBuilt) {
        SM_WARN("NavMeshSystem::Rebuild: no class meshes built; keeping previous");
        return;
    }
    m_EntityToObstacle.clear();   // old dtObstacleRefs invalid against new dtTileCache
    m_Obstacles.clear();          // per-class refs invalid against freshly-built tilecaches
    m_ClassCount = liveCount;

    // Disk auto-bake (single-mesh format) only for the single-class case.
    // Multi-class disk bake is the deferred follow-up (see spec non-goals).
    // Skips silently when no world path is known (e.g., test harness calling
    // Rebuild without prior SetWorldPath).
    if (liveCount == 1 && !m_LastWorldPath.empty()) {
        auto cur = Current(0);
        if (cur) {
            const std::string bakePath = DeriveBakePath(m_LastWorldPath);
            const uint64_t worldMtime = GetFileMtimeEpoch(m_LastWorldPath);
            if (!cur->SaveToFile(bakePath, worldMtime)) {
                SM_WARN("NavMeshSystem::Rebuild: failed to write disk bake to '%s'", bakePath.c_str());
            }
        }
    }
}

std::shared_ptr<const NavMesh> NavMeshSystem::Current(uint8_t classId) const {
    if (classId >= kMaxNavClasses) return {};
    return std::atomic_load(&m_Classes[classId]);
}

void NavMeshSystem::Tick(float dt) {
    // const_cast: NavMesh::Tick is a mutating operation on the dtTileCache.
    // shared_ptr<const NavMesh> is the snapshot type for cross-thread reads;
    // the GameThread owner needs the mutable view to drive dtTileCache::update.
    for (uint8_t i = 0; i < m_ClassCount; ++i) {
        auto cur = Current(i);
        if (cur) const_cast<NavMesh*>(cur.get())->Tick(dt);
    }
}

NavMeshSystem::ObstacleHandle NavMeshSystem::AddCylinderObstacle(
    const glm::vec3& pos, float radius, float height)
{
    std::array<uint32_t, kMaxNavClasses> refs{};
    bool any = false;
    for (uint8_t i = 0; i < m_ClassCount; ++i) {
        auto cur = Current(i);
        if (!cur) continue;
        refs[i] = const_cast<NavMesh*>(cur.get())->AddCylinderObstacle(pos, radius, height);
        any = any || (refs[i] != 0);
    }
    if (!any) return 0;
    const uint32_t id = m_NextObstacleId++;
    m_Obstacles[id] = refs;
    return id;
}

NavMeshSystem::ObstacleHandle NavMeshSystem::AddBoxObstacle(
    const glm::vec3& bmin, const glm::vec3& bmax)
{
    std::array<uint32_t, kMaxNavClasses> refs{};
    bool any = false;
    for (uint8_t i = 0; i < m_ClassCount; ++i) {
        auto cur = Current(i);
        if (!cur) continue;
        refs[i] = const_cast<NavMesh*>(cur.get())->AddBoxObstacle(bmin, bmax);
        any = any || (refs[i] != 0);
    }
    if (!any) return 0;
    const uint32_t id = m_NextObstacleId++;
    m_Obstacles[id] = refs;
    return id;
}

void NavMeshSystem::RemoveObstacle(ObstacleHandle id) {
    if (id == 0) return;
    auto it = m_Obstacles.find(id);
    if (it == m_Obstacles.end()) return;
    for (uint8_t i = 0; i < kMaxNavClasses; ++i) {
        const uint32_t ref = it->second[i];
        if (ref == 0) continue;
        auto cur = Current(i);
        if (cur) const_cast<NavMesh*>(cur.get())->RemoveObstacle(ref);
    }
    m_Obstacles.erase(it);
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
    auto cur = Current(0);
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
    m_Obstacles.clear();          // per-class refs invalid against freshly-loaded tilecache
    m_LastWorldPath = worldPath;
    std::shared_ptr<const NavMesh> shared(std::move(fresh));
    PublishNavMesh(0, shared);
    for (uint8_t i = 1; i < kMaxNavClasses; ++i) PublishNavMesh(i, {});   // clear stale slots
    m_ClassCount = 1;   // disk bake is single-mesh format → one live slot
    SM_TRACE("NavMeshSystem::TryLoadFromDisk: loaded '%s'", bakePath.c_str());
    return true;
}

// ---- Mesh CPU-data cache (Spec 5) ----

void NavMeshSystem::StoreMeshCpuData(uint32_t meshId,
                                     std::vector<MeshVertex>&& vertices,
                                     std::vector<uint32_t>&& indices)
{
    // Append-only: overwrite if meshId somehow reused (defensive; AddMesh
    // returns monotonic handles so this branch shouldn't fire in practice).
    m_MeshCpuData[meshId] = CachedMesh{ std::move(vertices), std::move(indices) };
}

bool NavMeshSystem::GetMeshCpuData(uint32_t meshId,
                                   std::span<const MeshVertex>& outVerts,
                                   std::span<const uint32_t>& outIndices) const
{
    auto it = m_MeshCpuData.find(meshId);
    if (it == m_MeshCpuData.end()) return false;
    outVerts   = std::span<const MeshVertex>(it->second.Vertices);
    outIndices = std::span<const uint32_t>(it->second.Indices);
    return true;
}

void NavMeshSystem::PublishNavMesh(uint8_t classId, std::shared_ptr<const NavMesh> mesh) {
    if (classId >= kMaxNavClasses) return;
    std::atomic_store(&m_Classes[classId], std::move(mesh));
    m_NavVersion.fetch_add(1, std::memory_order_relaxed);
}
