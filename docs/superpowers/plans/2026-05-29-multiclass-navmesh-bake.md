# Multi-Class Navmesh Disk Bake Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist + reload all N class navmeshes in one container file so multi-class scenes skip the N×-slower runtime rebuild at startup.

**Architecture:** `NavMesh` serializes a single-mesh **section** (no file header) via stream `WriteSection`/`ReadSection`. `NavMeshSystem` owns the **container** file: a 24-byte header (magic, version, bakeEpoch, worldMtime — unchanged layout) + `classCount` + N sections. Auto-bake on every Rebuild and the editor "Bake to Disk" both write all classes; `TryLoadFromDisk` reads all classes; the `ClassCount>1` startup bypass is removed.

**Tech Stack:** C++23, Recast/Detour, std file/string streams, project nav code.

**Build/preset (project memory):** `msvc-win64-vs2026-community` only. Test binary: `out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`.

**Commit identity (project memory):** `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit …`. Never `--no-verify`. Builds slow — let them finish.

**Branch:** `feat/navmesh-multiclass-bake`.

**Breaking on-disk format (early-dev, OK):** version bumps 1→2; existing v1 `.navmesh.bin` files are rejected on load → auto-rebuilt + rebaked as v2. No user action.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/engine/src/navigation/NavMesh.{h,cpp}` | `WriteSection`/`ReadSection` (one mesh ↔ header-less byte section); old `SaveToFile`/`LoadFromFile` removed |
| `src/engine/src/navigation/NavMeshSystem.{h,cpp}` | Container `WriteBake` + container read in `TryLoadFromDisk`; Rebuild auto-bake (all classes); `SaveCurrentToDisk`; magic/version=2 |
| `src/engine/src/threading/GameThread.cpp` | Remove the `ClassCount>1` disk-bypass |
| `tests/test_navmesh.cpp` | Section round-trip; container round-trip + staleness/corruption (replaces T19–T23) |

---

## Task 1: `NavMesh` section read/write (additive)

Add the stream-based section API alongside the existing `SaveToFile`/`LoadFromFile` (removed in Task 2 once `NavMeshSystem` no longer needs them — keeping both here so the tree compiles).

**Files:**
- Modify: `src/engine/src/navigation/NavMesh.h` (declarations + `#include <iosfwd>`)
- Modify: `src/engine/src/navigation/NavMesh.cpp` (definitions)
- Test: `tests/test_navmesh.cpp`

- [ ] **Step 1: Write the failing section round-trip test in `tests/test_navmesh.cpp`**

Add `#include <sstream>` near the top includes. Add before `int main()`:
```cpp
// ---------- NavMesh section round-trip: T40 ----------
static void T40_navmesh_section_roundtrip() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());
    auto built = NavMeshSystem::Instance().Current(0);
    EXPECT(built != nullptr);
    if (!built) return;

    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT(built->WriteSection(ss));

    auto loaded = NavMesh::ReadSection(ss);
    EXPECT(loaded != nullptr);
    if (!loaded) return;
    EXPECT(loaded->GetStats().PolyCount == built->GetStats().PolyCount);
    EXPECT(loaded->GetStats().TilesBuilt == built->GetStats().TilesBuilt);
    auto pa = built->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0));
    auto pb = loaded->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0));
    EXPECT(pa.size() == pb.size());
}
```
Register in `main()`: `T40_navmesh_section_roundtrip();`

- [ ] **Step 2: Confirm fail**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: FAIL to compile — `WriteSection`/`ReadSection` not members of `NavMesh`.

- [ ] **Step 3: Declare the section API in `NavMesh.h`**

