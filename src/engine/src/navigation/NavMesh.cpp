#include "navigation/NavMesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>
#include <unordered_set>

#include <Recast.h>
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourTileCache.h>
#include <DetourTileCacheBuilder.h>

#include "navigation/NavMeshBuilder.h"
#include "ECS.h"
#include "lib.h"    // SM_WARN, SM_ASSERT

namespace {

// ---------- Linear allocator + simple no-op compressor + mesh processor ----------
// Adapted from RecastDemo Sample_TempObstacles.cpp. Compressor is intentionally
// a no-op (memcpy) — we don't ship the fastlz dep, and Spec 1 navmeshes are tiny
// so compression buys nothing. Spec 4 (disk bake) may revisit.

struct LinearAllocator : public dtTileCacheAlloc {
    unsigned char* buffer = nullptr;
    size_t         capacity = 0;
    size_t         top = 0;
    size_t         high = 0;

    explicit LinearAllocator(size_t cap) : capacity(cap) {
        buffer = (unsigned char*)dtAlloc(cap, DT_ALLOC_PERM);
    }
    ~LinearAllocator() override { dtFree(buffer); }

    void reset() override { high = std::max(high, top); top = 0; }
    void* alloc(const size_t size) override {
        if (!buffer || top + size > capacity) return nullptr;
        unsigned char* p = buffer + top;
        top += size;
        return p;
    }
    void free(void*) override { /* no-op (LinearAllocator owns all) */ }
};

struct NoopCompressor : public dtTileCacheCompressor {
    int maxCompressedSize(const int bufferSize) override { return bufferSize + 1; }
    dtStatus compress(const unsigned char* buffer, const int bufferSize,
                      unsigned char* compressed, const int /*maxCompressedSize*/,
                      int* compressedSize) override {
        std::memcpy(compressed, buffer, bufferSize);
        *compressedSize = bufferSize;
        return DT_SUCCESS;
    }
    dtStatus decompress(const unsigned char* compressed, const int compressedSize,
                        unsigned char* buffer, const int /*maxBufferSize*/,
                        int* bufferSize) override {
        std::memcpy(buffer, compressed, compressedSize);
        *bufferSize = compressedSize;
        return DT_SUCCESS;
    }
};

struct MeshProcess : public dtTileCacheMeshProcess {
    void process(struct dtNavMeshCreateParams* params,
                 unsigned char* polyAreas,
                 unsigned short* polyFlags) override {
        for (int i = 0; i < params->polyCount; ++i) {
            if (polyAreas[i] != RC_NULL_AREA) {
                polyFlags[i] = 0x01;   // walkable bit; future spec adds per-area flags
            }
        }
    }
};

// Thread-id pin for the dtNavMeshQuery (debug-only assert in FindPath).
inline std::thread::id& NavQueryOwnerThread() {
    static std::thread::id id;
    return id;
}

} // namespace

// ---------- NavMeshQueryDeleter ----------
// dtNavMeshQuery is allocated via dtAlloc + placement-new, so it must be freed
// via dtFreeNavMeshQuery (manual dtor call + dtFree) — never ::delete.
void NavMeshQueryDeleter::operator()(dtNavMeshQuery* q) const noexcept {
    if (q) dtFreeNavMeshQuery(q);
}

// ---------- NavMeshAlloc RAII ----------
NavMeshAlloc::NavMeshAlloc() {
    Alloc      = new LinearAllocator(64 * 1024); // 64 KB scratch — bumped if a build needs more
    Compressor = new NoopCompressor();
    MeshProc   = new MeshProcess();
}
NavMeshAlloc::~NavMeshAlloc() {
    delete static_cast<LinearAllocator*>(Alloc);
    delete static_cast<NoopCompressor*>(Compressor);
    delete static_cast<MeshProcess*>(MeshProc);
}

// ---------- NavMesh ctor/dtor ----------
NavMesh::NavMesh() = default;
NavMesh::~NavMesh() {
    if (m_TileCache) { dtFreeTileCache(m_TileCache); m_TileCache = nullptr; }
    if (m_NavMesh)   { dtFreeNavMesh(m_NavMesh);     m_NavMesh   = nullptr; }
    // m_Query is unique_ptr with NavMeshQueryDeleter — auto-freed via dtFreeNavMeshQuery.
}

