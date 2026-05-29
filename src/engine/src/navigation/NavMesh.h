#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>
#include <thread>

#include <glm/vec3.hpp>

#include "Engine.h"

// Forward-declare Detour/Recast types so the header doesn't drag them in.
class  dtNavMesh;
class  dtNavMeshQuery;
struct dtTileCache;
struct dtTileCacheAlloc;
struct dtTileCacheCompressor;
struct dtTileCacheMeshProcess;

struct NavMeshTriangleSoup;          // from NavMeshBuilder.h
struct NavMeshConfigComponent;       // from ECS.h
struct NavClassConfig;               // from ECS.h

// Owning RAII wrapper for the per-NavMesh allocator/compressor/process objects
// so rebuild teardown is clean and we don't leak on shutdown.
struct ENGINE_API NavMeshAlloc {
    dtTileCacheAlloc*       Alloc      = nullptr;
    dtTileCacheCompressor*  Compressor = nullptr;
    dtTileCacheMeshProcess* MeshProc   = nullptr;
    NavMeshAlloc();
    ~NavMeshAlloc();
    NavMeshAlloc(const NavMeshAlloc&)            = delete;
    NavMeshAlloc& operator=(const NavMeshAlloc&) = delete;
};

// Custom deleter for dtNavMeshQuery — it's placement-new'd into a dtAlloc'd
// block, so the default ::delete the unique_ptr would do is undefined behavior.
struct ENGINE_API NavMeshQueryDeleter {
    void operator()(dtNavMeshQuery* q) const noexcept;
};

class ENGINE_API NavMesh {
public:
    struct PathPoint {
        glm::vec3 Position{0.0f};
        uint8_t   AreaId = 0;
    };

    // Build a navmesh from a triangle soup + config. Returns nullptr on failure
    // (already logged via SM_WARN). Caller atomic-publishes through NavMeshSystem.
    static std::unique_ptr<NavMesh> Build(const NavMeshTriangleSoup& soup,
                                          const NavMeshConfigComponent& cfg,
                                          const NavClassConfig& cls);

    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&)            = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // String-pulled path (Detour dtFindStraightPath). Empty result = no path
    // / out of search radius / start or end off the navmesh. GameThread only
    // (asserted in Debug — dtNavMeshQuery is not thread-safe).
    std::vector<PathPoint> FindPath(const glm::vec3& start,
                                    const glm::vec3& end,
                                    float maxSearchRadius = 50.0f) const;

    // Snap a world point to the closest poly. Returns false if no poly within
    // search extents (default 2m XZ, 4m Y).
    bool ClosestPoint(const glm::vec3& world, glm::vec3& out) const;

    // Agent (capsule) radius this mesh was built/eroded for, in world units
    // (the tilecache's walkableRadius). 0 if no tilecache. Used to dilate dynamic
    // obstacles by the agent radius — Detour's tilecache carves obstacles at their
    // literal radius with no agent-radius erosion (unlike static geometry).
    float AgentRadius() const;

    // Constrain a desired move to the navmesh surface (wall-slide via Detour
    // moveAlongSurface). If `start` has a poly within near extents, returns the
    // surface-constrained end position (slides along boundaries, never leaves the
    // mesh). If off-mesh, searches wider recovery extents and returns the nearest
    // poly point (pull back onto mesh) with a rate-limited SM_WARN; if even that
    // misses, returns `start` unchanged (no movement). No query / no mesh →
    // returns `desiredEnd` (unconstrained). GameThread only.
    glm::vec3 ConstrainMove(const glm::vec3& start, const glm::vec3& desiredEnd) const;

    // Collect poly outline edges as line segments (pairs of vec3 — caller draws
    // as DebugAppendLine pairs). Used by DebugRenderPass ShowNavMesh.
    void CollectPolyEdges(std::vector<glm::vec3>& outLines) const;

    // Stats for the Navigation panel's status block.
    struct Stats {
        int TilesBuilt = 0;
        int PolyCount  = 0;
        int VertCount  = 0;
        int MemoryKB   = 0;
    };
    Stats GetStats() const;

    // Serialize this mesh's tilecache state to a stream (no file header — the
    // container header is owned by NavMeshSystem). Returns false on null cache /
    // stream error. GameThread only.
    bool WriteSection(std::ostream& os) const;

    // Reconstruct a NavMesh from one section previously written by WriteSection.
    // Returns nullptr on stream error / malformed data (logs SM_WARN). GameThread only.
    static std::unique_ptr<NavMesh> ReadSection(std::istream& is);

    // Drive dtTileCache::update — applies queued add/removeObstacle calls and
    // re-bakes affected tiles. Single-call-per-tick policy from spec; remaining
    // work continues next tick.
    void Tick(float dt);

    // Queue a cylinder obstacle (Y-axis aligned). Returns 0 on failure (log via SM_WARN).
    // Caller is responsible for tracking the returned ref to later remove the obstacle.
    uint32_t AddCylinderObstacle(const glm::vec3& pos, float radius, float height);

    // Queue an AABB obstacle. Returns 0 on failure.
    uint32_t AddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax);

    // Remove a previously-added obstacle by its ref. No-op if ref is 0.
    void RemoveObstacle(uint32_t ref);

private:
    NavMeshAlloc                                              m_Alloc;
    dtTileCache*                                              m_TileCache = nullptr;
    dtNavMesh*                                                m_NavMesh   = nullptr;
    std::unique_ptr<dtNavMeshQuery, NavMeshQueryDeleter>      m_Query;
};