Add `#include <iosfwd>` near the top includes. Add to the public section (near `SaveToFile`/`LoadFromFile`):
```cpp
    // Serialize this mesh's tilecache state to a stream (no file header — the
    // container header is owned by NavMeshSystem). Returns false on null cache /
    // stream error. GameThread only.
    bool WriteSection(std::ostream& os) const;

    // Reconstruct a NavMesh from one section previously written by WriteSection.
    // Returns nullptr on stream error / malformed data (logs SM_WARN). GameThread only.
    static std::unique_ptr<NavMesh> ReadSection(std::istream& is);
```

- [ ] **Step 4: Define `WriteSection` in `NavMesh.cpp`**

Add `#include <ostream>` / `#include <istream>` is covered by the existing `<fstream>`. Add after `LoadFromFile` (the section body is the existing `SaveToFile` minus the header + minus opening the file):
```cpp
bool NavMesh::WriteSection(std::ostream& os) const
{
    if (!m_TileCache || !m_NavMesh) {
        SM_WARN("NavMesh::WriteSection: null tile cache / nav mesh; nothing to save");
        return false;
    }
    const dtTileCacheParams* tcp = m_TileCache->getParams();
    os.write(reinterpret_cast<const char*>(tcp), sizeof(dtTileCacheParams));
    const dtNavMeshParams* nmp = m_NavMesh->getParams();
    os.write(reinterpret_cast<const char*>(nmp), sizeof(dtNavMeshParams));

    const int tileCap = m_TileCache->getTileCount();
    int tileCount = 0;
    for (int i = 0; i < tileCap; ++i) {
        const dtCompressedTile* tile = m_TileCache->getTile(i);
        if (tile && tile->header && tile->data && tile->dataSize > 0) ++tileCount;
    }
    os.write(reinterpret_cast<const char*>(&tileCount), sizeof(tileCount));

    for (int i = 0; i < tileCap; ++i) {
        const dtCompressedTile* tile = m_TileCache->getTile(i);
        if (!tile || !tile->header || !tile->data || tile->dataSize <= 0) continue;
        os.write(reinterpret_cast<const char*>(&tile->dataSize), sizeof(int));
        os.write(reinterpret_cast<const char*>(tile->data), tile->dataSize);
    }
    return static_cast<bool>(os);
}
```

- [ ] **Step 5: Define `ReadSection` in `NavMesh.cpp`**