// ---------- Build ----------
std::unique_ptr<NavMesh> NavMesh::Build(const NavMeshTriangleSoup& soup,
                                        const NavMeshConfigComponent& cfg)
{
    if (soup.Empty || soup.Tris.empty()) {
        SM_WARN("NavMesh::Build called with empty soup; returning null");
        return nullptr;
    }

    auto out = std::unique_ptr<NavMesh>(new NavMesh());

    // ----- rcConfig from NavMeshConfigComponent -----
    rcConfig rcc{};
    rcc.cs                     = cfg.CellSize;
    rcc.ch                     = cfg.CellHeight;
    rcc.walkableSlopeAngle     = cfg.AgentMaxSlope;
    rcc.walkableHeight         = (int)std::ceil(cfg.AgentHeight / cfg.CellHeight);
    rcc.walkableClimb          = (int)std::floor(cfg.AgentMaxClimb / cfg.CellHeight);
    rcc.walkableRadius         = (int)std::ceil(cfg.AgentRadius / cfg.CellSize);
    rcc.maxEdgeLen             = (int)(12.0f / cfg.CellSize);
    rcc.maxSimplificationError = 1.3f;
    rcc.minRegionArea          = (int)rcSqr(8);
    rcc.mergeRegionArea        = (int)rcSqr(20);
    rcc.maxVertsPerPoly        = DT_VERTS_PER_POLYGON;
    rcc.tileSize               = (int)cfg.TileSize;
    rcc.borderSize             = rcc.walkableRadius + 3;
    rcc.width                  = rcc.tileSize + rcc.borderSize * 2;
    rcc.height                 = rcc.tileSize + rcc.borderSize * 2;
    rcc.detailSampleDist       = (rcc.cs * 6.0f);
    rcc.detailSampleMaxError   = (rcc.ch * 1.0f);

    // Headroom above the highest walkable surface so rcFilterWalkableLowHeightSpans
    // can verify clearance for an AgentHeight-tall agent on the topmost walkable
    // span. Without padding, a flat-only scene whose highest geometry sits at the
    // heightfield ceiling is fine (filter shortcuts to ceiling=MAX_HEIGHTFIELD), but
    // a scene with a low ceiling close to the floor (within walkableHeight voxels)
    // would have every walkable span clipped. Slack (+1m) covers numerical edge
    // cases at exact voxel boundaries.
    const float bmin[3] = { soup.AabbMin.x, soup.AabbMin.y, soup.AabbMin.z };
    const float bmax[3] = { soup.AabbMax.x,
                            soup.AabbMax.y + cfg.AgentHeight + 1.0f,
                            soup.AabbMax.z };
    int gw = 0, gh = 0;
    rcCalcGridSize(bmin, bmax, rcc.cs, &gw, &gh);
    const int tw = (gw + rcc.tileSize - 1) / rcc.tileSize;
    const int th = (gh + rcc.tileSize - 1) / rcc.tileSize;

    // ----- dtTileCacheParams -----
    dtTileCacheParams tcParams{};
    rcVcopy(tcParams.orig, bmin);
    tcParams.cs                     = cfg.CellSize;
    tcParams.ch                     = cfg.CellHeight;
    tcParams.width                  = rcc.tileSize;
    tcParams.height                 = rcc.tileSize;
    tcParams.walkableHeight         = cfg.AgentHeight;
    tcParams.walkableRadius         = cfg.AgentRadius;
    tcParams.walkableClimb          = cfg.AgentMaxClimb;
    tcParams.maxSimplificationError = 1.3f;
    tcParams.maxTiles               = tw * th * 4;        // 4 layers max per tile (Recast sample default)
    tcParams.maxObstacles           = cfg.MaxObstacles;

    out->m_TileCache = dtAllocTileCache();
    if (!out->m_TileCache) { SM_WARN("dtAllocTileCache failed"); return nullptr; }
    if (dtStatusFailed(out->m_TileCache->init(&tcParams,
                                              out->m_Alloc.Alloc,
                                              out->m_Alloc.Compressor,
                                              out->m_Alloc.MeshProc))) {
        SM_WARN("dtTileCache::init failed");
        return nullptr;
    }

    // ----- dtNavMesh + params -----
    dtNavMeshParams nmParams{};
    rcVcopy(nmParams.orig, bmin);
    nmParams.tileWidth  = rcc.tileSize * cfg.CellSize;
    nmParams.tileHeight = rcc.tileSize * cfg.CellSize;
    nmParams.maxTiles   = tw * th;
    nmParams.maxPolys   = 16384;

    out->m_NavMesh = dtAllocNavMesh();
    if (!out->m_NavMesh) { SM_WARN("dtAllocNavMesh failed"); return nullptr; }
    if (dtStatusFailed(out->m_NavMesh->init(&nmParams))) {
        SM_WARN("dtNavMesh::init failed");
        return nullptr;
    }

    // ----- Build each tile (boilerplate adapted from Sample_TempObstacles) -----
    rcContext ctx(false);
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            rcConfig tcfg = rcc;
            tcfg.bmin[0] = bmin[0] + (x * rcc.tileSize - rcc.borderSize) * rcc.cs;
            tcfg.bmin[1] = bmin[1];
            tcfg.bmin[2] = bmin[2] + (y * rcc.tileSize - rcc.borderSize) * rcc.cs;
            tcfg.bmax[0] = bmin[0] + ((x + 1) * rcc.tileSize + rcc.borderSize) * rcc.cs;
            tcfg.bmax[1] = bmax[1];
            tcfg.bmax[2] = bmin[2] + ((y + 1) * rcc.tileSize + rcc.borderSize) * rcc.cs;

            rcHeightfield* hf = rcAllocHeightfield();
            if (!hf || !rcCreateHeightfield(&ctx, *hf, tcfg.width, tcfg.height,
                                            tcfg.bmin, tcfg.bmax, tcfg.cs, tcfg.ch)) {
                rcFreeHeightField(hf);
                SM_WARN("rcCreateHeightfield failed for tile (%d,%d)", x, y);
                continue;
            }

            const int triCount = (int)(soup.Tris.size() / 3);
            std::vector<unsigned char> areasMutable(soup.Areas.begin(), soup.Areas.end());
            rcClearUnwalkableTriangles(&ctx, tcfg.walkableSlopeAngle,
                                       soup.Verts.data(), (int)(soup.Verts.size() / 3),
                                       soup.Tris.data(), triCount,
                                       areasMutable.data());
            if (!rcRasterizeTriangles(&ctx,
                                      soup.Verts.data(), (int)(soup.Verts.size() / 3),
                                      soup.Tris.data(), areasMutable.data(), triCount,
                                      *hf, tcfg.walkableClimb)) {
                rcFreeHeightField(hf);
                SM_WARN("rcRasterizeTriangles failed for tile (%d,%d)", x, y);
                continue;
            }

            rcFilterLowHangingWalkableObstacles(&ctx, tcfg.walkableClimb, *hf);
            rcFilterLedgeSpans(&ctx, tcfg.walkableHeight, tcfg.walkableClimb, *hf);
            rcFilterWalkableLowHeightSpans(&ctx, tcfg.walkableHeight, *hf);

            rcCompactHeightfield* chf = rcAllocCompactHeightfield();
            if (!chf || !rcBuildCompactHeightfield(&ctx, tcfg.walkableHeight, tcfg.walkableClimb, *hf, *chf)) {
                rcFreeHeightField(hf); rcFreeCompactHeightfield(chf);
                SM_WARN("rcBuildCompactHeightfield failed for tile (%d,%d)", x, y);
                continue;
            }
            rcFreeHeightField(hf);

            if (!rcErodeWalkableArea(&ctx, tcfg.walkableRadius, *chf)) {
                rcFreeCompactHeightfield(chf);
                SM_WARN("rcErodeWalkableArea failed for tile (%d,%d)", x, y);
                continue;
            }

            rcHeightfieldLayerSet* lset = rcAllocHeightfieldLayerSet();
            if (!lset || !rcBuildHeightfieldLayers(&ctx, *chf, tcfg.borderSize, tcfg.walkableHeight, *lset)) {
                rcFreeCompactHeightfield(chf); rcFreeHeightfieldLayerSet(lset);
                SM_WARN("rcBuildHeightfieldLayers failed for tile (%d,%d)", x, y);
                continue;
            }
            rcFreeCompactHeightfield(chf);

            for (int i = 0; i < lset->nlayers; ++i) {
                const rcHeightfieldLayer* layer = &lset->layers[i];
                dtTileCacheLayerHeader header{};
                header.magic   = DT_TILECACHE_MAGIC;
                header.version = DT_TILECACHE_VERSION;
                header.tx = x; header.ty = y; header.tlayer = i;
                dtVcopy(header.bmin, layer->bmin);
                dtVcopy(header.bmax, layer->bmax);
                header.hmin   = (unsigned short)layer->hmin;
                header.hmax   = (unsigned short)layer->hmax;
                header.width  = (unsigned char)layer->width;
                header.height = (unsigned char)layer->height;
                header.minx   = (unsigned char)layer->minx;
                header.maxx   = (unsigned char)layer->maxx;
                header.miny   = (unsigned char)layer->miny;
                header.maxy   = (unsigned char)layer->maxy;

                unsigned char* tileData = nullptr;
                int tileSize = 0;
                if (dtStatusFailed(dtBuildTileCacheLayer(out->m_Alloc.Compressor,
                                                         &header, layer->heights, layer->areas, layer->cons,
                                                         &tileData, &tileSize))) {
                    SM_WARN("dtBuildTileCacheLayer failed for tile (%d,%d) layer %d", x, y, i);
                    continue;
                }
                dtCompressedTileRef ref = 0;
                if (dtStatusFailed(out->m_TileCache->addTile(tileData, tileSize, DT_COMPRESSEDTILE_FREE_DATA, &ref))) {
                    dtFree(tileData);
                    SM_WARN("dtTileCache::addTile failed for tile (%d,%d) layer %d", x, y, i);
                    continue;
                }
            }
            rcFreeHeightfieldLayerSet(lset);
        }
    }

    // Build all tiles into the nav mesh.
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            out->m_TileCache->buildNavMeshTilesAt(x, y, out->m_NavMesh);
        }
    }

    // Init query for runtime path lookups; pin to current thread.
    out->m_Query = std::unique_ptr<dtNavMeshQuery, NavMeshQueryDeleter>(dtAllocNavMeshQuery());
    if (!out->m_Query || dtStatusFailed(out->m_Query->init(out->m_NavMesh, 2048))) {
        SM_WARN("dtNavMeshQuery::init failed");
        return nullptr;
    }
    NavQueryOwnerThread() = std::this_thread::get_id();

    return out;
}

