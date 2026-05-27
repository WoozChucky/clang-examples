# Navigation Bake Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist `dtTileCache` + `dtNavMesh` state to a `world.navmesh.bin` sidecar; skip Recast runtime rebuild on unchanged scenes by loading the bake from disk.

**Architecture:** Minimal binary format (Magic + FormatVersion + BakeEpoch + WorldMtimeAtBakeTime header, raw `dtTileCacheParams`/`dtNavMeshParams`, per-tile `compressed`-blob records). Auto-bake at tail of every `NavMeshSystem::Rebuild`. World load tries disk first; falls back to existing `RebuildNavMesh` command on miss/stale/error. Staleness = stored `WorldMtimeAtBakeTime` ≠ current `fs::mtime(world.json)`.

**Tech Stack:** C++23, Recast/Detour (linked into Engine since Spec 1), `<filesystem>` for mtime + path derivation, `<chrono>` for epoch seconds. No new third-party deps.

**Spec reference:** `docs/superpowers/specs/2026-05-27-navigation-bake-design.md` (commit `d71f087`).

---

## Codebase orientation (read once before Task 1)

- **Specs 1+2+3 shipped** to main. NavMesh + NavMeshSystem + obstacle sync + agent system + 18 navmesh tests all live. Spec 4 extends NavMesh + NavMeshSystem; no new components.
- **`dtCompressedTile` layout** (`third_party/recastnavigation/DetourTileCache/Include/DetourTileCache.h:15-25`):
  ```cpp
  struct dtCompressedTile {
      unsigned int salt;
      dtTileCacheLayerHeader* header;
      unsigned char* compressed;
      int compressedSize;
      unsigned char* data;
      int dataSize;
      unsigned int flags;
      dtCompressedTile* next;
  };
  ```
  We save `compressed`/`compressedSize` — that's what `dtTileCache::addTile` takes back. `data`/`dataSize` is internal decompressed scratch, NOT to be round-tripped.
- **`dtTileCache::getTileCount()` returns `m_params.maxTiles` (capacity, not used count).** Iterate `[0, getTileCount())` and skip slots where `tile->header == nullptr` or `tile->compressed == nullptr` (empty slot).
- **`dtTileCache::addTile(data, size, DT_COMPRESSEDTILE_FREE_DATA, &ref)`** takes ownership of `data` (calls `dtFree` on dtor). Load allocates each tile buffer via `dtAlloc(size, DT_ALLOC_PERM)` then passes ownership.
- **Spec 1's `NavMesh::Build` end pattern (`NavMesh.cpp:284-290`)** is the exact code Load needs to mirror after the per-tile read loop: `buildNavMeshTilesAt(x, y, m_NavMesh)` per (tx, ty), then `dtAllocNavMeshQuery` + `init(2048)` + pin thread.
- **`dtTileCacheParams` / `dtNavMeshParams`** are pure POD primitives (no pointers) — `sizeof()` serialization safe.
- **`NavMeshSystem::Rebuild` (Spec 1+2)** clears `m_EntityToObstacle` before `atomic_store`. `TryLoadFromDisk` must do the same on success (same invariant: ref handles invalid against the new dtTileCache instance).
- **GameThread world-load block** (`src/engine/src/threading/GameThread.cpp:62-75`): posts `RebuildNavMesh` after a successful `WorldManager::LoadWorldSnapshot`. Spec 4 wraps this in `TryLoadFromDisk` first.
- **`WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH`** is the only world path today — hardcode it. RenderThread also uses it for status panel reads (no EditorContext::WorldPath field needed for v1).
- **`ECSCommandHooks` pattern from Spec 1** (callback struct on `ECSCommandProcessor::ProcessCommands`) is the precedent for `OnBakeNavMesh`. Mirror `OnRebuildNavMesh` exactly.
- **`tests/test_navmesh.cpp`** has 18 tests now. T19-T23 append after T18. `<filesystem>` already used by Spec 2's helpers — no new include block needed if the file already pulls it.
- **Test target** `test_navmesh` links `Engine` + `ecs` (Spec 2 added `${CMAKE_SOURCE_DIR}/src/game/src` to include dirs). Filesystem ops on Windows go through `std::filesystem::temp_directory_path()` which resolves to `%TEMP%`.

---

## Task 0: Create feature branch

**Files:** none (git only)

- [ ] **Step 1: Verify clean main**

```bash
git status -sb
# Expected: "## main...origin/main" — clean.
git log --oneline -3
# Expected: d71f087 (Spec 4 spec) + c0d5a65 (nav-agents merge) + earlier.
```

- [ ] **Step 2: Create branch**

```bash
git checkout -b feat/navigation-bake
git status -sb
# Expected: "## feat/navigation-bake"
```