This is the existing `LoadFromFile` body minus the header read + minus opening the file (params → tilecache/navmesh init → tile addTile loop → buildNavMeshTilesAt dedupe → query init):
```cpp
std::unique_ptr<NavMesh> NavMesh::ReadSection(std::istream& is)
{
    auto out = std::unique_ptr<NavMesh>(new NavMesh());

    dtTileCacheParams tcParams{};
    is.read(reinterpret_cast<char*>(&tcParams), sizeof(dtTileCacheParams));
    dtNavMeshParams nmParams{};
    is.read(reinterpret_cast<char*>(&nmParams), sizeof(dtNavMeshParams));
    if (!is.good()) { SM_WARN("NavMesh::ReadSection: truncated reading params"); return nullptr; }

    out->m_TileCache = dtAllocTileCache();
    if (!out->m_TileCache) { SM_WARN("dtAllocTileCache failed"); return nullptr; }
    if (dtStatusFailed(out->m_TileCache->init(&tcParams,
                                              out->m_Alloc.Alloc,
                                              out->m_Alloc.Compressor,
                                              out->m_Alloc.MeshProc))) {
        SM_WARN("NavMesh::ReadSection: dtTileCache::init failed");
        return nullptr;
    }

    out->m_NavMesh = dtAllocNavMesh();
    if (!out->m_NavMesh) { SM_WARN("dtAllocNavMesh failed"); return nullptr; }
    if (dtStatusFailed(out->m_NavMesh->init(&nmParams))) {
        SM_WARN("NavMesh::ReadSection: dtNavMesh::init failed");
        return nullptr;
    }

    int tileCount = 0;
    is.read(reinterpret_cast<char*>(&tileCount), sizeof(int));
    if (!is.good() || tileCount < 0 || tileCount > tcParams.maxTiles) {
        SM_WARN("NavMesh::ReadSection: bad TileCount %d (max %d)", tileCount, tcParams.maxTiles);
        return nullptr;
    }

    for (int i = 0; i < tileCount; ++i) {
        int dataSize = 0;
        is.read(reinterpret_cast<char*>(&dataSize), sizeof(int));
        if (!is.good() || dataSize <= 0) {
            SM_WARN("NavMesh::ReadSection: bad tile %d size %d", i, dataSize);
            return nullptr;
        }
        unsigned char* tileData = (unsigned char*)dtAlloc(dataSize, DT_ALLOC_PERM);
        if (!tileData) {
            SM_WARN("NavMesh::ReadSection: dtAlloc(%d) failed for tile %d", dataSize, i);
            return nullptr;
        }
        is.read(reinterpret_cast<char*>(tileData), dataSize);
        if (!is.good()) {
            dtFree(tileData);
            SM_WARN("NavMesh::ReadSection: truncated reading tile %d", i);
            return nullptr;
        }
        dtCompressedTileRef ref = 0;
        if (dtStatusFailed(out->m_TileCache->addTile(tileData, dataSize,
                                                    DT_COMPRESSEDTILE_FREE_DATA, &ref))) {
            dtFree(tileData);
            SM_WARN("NavMesh::ReadSection: addTile failed for tile %d", i);
            return nullptr;
        }
    }

    {
        const int tileCap = out->m_TileCache->getTileCount();
        std::unordered_set<uint64_t> built;
        for (int i = 0; i < tileCap; ++i) {
            const dtCompressedTile* tile = out->m_TileCache->getTile(i);
            if (!tile || !tile->header) continue;
            const uint64_t key = (uint64_t(uint32_t(tile->header->tx)) << 32)
                               |  uint64_t(uint32_t(tile->header->ty));
            if (built.insert(key).second)
                out->m_TileCache->buildNavMeshTilesAt(tile->header->tx, tile->header->ty,
                                                     out->m_NavMesh);
        }
    }

    out->m_Query = std::unique_ptr<dtNavMeshQuery, NavMeshQueryDeleter>(dtAllocNavMeshQuery());
    if (!out->m_Query || dtStatusFailed(out->m_Query->init(out->m_NavMesh, 2048))) {
        SM_WARN("NavMesh::ReadSection: dtNavMeshQuery::init failed");
        return nullptr;
    }
    NavQueryOwnerThread() = std::this_thread::get_id();
    return out;
}
```
(`std::unordered_set`, `std::this_thread`, `dtAlloc`, `NavQueryOwnerThread`, `NavMeshQueryDeleter` are all already used/included in `NavMesh.cpp`.)

- [ ] **Step 6: Build + run**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.` (T40 passes; existing tests incl. T19–T23 still pass — old API untouched here).

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/navigation/NavMesh.h src/engine/src/navigation/NavMesh.cpp tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): NavMesh WriteSection/ReadSection (header-less mesh serialization)"
```

---

## Task 2: `NavMeshSystem` container cutover + remove old `NavMesh` file API

**Files:**
- Modify: `src/engine/src/navigation/NavMeshSystem.h` (declare `WriteBake`)
- Modify: `src/engine/src/navigation/NavMeshSystem.cpp` (container write/read, Rebuild gate, SaveCurrentToDisk, magic/version=2)
- Modify: `src/engine/src/navigation/NavMesh.{h,cpp}` (remove `SaveToFile`/`LoadFromFile` + their magic/version constants)
- Test: `tests/test_navmesh.cpp` (replace T19–T23 with container tests)

- [ ] **Step 1: Replace the bake tests in `tests/test_navmesh.cpp`**