// ---------- FindPath ----------
std::vector<NavMesh::PathPoint> NavMesh::FindPath(const glm::vec3& start,
                                                  const glm::vec3& end,
                                                  float maxSearchRadius) const
{
    SM_ASSERT(std::this_thread::get_id() == NavQueryOwnerThread(),
              "NavMesh::FindPath called from non-owner thread; dtNavMeshQuery is not thread-safe");
    std::vector<PathPoint> out;
    if (!m_Query || !m_NavMesh) return out;

    const float ext[3] = { maxSearchRadius, maxSearchRadius, maxSearchRadius };
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    dtPolyRef startRef = 0, endRef = 0;
    float startNearest[3], endNearest[3];
    const float s[3] = { start.x, start.y, start.z };
    const float e[3] = { end.x,   end.y,   end.z   };
    m_Query->findNearestPoly(s, ext, &filter, &startRef, startNearest);
    m_Query->findNearestPoly(e, ext, &filter, &endRef,   endNearest);
    if (!startRef || !endRef) return out;

    dtPolyRef polys[256];
    int npolys = 0;
    if (dtStatusFailed(m_Query->findPath(startRef, endRef, startNearest, endNearest,
                                         &filter, polys, &npolys, 256))) {
        return out;
    }
    if (npolys == 0) return out;

    float straight[256 * 3];
    unsigned char straightFlags[256];
    dtPolyRef straightRefs[256];
    int nstraight = 0;
    if (dtStatusFailed(m_Query->findStraightPath(startNearest, endNearest,
                                                 polys, npolys,
                                                 straight, straightFlags, straightRefs,
                                                 &nstraight, 256))) {
        return out;
    }
    out.reserve(nstraight);
    for (int i = 0; i < nstraight; ++i) {
        PathPoint p;
        p.Position = glm::vec3(straight[i*3+0], straight[i*3+1], straight[i*3+2]);
        p.AreaId   = 0;
        out.push_back(p);
    }
    return out;
}