- [ ] **Step 3: No commit yet** — administrative only.

---

## Task 1: `NavMesh::SaveToFile` + `NavMesh::LoadFromFile`

**Files:**
- Modify: `src/engine/src/navigation/NavMesh.h` (add declarations)
- Modify: `src/engine/src/navigation/NavMesh.cpp` (implementations)

- [ ] **Step 1: Add declarations to `NavMesh.h`**

Insert AFTER the existing `GetStats()` declaration (around line 80, before the Spec 2 Tick block):

```cpp
    // Save the current dtTileCache + dtNavMesh state to a binary file. Captures
    // post-Build / pre-obstacle state (obstacles are runtime ECS state reapplied
    // after load by NavObstacleSyncSystem). worldMtimeAtBake is stored in the
    // file header as the staleness signal — caller passes fs::mtime(world.json)
    // at the moment of bake. Returns false on null cache or IO error.
    bool SaveToFile(const std::string& path, uint64_t worldMtimeAtBake) const;

    // Read a previously-saved bake from disk and reconstruct a NavMesh. Outputs
    // the stored WorldMtimeAtBakeTime via outWorldMtime (caller compares to
    // current fs::mtime to decide staleness). Returns nullptr on missing file,
    // bad magic, version mismatch, IO error, or malformed data. Logs SM_WARN
    // with cause on every failure path.
    static std::unique_ptr<NavMesh> LoadFromFile(const std::string& path,
                                                 uint64_t* outWorldMtime);
```

Also add `#include <string>` to NavMesh.h includes if not already present (`std::string` parameter).

- [ ] **Step 2: Implement `SaveToFile` in `NavMesh.cpp`**

Append after the existing `GetStats()` implementation. Include `<fstream>` and `<chrono>` at the top if not already present:

```cpp
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
```

- [ ] **Step 3: Implement `LoadFromFile` in `NavMesh.cpp`**

Append right after `SaveToFile`. Reuses the existing `LinearAllocator`/`NoopCompressor`/`MeshProcess` anonymous-namespace types from Spec 1 (no duplication).

```cpp
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
    // Iterate the full tile grid since loaded tiles may be sparse across (tx, ty).
    // dtTileCache caps tx/ty by tileLayer; query getTilesAt is overkill — just iterate
    // the param grid bounds inferred from maxTiles. Simpler: walk each loaded tile and
    // call buildNavMeshTilesAt for unique (tx, ty) pairs.
    {
        const int tileCap = out->m_TileCache->getTileCount();
        // Track which (tx, ty) we've already built to avoid redundant calls.
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
```

Add `#include <unordered_set>` to NavMesh.cpp if not already present.

- [ ] **Step 4: Build to verify compile**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine --config Debug
```

Expected: clean build. No new callers yet — pure compile-test.

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/navigation/NavMesh.h src/engine/src/navigation/NavMesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): NavMesh::SaveToFile + LoadFromFile

Save flow: magic + version + bakeEpoch + worldMtimeAtBakeTime
header, raw dtTileCacheParams + dtNavMeshParams bytes, per-tile
compressedSize + compressed-buffer pairs (the same bytes dtBuild-
TileCacheLayer produces in Build). Skips empty tile slots.

Load flow: validate magic + version (FormatVersion=1), output the
stored WorldMtimeAtBakeTime so caller can decide staleness, allocate
each tile via dtAlloc + addTile with DT_COMPRESSEDTILE_FREE_DATA
(tile cache takes ownership, matches Build's lifetime contract),
buildNavMeshTilesAt per loaded (tx, ty), dtAllocNavMeshQuery + init
+ thread pin (mirrors Build's tail). SM_WARN on every failure path
naming the cause.

No callers yet; Task 3 wires SaveCurrentToDisk + TryLoadFromDisk
into NavMeshSystem."
```

---

## Task 2: `test_navmesh` T19-T22 (round-trip + version guards + missing-file)

**Files:**
- Modify: `tests/test_navmesh.cpp`

- [ ] **Step 1: Write failing tests first**

Add `#include <filesystem>` + `#include <fstream>` near the top of `tests/test_navmesh.cpp` (already present from Spec 2 — verify before duplicating).

Below T18 — `T18_agent_routes_around_obstacle` — add:

```cpp
// ---------- Spec 4: bake (T19-T22 file-format + load behavior) ----------

static std::string TempBakePath(const char* suffix = "test_navmesh_bake.bin") {
    auto tmp = std::filesystem::temp_directory_path() / suffix;
    return tmp.string();
}

static void T19_save_and_load_roundtrip_produces_equivalent_navmesh() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto built = NavMeshSystem::Instance().Current();
    EXPECT(built != nullptr);
    if (!built) return;

    const auto builtStats = built->GetStats();
    const std::string path = TempBakePath("test_navmesh_T19.bin");
    EXPECT(built->SaveToFile(path, /*worldMtime*/ 12345ULL));

    uint64_t storedMtime = 0;
    auto loaded = NavMesh::LoadFromFile(path, &storedMtime);
    EXPECT(loaded != nullptr);
    EXPECT(storedMtime == 12345ULL);
    if (!loaded) { std::filesystem::remove(path); return; }

    const auto loadedStats = loaded->GetStats();
    EXPECT(loadedStats.TilesBuilt == builtStats.TilesBuilt);
    EXPECT(loadedStats.PolyCount  == builtStats.PolyCount);
    EXPECT(loadedStats.VertCount  == builtStats.VertCount);

    // Smoke: FindPath on loaded navmesh works same as built.
    auto pathBuilt  = built->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    auto pathLoaded = loaded->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathBuilt.size() == pathLoaded.size());

    std::filesystem::remove(path);
}

static void T20_load_bad_magic_returns_null() {
    const std::string path = TempBakePath("test_navmesh_T20.bin");
    {
        std::ofstream ofs(path, std::ios::binary);
        const uint32_t badMagic = 0xDEADBEEF;
        ofs.write(reinterpret_cast<const char*>(&badMagic), 4);
    }
    uint64_t unused = 0;
    auto loaded = NavMesh::LoadFromFile(path, &unused);
    EXPECT(loaded == nullptr);
    std::filesystem::remove(path);
}

static void T21_load_bad_version_returns_null() {
    const std::string path = TempBakePath("test_navmesh_T21.bin");
    {
        std::ofstream ofs(path, std::ios::binary);
        const uint32_t kMagic = 0x484D534E;
        const uint32_t badVer = 999;
        ofs.write(reinterpret_cast<const char*>(&kMagic), 4);
        ofs.write(reinterpret_cast<const char*>(&badVer), 4);
    }
    uint64_t unused = 0;
    auto loaded = NavMesh::LoadFromFile(path, &unused);
    EXPECT(loaded == nullptr);
    std::filesystem::remove(path);
}

static void T22_load_missing_file_returns_null() {
    uint64_t unused = 0;
    auto loaded = NavMesh::LoadFromFile("Z:/definitely_does_not_exist_navmesh.bin", &unused);
    EXPECT(loaded == nullptr);
}
```

Wire into `main()` after T18:

```cpp
    T18_agent_routes_around_obstacle();
    T19_save_and_load_roundtrip_produces_equivalent_navmesh();
    T20_load_bad_magic_returns_null();
    T21_load_bad_version_returns_null();
    T22_load_missing_file_returns_null();
```

- [ ] **Step 2: Build + run**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: `All navmesh tests passed.` All 22 tests green. T20/T21 emit SM_WARN lines (intentional — they're the negative-path log messages from Task 1's LoadFromFile failure paths).

- [ ] **Step 3: Regression sweep**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_collision test_worldserial test_menu --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_collision.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
```

Expected: all pass.

- [ ] **Step 4: Commit**

```bash
git add tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "test(navigation): T19-T22 bake round-trip + version guards + missing-file

T19 round-trip: build navmesh, save with worldMtime=12345, load,
verify stored mtime matches + TilesBuilt/PolyCount/VertCount match
+ FindPath result equivalent.
T20 bad Magic: write 0xDEADBEEF magic, LoadFromFile returns null.
T21 bad FormatVersion: write valid magic + version=999, LoadFromFile
returns null.
T22 missing file: LoadFromFile on nonexistent Z:/ path returns null.