Delete `T19`–`T23` (they call the removed `NavMesh::SaveToFile`/`LoadFromFile`) and their `main()` registrations. Add these container tests (they drive the public `NavMeshSystem` path) before `int main()`:
```cpp
// ---------- Multi-class container bake: T41-T45 ----------
static std::string TempWorldPath(const char* suffix) {
    return (std::filesystem::temp_directory_path() / suffix).string();
}

static void T41_container_roundtrip_multiclass() {
    namespace fs = std::filesystem;
    const std::string world = TempWorldPath("test_navbake_T41_world.json");
    const std::string bake  = TempWorldPath("test_navbake_T41_world.navmesh.bin");
    { std::ofstream wf(world); wf << "{}\n"; }

    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshConfigComponent cfg = DefaultCfg();
    cfg.ClassCount = 2;
    cfg.Classes[0] = NavClassConfig{ 0.3f, 1.8f, 0.4f };
    cfg.Classes[1] = NavClassConfig{ 0.5f, 1.8f, 0.4f };

    NavMeshSystem::Instance().SetWorldPath(world);     // enables auto-bake to <world>.navmesh.bin
    NavMeshSystem::Instance().Rebuild(w, cfg);          // builds 2 classes + auto-bakes the container
    const int poly0 = NavMeshSystem::Instance().Current(0)->GetStats().PolyCount;
    const int poly1 = NavMeshSystem::Instance().Current(1)->GetStats().PolyCount;

    const bool loaded = NavMeshSystem::Instance().TryLoadFromDisk(world);
    EXPECT(loaded);
    auto c0 = NavMeshSystem::Instance().Current(0);
    auto c1 = NavMeshSystem::Instance().Current(1);
    EXPECT(c0 && c1);
    if (c0 && c1) {
        EXPECT(c0->GetStats().PolyCount == poly0);
        EXPECT(c1->GetStats().PolyCount == poly1);
    }
    NavMeshSystem::Instance().SetWorldPath("");          // disable auto-bake for later tests
    fs::remove(world); fs::remove(bake);
}

static void T42_container_stale_returns_false() {
    namespace fs = std::filesystem;
    const std::string world = TempWorldPath("test_navbake_T42_world.json");
    const std::string bake  = TempWorldPath("test_navbake_T42_world.navmesh.bin");
    { std::ofstream wf(world); wf << "{}\n"; }

    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().SetWorldPath(world);
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());  // bakes with current mtime

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    { std::ofstream wf(world); wf << "{\"changed\":true}\n"; }  // bump mtime → stale

    EXPECT(!NavMeshSystem::Instance().TryLoadFromDisk(world));
    NavMeshSystem::Instance().SetWorldPath("");
    fs::remove(world); fs::remove(bake);
}

static void T43_container_bad_magic_returns_false() {
    namespace fs = std::filesystem;
    const std::string world = TempWorldPath("test_navbake_T43_world.json");
    const std::string bake  = TempWorldPath("test_navbake_T43_world.navmesh.bin");
    { std::ofstream wf(world); wf << "{}\n"; }
    { std::ofstream bf(bake, std::ios::binary); const uint32_t bad = 0xDEADBEEF; bf.write((const char*)&bad, 4); }
    EXPECT(!NavMeshSystem::Instance().TryLoadFromDisk(world));
    fs::remove(world); fs::remove(bake);
}

static void T44_container_bad_version_returns_false() {
    namespace fs = std::filesystem;
    const std::string world = TempWorldPath("test_navbake_T44_world.json");
    const std::string bake  = TempWorldPath("test_navbake_T44_world.navmesh.bin");
    { std::ofstream wf(world); wf << "{}\n"; }
    { std::ofstream bf(bake, std::ios::binary);
      const uint32_t magic = 0x484D534E; const uint32_t ver = 1;   // old v1 → rejected
      bf.write((const char*)&magic, 4); bf.write((const char*)&ver, 4); }
    EXPECT(!NavMeshSystem::Instance().TryLoadFromDisk(world));
    fs::remove(world); fs::remove(bake);
}

static void T45_container_missing_returns_false() {
    EXPECT(!NavMeshSystem::Instance().TryLoadFromDisk("Z:/definitely_missing_world.json"));
}
```
Update `main()`: remove `T19_…();`…`T23_…();`, add:
```cpp
    T41_container_roundtrip_multiclass();
    T42_container_stale_returns_false();
    T43_container_bad_magic_returns_false();
    T44_container_bad_version_returns_false();
    T45_container_missing_returns_false();
```