bool NavMesh::ClosestPoint(const glm::vec3& world, glm::vec3& out) const
{
    if (!m_Query) return false;
    const float ext[3] = { 2.0f, 4.0f, 2.0f };
    dtQueryFilter filter;
    dtPolyRef ref = 0;
    float nearest[3];
    const float p[3] = { world.x, world.y, world.z };
    m_Query->findNearestPoly(p, ext, &filter, &ref, nearest);
    if (!ref) return false;
    out = glm::vec3(nearest[0], nearest[1], nearest[2]);
    return true;
}

void NavMesh::CollectPolyEdges(std::vector<glm::vec3>& outLines) const
{
    if (!m_NavMesh) return;
    // Force the public const getTile(int) overload — the non-const sibling is private.
    const dtNavMesh* nav = m_NavMesh;
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* tile = nav->getTile(i);
        if (!tile || !tile->header) continue;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly* poly = &tile->polys[p];
            for (int v = 0; v < poly->vertCount; ++v) {
                const int vi0 = poly->verts[v];
                const int vi1 = poly->verts[(v + 1) % poly->vertCount];
                const float* a = &tile->verts[vi0 * 3];
                const float* b = &tile->verts[vi1 * 3];
                outLines.emplace_back(a[0], a[1], a[2]);
                outLines.emplace_back(b[0], b[1], b[2]);
            }
        }
    }
}