TempBakePath helper resolves via fs::temp_directory_path() so tests
don't pollute the repo. Each test removes its own file at the end."
```

---

## Task 3: `NavMeshSystem` extensions — SaveCurrentToDisk + TryLoadFromDisk + auto-bake

**Files:**
- Modify: `src/engine/src/navigation/NavMeshSystem.h`
- Modify: `src/engine/src/navigation/NavMeshSystem.cpp`

- [ ] **Step 1: Extend `NavMeshSystem.h`**

Replace the class definition to add the Spec 4 surface. Preserve all existing public + private members from Spec 1+2:

```cpp
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
```

- [ ] **Step 2: Implement extensions in `NavMeshSystem.cpp`**

Add includes near the top (after existing ones):

```cpp
#include <chrono>
#include <filesystem>
```

Add the path-derivation + mtime helpers in an anonymous namespace at top of file:

```cpp
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
    // (C++20) or the file_clock → system_clock approximation for older toolchains.
    // MSVC supports clock_cast in C++20+; project is C++23 per CLAUDE.md.
    const auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            sysTime.time_since_epoch()).count());
}

} // namespace
```

Add the new methods at the end of NavMeshSystem.cpp:

```cpp
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
```

- [ ] **Step 3: Add auto-bake call to existing `Rebuild` success path**

In `NavMeshSystem::Rebuild`, find the existing success-path `std::atomic_store(&m_Current, shared);` line (added Spec 1 Task 6). Add the auto-bake call immediately after it:

```cpp
    m_EntityToObstacle.clear();
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
```

- [ ] **Step 4: Build to verify**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: Engine + test_navmesh build clean. All 22 tests still pass (T19-T22 unaffected; the auto-bake skips silently because tests don't call SetWorldPath).

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/navigation/NavMeshSystem.h src/engine/src/navigation/NavMeshSystem.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): NavMeshSystem disk bake — SaveCurrentToDisk + TryLoadFromDisk + auto-bake

SetWorldPath/GetWorldPath let callers (WorldManager Task 6,
TryLoadFromDisk itself) record which world is loaded so the bake
sidecar can be derived. m_LastWorldPath empty disables auto-bake
(test harness path stays unaffected — tests don't call SetWorldPath).

SaveCurrentToDisk: editor 'Bake to Disk' button entry point — saves
the currently-published NavMesh without rerunning Recast.
TryLoadFromDisk: validates magic/version (delegated to LoadFromFile)
+ staleness via stored WorldMtimeAtBakeTime vs current fs::mtime;
on success clears m_EntityToObstacle (same invariant as Rebuild —
new tilecache, old obstacle refs invalid).

Rebuild success path now auto-bakes when m_LastWorldPath is set.
Uses const_cast pattern on the published shared_ptr to call the
mutating-on-disk SaveToFile via the const ptr — safe because the
write target is the filesystem, not the NavMesh object state.

DeriveBakePath = '.json' suffix → '.navmesh.bin' (plain string op).
GetFileMtimeEpoch uses std::chrono::clock_cast (C++20) to convert
file_clock → system_clock seconds for the staleness signal."
```

---

## Task 4: `test_navmesh` T23 — stale-rejection via mtime drift

**Files:**
- Modify: `tests/test_navmesh.cpp`

- [ ] **Step 1: Write failing test**

Add `<thread>` include if not present (Spec 1's T07 already pulls it).

Below T22 — `T22_load_missing_file_returns_null` — add:

```cpp
static void T23_try_load_from_disk_stale_returns_false() {
    namespace fs = std::filesystem;
    auto worldPath = fs::temp_directory_path() / "test_navmesh_T23_world.json";
    auto bakePath  = fs::temp_directory_path() / "test_navmesh_T23_world.navmesh.bin";

    // Initial world.json.
    { std::ofstream wf(worldPath); wf << "{}\n"; }

    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto built = NavMeshSystem::Instance().Current();
    EXPECT(built != nullptr);
    if (built) {
        // Save with a deliberately-stale worldMtime (much older than the file's actual mtime).
        EXPECT(built->SaveToFile(bakePath.string(), /*stale*/ 100ULL));
    }

    // Touch world.json forward so fs::mtime is well above stored 100.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    { std::ofstream wf(worldPath); wf << "{\"changed\":true}\n"; }

    const bool loaded = NavMeshSystem::Instance().TryLoadFromDisk(worldPath.string());
    EXPECT(!loaded);  // stale → returns false; caller falls back to Rebuild

    fs::remove(worldPath);
    fs::remove(bakePath);
}
```

Wire into `main()` after T22:

```cpp
    T22_load_missing_file_returns_null();
    T23_try_load_from_disk_stale_returns_false();
```

- [ ] **Step 2: Build + run**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: `All navmesh tests passed.` All 23 tests green. T23 emits SM_TRACE about "stale" — informational, not failure.

- [ ] **Step 3: Commit**

```bash
git add tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "test(navigation): T23 TryLoadFromDisk rejects stale bake

Writes world.json + saves bake with worldMtime=100 (deliberately
stale). Sleeps 50ms past mtime-truncation resolution + touches
world.json so fs::mtime advances. TryLoadFromDisk reads bake,
compares stored 100 vs current fs::mtime (well above 100),
returns false — caller falls back to Rebuild.

Pins the Spec 4 core invariant: stored WorldMtimeAtBakeTime drift
forces rebuild. Test cleans up both files at end."
```

---

## Task 5: ECSCommands — BakeNavMesh + ECSCommandHooks::OnBakeNavMesh

**Files:**
- Modify: `src/common/include/ECSCommands.h`

- [ ] **Step 1: Add BakeNavMesh enum value + factory**

Find `enum class ECSCommandType` (around line 14). Append `BakeNavMesh = 7`:

```cpp
enum class ECSCommandType : uint8_t {
    CreateEntity     = 0,
    DestroyEntity    = 1,
    AddComponent     = 2,
    RemoveComponent  = 3,
    ModifyComponent  = 4,
    DuplicateEntity  = 5,
    RebuildNavMesh   = 6,
    BakeNavMesh      = 7,
};
```

Add factory method inside the `ECSCommand` struct (alongside `RebuildNavMesh` factory):

```cpp
    static ECSCommand BakeNavMesh() {
        return ECSCommand(ECSCommandType::BakeNavMesh);
    }
```

- [ ] **Step 2: Add `OnBakeNavMesh` to `ECSCommandHooks`**

Find `struct ECSCommandHooks` (added Spec 1 Task 3, around line 168). Add a sibling field:

```cpp
struct ECSCommandHooks {
    std::function<void(ECS&)> OnRebuildNavMesh;   // optional
    std::function<void(ECS&)> OnBakeNavMesh;      // optional — Spec 4 disk bake trigger
};
```

- [ ] **Step 3: Dispatch in `ProcessCommands` switch**

Find the existing `case ECSCommandType::RebuildNavMesh:` branch (around line 225). Append a new case immediately after it (before the closing `}` of the switch):

```cpp
                case ECSCommandType::RebuildNavMesh: {
                    if (hooks.OnRebuildNavMesh) hooks.OnRebuildNavMesh(world);
                    break;
                }
                case ECSCommandType::BakeNavMesh: {
                    if (hooks.OnBakeNavMesh) hooks.OnBakeNavMesh(world);
                    break;
                }
```

- [ ] **Step 4: Build to verify**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target ecs Engine editor game --config Debug
```

Expected: clean build. Header changes propagate to all consumers.

- [ ] **Step 5: Commit**

```bash
git add src/common/include/ECSCommands.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs-cmds): BakeNavMesh command + ECSCommandHooks::OnBakeNavMesh