- [ ] **Step 2: Confirm fail**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: still compiles + passes for now (T41–T45 use existing APIs that currently behave single-mesh — T41's multi-class assertions on `Current(1)` may fail at runtime because `TryLoadFromDisk` is still single-mesh). The point of this step is to see T41 FAIL on the multi-class load (`Current(1)` null or stale) before the cutover. If it compiles, run it and expect T41 to fail.

- [ ] **Step 3: Add `WriteBake` + version=2 in `NavMeshSystem`**

In `NavMeshSystem.h`, add a private helper:
```cpp
    // Write the full multi-class container (header + N sections) to bakePath.
    bool WriteBake(const std::string& bakePath, uint64_t worldMtime) const;
```
Ensure `<fstream>`/`<chrono>` are included in `NavMeshSystem.cpp` (add if missing). At the top of `NavMeshSystem.cpp`'s anonymous namespace add:
```cpp
constexpr uint32_t kNavMeshBakeMagic   = 0x484D534E;  // 'NMSH'
constexpr uint32_t kNavMeshBakeVersion = 2;           // v2 = multi-class container
```
Implement `WriteBake`:
```cpp
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
```

- [ ] **Step 4: Rewrite `TryLoadFromDisk` for the container, `Rebuild` auto-bake, `SaveCurrentToDisk`**

Replace the body of `TryLoadFromDisk` with:
```cpp
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
```
In `Rebuild`, replace the `liveCount == 1` auto-bake block with:
```cpp
    // Auto-bake the full multi-class container so subsequent startups skip Rebuild.
    // Skips silently when no world path is known (e.g. test harness without SetWorldPath).
    if (!m_LastWorldPath.empty()) {
        const std::string bakePath = DeriveBakePath(m_LastWorldPath);
        if (!WriteBake(bakePath, GetFileMtimeEpoch(m_LastWorldPath)))
            SM_WARN("NavMeshSystem::Rebuild: failed to write disk bake to '%s'", bakePath.c_str());
    }
```
Replace `SaveCurrentToDisk` with:
```cpp
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
```
(`<array>` is already included for `m_Classes`.)

- [ ] **Step 5: Remove the old `NavMesh::SaveToFile`/`LoadFromFile`**

In `NavMesh.h` delete the `SaveToFile` + `LoadFromFile` declarations (the `WriteSection`/`ReadSection` from Task 1 replace them). In `NavMesh.cpp` delete the `SaveToFile` + `LoadFromFile` definitions and the now-unused `kNavMeshBakeMagic`/`kNavMeshBakeVersion` anonymous-namespace constants (they live in `NavMeshSystem.cpp` now). Leave `WriteSection`/`ReadSection`.

- [ ] **Step 6: Build + run**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.` — T41 (multi-class round-trip) now passes; T42–T45 (stale/magic/version/missing) pass; T40 (section) still passes; all earlier tests pass. If T41's `PolyCount` equality is off, print the values and confirm the reload reproduces the build (do NOT loosen — a mismatch means the section round-trip is lossy).

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/navigation/NavMeshSystem.h src/engine/src/navigation/NavMeshSystem.cpp src/engine/src/navigation/NavMesh.h src/engine/src/navigation/NavMesh.cpp tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): multi-class container bake (SaveAllToDisk/TryLoadFromDisk over N sections, v2)"
```

---

## Task 3: Remove the `ClassCount>1` startup bypass

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp` (~line 70)

- [ ] **Step 1: Drop the multi-class bypass**

Replace:
```cpp
            const auto* navCfg = gameState.World.GetSingleton<NavMeshConfigComponent>();
            const bool multiClass = navCfg && navCfg->ClassCount > 1;
            if (multiClass || !NavMeshSystem::Instance().TryLoadFromDisk(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH)) {
                if (!m_AppContext->ECSCommandRing.Push(ECSCommand::RebuildNavMesh())) {
                    SM_WARN("GameThread: ECSCommandRing full when posting initial RebuildNavMesh");
                }
            }
```
with:
```cpp
            // TryLoadFromDisk now handles any class count (multi-class container bake);
            // fall back to a runtime Rebuild on miss / stale / corrupt.
            if (!NavMeshSystem::Instance().TryLoadFromDisk(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH)) {
                if (!m_AppContext->ECSCommandRing.Push(ECSCommand::RebuildNavMesh())) {
                    SM_WARN("GameThread: ECSCommandRing full when posting initial RebuildNavMesh");
                }
            }
```

- [ ] **Step 2: Build**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds clean (this pulls the engine `GameThread` change). If `NavMeshConfigComponent` becomes unused in this scope, no harm; remove a now-dead include only if the compiler warns.

- [ ] **Step 3: Commit**

```bash
git add src/engine/src/threading/GameThread.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): use disk bake for any class count (drop ClassCount>1 bypass)"
```

---

## Task 4: End-to-end manual verification

No code.

- [ ] **Step 1: Full rebuild + restart editor** — `cmake --build --preset msvc-win64-vs2026-community`, restart `editor.exe`.
- [ ] **Step 2: First start** with a multi-class scene → console logs a runtime Rebuild (old v1 bake rejected) and writes the v2 container.
- [ ] **Step 3: Second start** (no edits) → console logs `TryLoadFromDisk: loaded … (N classes)` and **no** Rebuild; play and confirm all classes behave (player + a class-1 entity both constrained/ground-snapped correctly).
- [ ] **Step 4: Stale path** — edit a class radius in the Navigation panel + Save World → next start rebuilds (mtime bumped) and rebakes. The editor bake-status panel still shows the header (Fresh? yes/no).
- [ ] **Step 5: `runtime.exe`** on the same world → loads the multi-class bake (no rebuild).
- [ ] **Step 6 (if a fix was needed): commit it.**

```bash
git add -A
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "fix(nav): <describe bake fix>"
```

---

## Self-Review (completed during authoring)

- **Spec coverage:** §1 container format + 24-byte header preserved (Task 2 WriteBake/TryLoadFromDisk); §2 NavMesh section API (Task 1); §3 NavMeshSystem container + Rebuild auto-bake + SaveCurrentToDisk (Task 2); §4 GameThread bypass removal (Task 3); §5 error handling (magic/version/truncation/stale/missing — Task 2 tests + reader); §6 testing (T40 section, T41–T45 container); migration of v1→reject (version=2). All mapped.
- **Build-green ordering:** Task 1 is additive (old API kept) → compiles. Task 2 cuts over NavMeshSystem to sections THEN removes the old API in the same task (tests migrated in the same task) → compiles. Task 3 is the GameThread one-liner.
- **Type consistency:** `WriteSection(std::ostream&) const`, `static ReadSection(std::istream&)`, `WriteBake(path, worldMtime) const`, `Current(uint8_t)`, `PublishNavMesh(uint8_t,…)`, `m_ClassCount`, `kNavMeshBakeMagic/Version(2)` consistent across tasks. Header magic/version moved NavMesh→NavMeshSystem (removed from NavMesh in Task 2 Step 5).
- **Placeholders:** none — full bodies + exact commands.
- **Test-singleton hygiene:** container tests `SetWorldPath("")` after themselves so the shared `NavMeshSystem` singleton doesn't auto-bake to a temp path during later tests.