NavMesh::Stats NavMesh::GetStats() const
{
    Stats s;
    if (!m_NavMesh) return s;
    // Force the public const getTile(int) overload — the non-const sibling is private.
    const dtNavMesh* nav = m_NavMesh;
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* tile = nav->getTile(i);
        if (!tile || !tile->header) continue;
        ++s.TilesBuilt;
        s.PolyCount += tile->header->polyCount;
        s.VertCount += tile->header->vertCount;
    }
    s.MemoryKB = (int)((s.VertCount * sizeof(float) * 3 + s.PolyCount * 64) / 1024);
    return s;
}

// ---------- Tick / Obstacle API ----------
void NavMesh::Tick(float dt)
{
    if (!m_TileCache || !m_NavMesh) return;
    bool upToDate = true;
    m_TileCache->update(dt, m_NavMesh, &upToDate);
    // Single-call policy per spec; if !upToDate, remaining work continues next tick.
}

uint32_t NavMesh::AddCylinderObstacle(const glm::vec3& pos, float radius, float height)
{
    if (!m_TileCache) return 0;
    const float p[3] = { pos.x, pos.y, pos.z };
    dtObstacleRef ref = 0;
    if (dtStatusFailed(m_TileCache->addObstacle(p, radius, height, &ref))) {
        SM_WARN("NavMesh::AddCylinderObstacle: dtTileCache::addObstacle failed "
                "(MaxObstacles cap reached?)");
        return 0;
    }
    return static_cast<uint32_t>(ref);
}

uint32_t NavMesh::AddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax)
{
    if (!m_TileCache) return 0;
    const float mn[3] = { bmin.x, bmin.y, bmin.z };
    const float mx[3] = { bmax.x, bmax.y, bmax.z };
    dtObstacleRef ref = 0;
    if (dtStatusFailed(m_TileCache->addBoxObstacle(mn, mx, &ref))) {
        SM_WARN("NavMesh::AddBoxObstacle: dtTileCache::addBoxObstacle failed "
                "(MaxObstacles cap reached?)");
        return 0;
    }
    return static_cast<uint32_t>(ref);
}

void NavMesh::RemoveObstacle(uint32_t ref)
{
    if (!m_TileCache || ref == 0) return;
    m_TileCache->removeObstacle(static_cast<dtObstacleRef>(ref));
}