Mirrors the RebuildNavMesh pattern from Spec 1. Editor 'Bake to
Disk' button (Task 7) pushes BakeNavMesh through ECSCommandRing;
GameThread (Task 6) wires OnBakeNavMesh to call
NavMeshSystem::SaveCurrentToDisk on the GameThread side. Keeps
ECSCommands.h free of engine-private deps."
```

---

## Task 6: GameThread + WorldManager wiring

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp`
- Modify: `src/engine/src/utilities/WorldManager.cpp`

- [ ] **Step 1: GameThread: wire `OnBakeNavMesh` hook**

In `src/engine/src/threading/GameThread.cpp`, find the existing `ECSCommandHooks hooks;` block (Spec 1 Task 6, around line 219-225). Add a second hook lambda:

```cpp
        ECSCommandHooks hooks;
        hooks.OnRebuildNavMesh = [meshSystem = /* same capture as existing */](ECS& w) {
            const auto* cfg = w.GetSingleton<NavMeshConfigComponent>();
            NavMeshConfigComponent defaultCfg{};
            NavMeshSystem::Instance().Rebuild(w, cfg ? *cfg : defaultCfg, meshSystem);
        };
        hooks.OnBakeNavMesh = [](ECS&) {
            // Spec 4: editor 'Bake to Disk' button. Saves the currently-published
            // NavMesh without rerunning Recast. World path is set by WorldManager
            // after LoadWorldSnapshot — empty path → SM_WARN inside SaveCurrentToDisk.
            NavMeshSystem::Instance().SaveCurrentToDisk();
        };
        ECSCommandProcessor::ProcessCommands(gameState.World, m_AppContext->ECSCommandRing, hooks);
```

