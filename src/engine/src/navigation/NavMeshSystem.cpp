#include "navigation/NavMeshSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "ECS.h"
#include "NavClass.h"
#include "lib.h"

namespace {

constexpr uint32_t kNavMeshBakeMagic   = 0x484D534E;  // 'NMSH'
constexpr uint32_t kNavMeshBakeVersion = 2;           // v2 = multi-class container

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

// fs::last_write_time → raw file_time tick count (uint64), used purely as an
// exact-match staleness signal (stored-at-bake vs current). Returns 0 on missing
// file or any fs error (also our "no bake" sentinel). We keep the native clock's
// full resolution (file_time_type ticks — 100 ns on MSVC) rather than truncating
// to seconds: a sub-second edit to world.json must invalidate the bake, and a
// 1 s granularity would miss edits made within the same wall-clock second.
uint64_t GetFileMtimeEpoch(const std::string& path) {
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<uint64_t>(ftime.time_since_epoch().count());
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

    // Auto-bake the full multi-class container so subsequent startups skip Rebuild.
    // Skips silently when no world path is known (e.g. test harness without SetWorldPath).
    if (!m_LastWorldPath.empty()) {
        const std::string bakePath = DeriveBakePath(m_LastWorldPath);
        if (!WriteBake(bakePath, GetFileMtimeEpoch(m_LastWorldPath)))
            SM_WARN("NavMeshSystem::Rebuild: failed to write disk bake to '%s'", bakePath.c_str());
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
        // Dilate by this class's agent radius: Detour's tilecache carves obstacles
        // at their literal radius (no agent-radius erosion), so inflate the carve
        // to keep each class's agent its radius clear of the obstacle.
        refs[i] = const_cast<NavMesh*>(cur.get())->AddCylinderObstacle(
            pos, radius + cur->AgentRadius(), height);
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
        // Dilate XZ by this class's agent radius (see AddCylinderObstacle); height (Y) unchanged.
        const float r = cur->AgentRadius();
        const glm::vec3 emin = bmin - glm::vec3(r, 0.0f, r);
        const glm::vec3 emax = bmax + glm::vec3(r, 0.0f, r);
        refs[i] = const_cast<NavMesh*>(cur.get())->AddBoxObstacle(emin, emax);
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

bool NavMeshSystem::WriteBake(const std::string& bakePath, uint64_t worldMtime) const
{
    std::ofstream ofs(bakePath, std::ios::binary);
    if (!ofs) { SM_WARN("NavMeshSystem::WriteBake: cannot open '%s' for write", bakePath.c_str()); return false; }

    const uint32_t magic = kNavMeshBakeMagic, version = kNavMeshBakeVersion;
    const uint64_t bakeEpoch = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    ofs.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));
    ofs.write(reinterpret_cast<const char*>(&bakeEpoch), sizeof(bakeEpoch));
    ofs.write(reinterpret_cast<const char*>(&worldMtime), sizeof(worldMtime));
    ofs.write(reinterpret_cast<const char*>(&m_ClassCount), sizeof(m_ClassCount));  // uint8_t

    for (uint8_t i = 0; i < m_ClassCount; ++i) {
        auto cur = Current(i);
        if (!cur) { SM_WARN("NavMeshSystem::WriteBake: null slot %u", (unsigned)i); return false; }
        if (!cur->WriteSection(ofs)) return false;
    }
    return ofs.good();
}

bool NavMeshSystem::SaveCurrentToDisk()
{
    if (m_ClassCount == 0) {
        SM_WARN("NavMeshSystem::SaveCurrentToDisk: no navmesh; nothing to save");
        return false;
    }
    if (m_LastWorldPath.empty()) {
        SM_WARN("NavMeshSystem::SaveCurrentToDisk: no world path set; cannot derive bake path");
        return false;
    }
    const std::string bakePath = DeriveBakePath(m_LastWorldPath);
    if (!WriteBake(bakePath, GetFileMtimeEpoch(m_LastWorldPath))) {
        SM_WARN("NavMeshSystem::SaveCurrentToDisk: WriteBake failed for '%s'", bakePath.c_str());
        return false;
    }
    SM_TRACE("NavMeshSystem::SaveCurrentToDisk: wrote '%s' (%u classes)", bakePath.c_str(), (unsigned)m_ClassCount);
    return true;
}

bool NavMeshSystem::TryLoadFromDisk(const std::string& worldPath)
{
    const std::string bakePath = DeriveBakePath(worldPath);
    std::ifstream ifs(bakePath, std::ios::binary);
    if (!ifs) { SM_TRACE("NavMeshSystem::TryLoadFromDisk: no bake at '%s'; will Rebuild", bakePath.c_str()); return false; }

    uint32_t magic = 0, version = 0;
    uint64_t bakeEpoch = 0, storedMtime = 0;
    uint8_t  classCount = 0;
    ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!ifs.good() || magic != kNavMeshBakeMagic) {
        SM_WARN("NavMeshSystem::TryLoadFromDisk: '%s' bad magic", bakePath.c_str()); return false;
    }
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!ifs.good() || version != kNavMeshBakeVersion) {
        SM_WARN("NavMeshSystem::TryLoadFromDisk: '%s' unsupported version %u (expected %u); will Rebuild",
                bakePath.c_str(), version, kNavMeshBakeVersion);
        return false;
    }
    ifs.read(reinterpret_cast<char*>(&bakeEpoch), sizeof(bakeEpoch));
    ifs.read(reinterpret_cast<char*>(&storedMtime), sizeof(storedMtime));
    ifs.read(reinterpret_cast<char*>(&classCount), sizeof(classCount));
    if (!ifs.good()) { SM_WARN("NavMeshSystem::TryLoadFromDisk: '%s' truncated header", bakePath.c_str()); return false; }
    if (classCount == 0 || classCount > kMaxNavClasses) {
        SM_WARN("NavMeshSystem::TryLoadFromDisk: '%s' bad classCount %u", bakePath.c_str(), (unsigned)classCount);
        return false;
    }

    const uint64_t currentMtime = GetFileMtimeEpoch(worldPath);
    if (storedMtime != currentMtime) {
        SM_TRACE("NavMeshSystem::TryLoadFromDisk: '%s' stale (stored %llu != current %llu); will Rebuild",
                 bakePath.c_str(), storedMtime, currentMtime);
        return false;
    }

    // Read ALL sections before publishing, so a mid-file failure leaves the live meshes intact.
    std::array<std::shared_ptr<const NavMesh>, kMaxNavClasses> loaded{};
    for (uint8_t i = 0; i < classCount; ++i) {
        auto sec = NavMesh::ReadSection(ifs);
        if (!sec) {
            SM_WARN("NavMeshSystem::TryLoadFromDisk: '%s' section %u failed; will Rebuild", bakePath.c_str(), (unsigned)i);
            return false;
        }
        loaded[i] = std::shared_ptr<const NavMesh>(std::move(sec));
    }

    m_EntityToObstacle.clear();
    m_Obstacles.clear();
    m_LastWorldPath = worldPath;
    for (uint8_t i = 0; i < classCount; ++i) PublishNavMesh(i, loaded[i]);
    for (uint8_t i = classCount; i < kMaxNavClasses; ++i) PublishNavMesh(i, {});
    m_ClassCount = classCount;
    SM_TRACE("NavMeshSystem::TryLoadFromDisk: loaded '%s' (%u classes)", bakePath.c_str(), (unsigned)classCount);
    return true;
}

// ---- Mesh CPU-data cache (Spec 5) ----

void NavMeshSystem::StoreMeshCpuData(uint64_t meshId,
                                     std::vector<MeshVertex>&& vertices,
                                     std::vector<uint32_t>&& indices)
{
    // Append-only: overwrite if meshId somehow reused (defensive; AddMesh
    // returns monotonic handles so this branch shouldn't fire in practice).
    m_MeshCpuData[meshId] = CachedMesh{ std::move(vertices), std::move(indices) };
}

bool NavMeshSystem::GetMeshCpuData(uint64_t meshId,
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