// ---------- Spec 4: SaveToFile / LoadFromFile ----------

namespace {
constexpr uint32_t kNavMeshBakeMagic   = 0x484D534E;  // 'NMSH' little-endian
constexpr uint32_t kNavMeshBakeVersion = 1;
}

bool NavMesh::SaveToFile(const std::string& path, uint64_t worldMtimeAtBake) const
{
    if (!m_TileCache || !m_NavMesh) {
        SM_WARN("NavMesh::SaveToFile: null tile cache / nav mesh; nothing to save");
        return false;
    }

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        SM_WARN("NavMesh::SaveToFile: cannot open '%s' for write", path.c_str());
        return false;
    }

    // Header
    const uint32_t magic   = kNavMeshBakeMagic;
    const uint32_t version = kNavMeshBakeVersion;
    const uint64_t bakeEpoch = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    ofs.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));
    ofs.write(reinterpret_cast<const char*>(&bakeEpoch), sizeof(bakeEpoch));
    ofs.write(reinterpret_cast<const char*>(&worldMtimeAtBake), sizeof(worldMtimeAtBake));

    // dtTileCacheParams + dtNavMeshParams (raw POD bytes)
    const dtTileCacheParams* tcp = m_TileCache->getParams();
    ofs.write(reinterpret_cast<const char*>(tcp), sizeof(dtTileCacheParams));

    const dtNavMeshParams* nmp = m_NavMesh->getParams();
    ofs.write(reinterpret_cast<const char*>(nmp), sizeof(dtNavMeshParams));

    // Count actually-populated tiles first (m_params.maxTiles is capacity, not used count).
    const int tileCap = m_TileCache->getTileCount();
    int tileCount = 0;
    for (int i = 0; i < tileCap; ++i) {
        const dtCompressedTile* tile = m_TileCache->getTile(i);
        if (tile && tile->header && tile->compressed && tile->compressedSize > 0) {
            ++tileCount;
        }
    }
    ofs.write(reinterpret_cast<const char*>(&tileCount), sizeof(tileCount));

    // Per-tile: compressedSize + compressed bytes (this is what addTile takes back).
    for (int i = 0; i < tileCap; ++i) {
        const dtCompressedTile* tile = m_TileCache->getTile(i);
        if (!tile || !tile->header || !tile->compressed || tile->compressedSize <= 0) continue;
        ofs.write(reinterpret_cast<const char*>(&tile->compressedSize), sizeof(int));
        ofs.write(reinterpret_cast<const char*>(tile->compressed), tile->compressedSize);
    }

    return ofs.good();
}