(Preserve the existing OnRebuildNavMesh lambda's exact body — Spec 1+2 wired the meshSystem capture; do NOT change it.)

- [ ] **Step 2: GameThread: load disk bake before posting RebuildNavMesh on world load**

Find the existing world-load success block (Spec 1 Task 9, around line 62-72):

```cpp
    if (!gameState.WorldLoaded) {
        if (WorldManager::LoadWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, &gameState.World)) {
            gameState.WorldLoaded = true;
            SM_TRACE("GameThread: default world loaded from '%s'", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
            if (!m_AppContext->ECSCommandRing.Push(ECSCommand::RebuildNavMesh())) {
                SM_WARN("GameThread: ECSCommandRing full when posting initial RebuildNavMesh");
            }
        } else {
            SM_WARN("GameThread: default world '%s' not loaded (file missing or invalid)", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
        }
    }
```

Replace the `if (!ECSCommandRing.Push(RebuildNavMesh)) {...}` line with a TryLoadFromDisk-first attempt:

```cpp
    if (!gameState.WorldLoaded) {
        if (WorldManager::LoadWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, &gameState.World)) {
            gameState.WorldLoaded = true;
            SM_TRACE("GameThread: default world loaded from '%s'", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);

            // Spec 4: try disk bake first; fall back to runtime Rebuild on miss/stale/error.
            if (!NavMeshSystem::Instance().TryLoadFromDisk(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH)) {
                if (!m_AppContext->ECSCommandRing.Push(ECSCommand::RebuildNavMesh())) {
                    SM_WARN("GameThread: ECSCommandRing full when posting initial RebuildNavMesh");
                }
            }
        } else {
            SM_WARN("GameThread: default world '%s' not loaded (file missing or invalid)", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
        }
    }
```

- [ ] **Step 3: WorldManager: call `SetWorldPath` after successful Load**

In `src/engine/src/utilities/WorldManager.cpp`, find the existing `LoadWorldSnapshot` function. At the end of the success path (right before `return true;`), add:

```cpp
#include "navigation/NavMeshSystem.h"  // top of file if not present
```

And inside `LoadWorldSnapshot`:

```cpp
    // Spec 4: tell NavMeshSystem the world path so auto-bake (in Rebuild) and
    // SaveCurrentToDisk (Bake button) can derive the sidecar path. Done at the
    // end of load so it only happens on success.
    NavMeshSystem::Instance().SetWorldPath(filepath);

    return true;
```

(Adapt the exact return statement / final lines to match the actual function structure — verify by reading the file first.)

- [ ] **Step 4: Build + run all tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine editor game test_ecs test_collision test_worldserial test_menu test_navmesh test_editorprefs --config Debug
for t in test_ecs test_collision test_worldserial test_menu test_navmesh test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
```

Expected: all pass. No regressions.

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/threading/GameThread.cpp src/engine/src/utilities/WorldManager.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): GameThread + WorldManager disk-bake wiring

WorldManager::LoadWorldSnapshot now calls NavMeshSystem::SetWorldPath
on success so the bake sidecar can be derived. Skipped on load
failure (no point baking against a path we can't open).

GameThread world-load block: TryLoadFromDisk first; only posts the
existing RebuildNavMesh command on miss/stale/error. Logs trace
either way so the cold-path decision is visible.

GameThread ECSCommandHooks gains OnBakeNavMesh wiring: calls
NavMeshSystem::Instance().SaveCurrentToDisk() — editor 'Bake to
Disk' button (Task 7) pushes the BakeNavMesh command through the
ring; this hook runs on GameThread."
```

---

## Task 7: NavigationPanel — Bake to Disk button + Status block extension

**Files:**
- Modify: `src/editor/src/rendering/imgui/NavigationPanel.cpp`

- [ ] **Step 1: Add Bake button alongside Rebuild button**

In `src/editor/src/rendering/imgui/NavigationPanel.cpp`, find the existing "Rebuild NavMesh" button (Spec 1 Task 8). Replace the single-button line with a two-button row:

```cpp
    // --- Build / Bake ---
    ImGui::Spacing();
    if (ImGui::Button("Rebuild NavMesh", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 4.0f, 0))) {
        ECSCommand cmd = ECSCommand::RebuildNavMesh();
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("NavigationPanel: ECSCommandRing full, RebuildNavMesh dropped");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Bake to Disk", ImVec2(-1, 0))) {
        ECSCommand cmd = ECSCommand::BakeNavMesh();
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("NavigationPanel: ECSCommandRing full, BakeNavMesh dropped");
        }
    }
```

(Preserve the original Rebuild button's behavior — just split the width and add the Bake sibling. The `0.5f - 4.0f` math gives the first button half the panel width minus the SameLine gap.)

- [ ] **Step 2: Extend Status block with disk-bake metadata**

Find the existing Status section (Spec 1 Task 8 — `if (!nm) { ImGui::TextUnformatted("(no navmesh built yet)"); } else { ... }`). After the existing memory line, add disk-bake fields. Add includes near the top of the file:

```cpp
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "WorldManager.h"  // DEFAULT_WORLD_SNAPSHOT_PATH (path hardcode source)
```

Helper function in the anonymous namespace at top of file:

```cpp
namespace {

// Format a uint64 epoch-seconds as "YYYY-MM-DD HH:MM:SS UTC" string. Returns
// "(invalid)" on epoch == 0 or any time-conversion error. Defensive against
// pre-1970 negative cast values (unlikely for our use case).
std::string FormatEpochUtc(uint64_t epochSeconds) {
    if (epochSeconds == 0) return "(invalid)";
    std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm tm{};
#ifdef _WIN32
    if (gmtime_s(&tm, &t) != 0) return "(invalid)";
#else
    if (!gmtime_r(&t, &tm)) return "(invalid)";
#endif
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm) == 0) {
        return "(invalid)";
    }
    return std::string(buf);
}

// Read just the bake-file header to extract BakeEpoch + WorldMtimeAtBakeTime.
// Returns false on missing file, bad magic, version mismatch, or short read.
// Status panel uses this each frame — header is 24 bytes, cheap enough for
// per-frame editor reads (cache + invalidate-on-command-observation is a v2
// optimization if profiling shows hit).
struct BakeHeaderInfo {
    bool     Exists = false;
    bool     ValidMagic = false;
    uint32_t Version = 0;
    uint64_t BakeEpoch = 0;
    uint64_t WorldMtimeAtBake = 0;
};
BakeHeaderInfo PeekBakeHeader(const std::string& bakePath) {
    BakeHeaderInfo out;
    if (!std::filesystem::exists(bakePath)) return out;
    out.Exists = true;
    std::ifstream ifs(bakePath, std::ios::binary);
    if (!ifs) return out;
    uint32_t magic = 0;
    ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!ifs.good() || magic != 0x484D534Eu) return out;
    out.ValidMagic = true;
    ifs.read(reinterpret_cast<char*>(&out.Version), sizeof(out.Version));
    ifs.read(reinterpret_cast<char*>(&out.BakeEpoch), sizeof(out.BakeEpoch));
    ifs.read(reinterpret_cast<char*>(&out.WorldMtimeAtBake), sizeof(out.WorldMtimeAtBake));
    return out;
}

uint64_t PanelFileMtimeEpoch(const std::string& path) {
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    const auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            sysTime.time_since_epoch()).count());
}

} // namespace
```

Then in the Status section, AFTER the existing `ImGui::Text("Memory: %d KB", s.MemoryKB);` line, append:

```cpp
        ImGui::Spacing();
        ImGui::SeparatorText("Disk bake");

        const std::string worldPath = WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH;
        const std::string bakePath = [&worldPath]() {
            if (worldPath.size() >= 5 && worldPath.compare(worldPath.size() - 5, 5, ".json") == 0) {
                return worldPath.substr(0, worldPath.size() - 5) + ".navmesh.bin";
            }
            return worldPath + ".navmesh.bin";
        }();

        const BakeHeaderInfo info = PeekBakeHeader(bakePath);
        const uint64_t currentWorldMtime = PanelFileMtimeEpoch(worldPath);

        ImGui::Text("Disk bake: %s", info.Exists ? "yes" : "no");
        if (info.Exists) {
            if (!info.ValidMagic) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Header: invalid magic");
            } else {
                ImGui::Text("World.json mtime: %s", FormatEpochUtc(currentWorldMtime).c_str());
                ImGui::Text("Last baked: %s", FormatEpochUtc(info.BakeEpoch).c_str());
                const bool fresh = (info.WorldMtimeAtBake == currentWorldMtime);
                if (fresh) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Fresh? yes");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "Fresh? no (stale — Rebuild will fire on next load)");
                }
            }
        }
```

- [ ] **Step 3: Build + manual smoke (controller note: smoke is the user's job)**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target editor --config Debug
```

Expected: editor builds clean. No new tests for the panel itself — UI is tested via user GUI smoke.

- [ ] **Step 4: Commit**

```bash
git add src/editor/src/rendering/imgui/NavigationPanel.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): NavigationPanel — Bake to Disk button + bake-metadata status

Rebuild NavMesh button shrunk to half-width; Bake to Disk sibling
sits on the same row. Bake pushes the new BakeNavMesh ECSCommand
(no rebuild — saves currently-published NavMesh; useful when .bin
file is missing but in-memory state is correct).

Status block extended with a 'Disk bake' subsection: file existence,
world.json current mtime, bake-header's BakeEpoch (formatted UTC),
freshness (stored WorldMtimeAtBakeTime == current fs::mtime).
Stale state gets orange text + an explanatory hint that Rebuild
will fire on next load.

PeekBakeHeader reads just the 24-byte header (cheap enough per
frame for a debug panel — cache + invalidate-on-command is a v2
optimization if profiling shows hit). FormatEpochUtc handles
gmtime_s (Windows) / gmtime_r (POSIX) via __WIN32 guard."
```

---

## Task 8: Final whole-feature review

- [ ] **Step 1: Verify clean tree + run full test sweep**

```bash
git status -sb
# Expected: clean.
cmake --build out/build/msvc-win64-vs2026-community --target Engine editor game test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs --config Debug
for t in test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "All tests green."
```

Expected: 9 test suites pass, final "All tests green." line.

- [ ] **Step 2: Dispatch final whole-feature reviewer subagent**

Per `superpowers:subagent-driven-development`, dispatch the final reviewer. Provide:
- Spec: `docs/superpowers/specs/2026-05-27-navigation-bake-design.md` (commit `d71f087`)
- Full branch diff: `git diff main..feat/navigation-bake`
- All 7 commits on the branch (one per task).
- Per-task reviews summary (all APPROVED).
- Note: T1, T3, T6 are the novel code surfaces — others are tests + UI plumbing mirroring patterns shipped in earlier specs.

Reviewer verdict (READY TO MERGE / READY WITH NOTES / NEEDS CHANGES) drives merge-readiness. User does the GUI smoke before merge.

GUI smoke checklist for the user:
1. Editor restarts clean.
2. Launch with no `.navmesh.bin` present → console traces "no bake at ..." → RebuildNavMesh fires → navmesh appears.
3. Navigation panel → Bake to Disk → file appears next to world.json → status block shows "Fresh? yes".
4. Restart editor → console traces "loaded '..navmesh.bin'" → no Rebuild log → navmesh ready faster.
5. Edit world.json externally (or in-editor save) → status panel shows "Fresh? no (stale)" within a frame.
6. Restart editor → console traces "stale (stored mtime ... != current ...)" → RebuildNavMesh fires → bake re-written.
7. Click Rebuild NavMesh → auto-bake fires inside Rebuild → status panel "Fresh? yes" again.
8. Manually corrupt the .bin (e.g., echo "xxx" > world.navmesh.bin) → restart → SM_WARN "bad magic" → RebuildNavMesh fallback fires.

---

## Self-review notes

**Spec coverage check:**

- ✅ NavMesh::SaveToFile / LoadFromFile + Magic + FormatVersion=1 + BakeEpoch + WorldMtimeAtBakeTime header — Task 1
- ✅ tcParams + nmParams raw bytes + per-tile compressedSize/compressed records — Task 1
- ✅ T19 round-trip (TilesBuilt/PolyCount/VertCount + FindPath equivalence) — Task 2
- ✅ T20 bad Magic, T21 bad FormatVersion, T22 missing file — Task 2
- ✅ NavMeshSystem::SetWorldPath + m_LastWorldPath + SaveCurrentToDisk + TryLoadFromDisk — Task 3
- ✅ DeriveBakePath + GetFileMtimeEpoch helpers (anonymous namespace) — Task 3
- ✅ Auto-bake at tail of Rebuild success path — Task 3
- ✅ T23 stale-rejection via mtime drift — Task 4
- ✅ BakeNavMesh command + OnBakeNavMesh hook + dispatch — Task 5
- ✅ GameThread OnBakeNavMesh wire + TryLoadFromDisk before RebuildNavMesh post — Task 6
- ✅ WorldManager SetWorldPath after LoadWorldSnapshot — Task 6
- ✅ NavigationPanel Bake to Disk button + Status block (disk-bake yes/no + world mtime + last baked + Fresh? indicator) — Task 7
- ✅ Final whole-feature review — Task 8

No gaps.

**Type-consistency check:**

- `kNavMeshBakeMagic = 0x484D534E` used in SaveToFile, LoadFromFile, T20 negative test (writes badMagic = 0xDEADBEEF which differs), T21 (writes valid magic), PeekBakeHeader (matches).
- `kNavMeshBakeVersion = 1` consistent across Save + Load + T21 (writes 999 which differs).
- `uint64_t worldMtimeAtBake` consistent across SaveToFile signature, LoadFromFile outparam, TryLoadFromDisk staleness check, T19/T23 test passes.
- `uint64_t BakeEpoch` consistent in header + Status panel display.
- `DeriveBakePath` semantics ('.json' suffix → '.navmesh.bin') consistent across Task 3 anonymous helper + Task 7 inline lambda in NavigationPanel.
- `NavMeshSystem::SetWorldPath` called from `WorldManager::LoadWorldSnapshot` (Task 6) and from `TryLoadFromDisk` (Task 3); both set `m_LastWorldPath`. Empty path disables auto-bake.
- `ECSCommandType::BakeNavMesh = 7` consistent across enum + factory + dispatch case.
- `OnBakeNavMesh` field name consistent on struct definition (Task 5) + GameThread wiring (Task 6).

**Placeholder scan:** No TBDs, no "implement later", no "similar to". Every code step has complete code or a verified-against-actual-codebase pattern with concrete adaptation. Two locations have honest "verify-before-paste" instructions (Task 6 Step 1 preserves the existing OnRebuildNavMesh lambda body verbatim; Task 6 Step 3 asks to verify WorldManager function structure) — both are 1-line edits with clear surrounding context.

**Commit count:** 7 commits (Tasks 1-7, Task 0 admin, Task 8 review-only). Within the spec's 5-6 estimate (slight overshoot because T2 and T4 are separate test commits per TDD).