std::unique_ptr<NavMesh> NavMesh::LoadFromFile(const std::string& path, uint64_t* outWorldMtime)
{
    if (outWorldMtime) *outWorldMtime = 0;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        SM_WARN("NavMesh::LoadFromFile: cannot open '%s' for read", path.c_str());
        return nullptr;
    }

    // Header
    uint32_t magic = 0, version = 0;
    uint64_t bakeEpoch = 0, worldMtime = 0;
    ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    ifs.read(reinterpret_cast<char*>(&bakeEpoch), sizeof(bakeEpoch));
    ifs.read(reinterpret_cast<char*>(&worldMtime), sizeof(worldMtime));
    if (!ifs.good() || magic != kNavMeshBakeMagic) {
        SM_WARN("NavMesh::LoadFromFile: '%s' bad magic (got 0x%08x, expected 0x%08x)",
                path.c_str(), magic, kNavMeshBakeMagic);
        return nullptr;
    }
    if (version != kNavMeshBakeVersion) {
        SM_WARN("NavMesh::LoadFromFile: '%s' unsupported format version %u (expected %u)",
                path.c_str(), version, kNavMeshBakeVersion);
        return nullptr;
    }
    if (outWorldMtime) *outWorldMtime = worldMtime;

    auto out = std::unique_ptr<NavMesh>(new NavMesh());

    // Params
    dtTileCacheParams tcParams{};
    ifs.read(reinterpret_cast<char*>(&tcParams), sizeof(dtTileCacheParams));
    dtNavMeshParams nmParams{};
    ifs.read(reinterpret_cast<char*>(&nmParams), sizeof(dtNavMeshParams));
    if (!ifs.good()) {
        SM_WARN("NavMesh::LoadFromFile: '%s' truncated reading params", path.c_str());
        return nullptr;
    }

    // dtTileCache init
    out->m_TileCache = dtAllocTileCache();
    if (!out->m_TileCache) { SM_WARN("dtAllocTileCache failed"); return nullptr; }
    if (dtStatusFailed(out->m_TileCache->init(&tcParams,
                                              out->m_Alloc.Alloc,
                                              out->m_Alloc.Compressor,
                                              out->m_Alloc.MeshProc))) {
        SM_WARN("NavMesh::LoadFromFile: dtTileCache::init failed");
        return nullptr;
    }

    // dtNavMesh init
    out->m_NavMesh = dtAllocNavMesh();
    if (!out->m_NavMesh) { SM_WARN("dtAllocNavMesh failed"); return nullptr; }
    if (dtStatusFailed(out->m_NavMesh->init(&nmParams))) {
        SM_WARN("NavMesh::LoadFromFile: dtNavMesh::init failed");
        return nullptr;
    }

    // Per-tile read + addTile
    int tileCount = 0;
    ifs.read(reinterpret_cast<char*>(&tileCount), sizeof(int));
    if (!ifs.good() || tileCount < 0 || tileCount > tcParams.maxTiles) {
        SM_WARN("NavMesh::LoadFromFile: '%s' bad TileCount %d (max %d)",
                path.c_str(), tileCount, tcParams.maxTiles);
        return nullptr;
    }

    for (int i = 0; i < tileCount; ++i) {
        int dataSize = 0;
        ifs.read(reinterpret_cast<char*>(&dataSize), sizeof(int));
        if (!ifs.good() || dataSize <= 0) {
            SM_WARN("NavMesh::LoadFromFile: '%s' bad tile %d size %d", path.c_str(), i, dataSize);
            return nullptr;
        }
        unsigned char* tileData = (unsigned char*)dtAlloc(dataSize, DT_ALLOC_PERM);
        if (!tileData) {
            SM_WARN("NavMesh::LoadFromFile: dtAlloc(%d) failed for tile %d", dataSize, i);
            return nullptr;
        }
        ifs.read(reinterpret_cast<char*>(tileData), dataSize);
        if (!ifs.good()) {
            dtFree(tileData);
            SM_WARN("NavMesh::LoadFromFile: '%s' truncated reading tile %d", path.c_str(), i);
            return nullptr;
        }
        dtCompressedTileRef ref = 0;
        if (dtStatusFailed(out->m_TileCache->addTile(tileData, dataSize,
                                                    DT_COMPRESSEDTILE_FREE_DATA, &ref))) {
            dtFree(tileData);
            SM_WARN("NavMesh::LoadFromFile: dtTileCache::addTile failed for tile %d", i);
            return nullptr;
        }
    }

    // Build nav mesh tiles from the compressed tile cache (same final pass as Build).
    // Iterate loaded tiles, dedupe (tx, ty) via a uint64-key set.
    {
        const int tileCap = out->m_TileCache->getTileCount();
        std::unordered_set<uint64_t> built;
        for (int i = 0; i < tileCap; ++i) {
            const dtCompressedTile* tile = out->m_TileCache->getTile(i);
            if (!tile || !tile->header) continue;
            const uint64_t key = (uint64_t(uint32_t(tile->header->tx)) << 32)
                               |  uint64_t(uint32_t(tile->header->ty));
            if (built.insert(key).second) {
                out->m_TileCache->buildNavMeshTilesAt(tile->header->tx, tile->header->ty,
                                                     out->m_NavMesh);
            }
        }
    }

    // Init query + pin thread (same as Build).
    out->m_Query = std::unique_ptr<dtNavMeshQuery, NavMeshQueryDeleter>(dtAllocNavMeshQuery());
    if (!out->m_Query || dtStatusFailed(out->m_Query->init(out->m_NavMesh, 2048))) {
        SM_WARN("NavMesh::LoadFromFile: dtNavMeshQuery::init failed");
        return nullptr;
    }
    NavQueryOwnerThread() = std::this_thread::get_id();

    return out;
}
