# Navigation Bake — Design Spec

**Date:** 2026-05-27
**Status:** Approved
**Subspec of:** Recast/Detour integration (Spec 4 — final, conditional polish)
**Depends on:**
- `2026-05-27-navigation-core-design.md` (Spec 1 — NavMesh + NavMeshSystem + FindPath).
- `2026-05-27-navigation-obstacles-design.md` (Spec 2 — NavObstacleSyncSystem, re-applied after load).
- `2026-05-27-navigation-agents-design.md` (Spec 3 — NavAgentSystem, independent of bake).

---

## Goal

Skip Recast's runtime build when the on-disk bake is fresh. Editor + runtime startup loads `world.navmesh.bin` instead of rebuilding from triangle soup. Bake stores `dtTileCache` tile blobs + params + a staleness signal; load reconstructs `dtNavMesh` + `dtNavMeshQuery` exactly as `NavMesh::Build` does.

Conditional polish per Spec 1's decomposition — ships defensively rather than reactively (no measured pain today; current scenes rebuild <100ms). Provides foundation for future ship-build asset pipeline.

---

## Architecture overview

**Code layout:**

```
src/engine/src/navigation/
  ├── NavMesh.{h,cpp}             # extend: SaveToFile + LoadFromFile static factory
  └── NavMeshSystem.{h,cpp}       # extend: SaveToDisk + TryLoadFromDisk + SetWorldPath + m_LastWorldPath
src/common/include/ECSCommands.h
  + BakeNavMesh command variant + ECSCommandHooks::OnBakeNavMesh callback
src/engine/src/threading/GameThread.cpp
  + TryLoadFromDisk before falling back to existing RebuildNavMesh post
  + OnBakeNavMesh hook wires NavMeshSystem::SaveCurrentToDisk
src/engine/src/utilities/WorldManager.cpp
  + Notify NavMeshSystem of the loaded world path
src/editor/src/rendering/imgui/NavigationPanel.{h,cpp}
  + "Bake to Disk" button + Status block extension (disk-bake metadata)
tests/test_navmesh.cpp
  + T19-T23: save/load round-trip + version guards + staleness
```

**File location:** `<world.json sibling>.navmesh.bin`. For `assets/world.json` → `assets/world.navmesh.bin`. Mirrors world.json convention (single-file scene; bake is a sidecar). Existing CMake `assets/` copy step picks up the bake automatically — no build wiring change.

**Threading:**
- Save (bake): GameThread. dtTileCache lives on GameThread (Spec 1 invariant). Synchronous file write. ≪100ms expected.
- Load: GameThread. Reads file → dtAlloc/init → tile add → atomic-publish `shared_ptr<const NavMesh>` (same as Rebuild).
- Staleness check (mtime compare): GameThread, before deciding load vs rebuild.

**Trigger flow:**
- **Auto-bake** fires at the tail of every `NavMeshSystem::Rebuild` success path. No separate command needed for the normal flow — every rebuild produces a fresh disk artifact.
- **"Bake to Disk" editor button** pushes new `BakeNavMesh` command → save the currently-published NavMesh without rebuilding (sub-ms; useful when .bin file is missing but in-memory state is fine).
- **Load** fires from GameThread's world-load block. Spec 1 currently always posts `RebuildNavMesh` after load. Spec 4 changes this to: if bake exists + fresh → `LoadFromDisk`; else post `RebuildNavMesh` as before.

**Bake captures the post-Build / pre-obstacle state.** Obstacles are runtime ECS state (NavObstacleComponent). NavObstacleSyncSystem (Physics phase) re-applies them naturally after the navmesh loads. No obstacle data in the bake file.

**GAME_API_VERSION:** unchanged. No ECS layout change. Bake format has its own internal version uint32, decoupled from GAME_API.

---

## File format

```
Offset  Size  Field
------  ----  -------------------------------------------------------
0       4     uint32  Magic = 0x484D534E ('NMSH' little-endian)
4       4     uint32  FormatVersion = 1
8       8     uint64  BakeEpoch              (system_clock seconds; diagnostic — shown in panel)
16      8     uint64  WorldMtimeAtBakeTime   (fs::last_write_time(world.json) seconds at bake; STALENESS SIGNAL)
24      sizeof(dtTileCacheParams)  dtTileCacheParams (raw struct bytes)
...     sizeof(dtNavMeshParams)    dtNavMeshParams (raw struct bytes)
...     4     int     TileCount
...     ...   Per-tile records:
                4   int    CompressedSize
                N   bytes  Compressed tile data (from dtCompressedTile::data, size = dtCompressedTile::dataSize)
```

**Staleness check:** stored `WorldMtimeAtBakeTime` == current `fs::mtime(world.json)` → fresh. Mismatch → stale → fall back to Rebuild.

**Why this beats both "filesystem mtime compare" and "BakeEpoch compare":**
- Edit world.json → mtime drifts → stored snapshot no longer matches → stale (correct).
- Git clone → both files get checkout-time mtime, but the **stored** WorldMtimeAtBakeTime inside the .bin reflects whatever was baked originally. Forces deterministic stale/fresh decision — no false-fresh from mtime collisions.
- Copy .bin to another machine → file's external mtime irrelevant, only embedded value matters.

`BakeEpoch` is a separate diagnostic field shown in the Navigation panel ("Last baked: 2026-05-27 14:23 UTC") — NOT used for staleness.

**No compression.** Spec 1 uses `NoopCompressor` (memcpy). Tile bytes are already "compressed" identity. Adding real compression (LZ4) is a separate future optimization — bigger file → smaller file, no functional change.

**No standard "Recast bake format" exists** — projects roll their own glue around `dtBuildTileCacheLayer` / `addTile`. This format is the minimal viable one for our use.

---

## Save flow

**`NavMesh::SaveToFile(const std::string& path, uint64_t worldMtimeAtBakeTime) const → bool`**

Returns false on any failure (null m_TileCache, IO error). Caller logs SM_WARN.

Writes header → tcParams → nmParams → TileCount → per-tile (size + bytes). Tile bytes come from `m_TileCache->getTile(i)->{data, dataSize}` — the same blob Build originally produced via `dtBuildTileCacheLayer`. Empty tile slots write CompressedSize=0.

**Auto-bake inside `NavMeshSystem::Rebuild`** (success path tail):

```cpp
std::atomic_store(&m_Current, shared);

if (!m_LastWorldPath.empty()) {
    const std::string bakePath = DeriveBakePath(m_LastWorldPath);
    const uint64_t worldMtime = GetFileMtimeEpoch(m_LastWorldPath);
    if (!shared->SaveToFile(bakePath, worldMtime)) {
        SM_WARN("NavMeshSystem::Rebuild: failed to write disk bake to '%s'", bakePath.c_str());
    }
}
```

`m_LastWorldPath` is a new `std::string` member set by:
- `WorldManager::LoadWorldSnapshot` calling `NavMeshSystem::Instance().SetWorldPath(path)` after a successful load.
- `NavMeshSystem::TryLoadFromDisk` storing the path it loaded from.

Empty means "no world loaded yet, don't bake" — skips the bake silently.

**`DeriveBakePath(worldPath)`** = strip `.json` suffix, append `.navmesh.bin`. Plain string op, no fs dep.

**`GetFileMtimeEpoch(path)`** = `std::filesystem::last_write_time` → `std::chrono` cast to `system_clock` epoch seconds.

**`SaveCurrentToDisk()`** (for the "Bake to Disk" editor button): grabs current shared_ptr, calls SaveToFile with current world mtime. Sub-ms vs ~50ms for full Rebuild — avoids running the Recast pipeline when in-memory state is already correct.

---

## Load flow

**`NavMesh::LoadFromFile(const std::string& path, uint64_t* outWorldMtimeAtBakeTime) → std::unique_ptr<NavMesh>`**

Static factory. Returns nullptr on any error (magic mismatch, version mismatch, IO error, truncated/malformed). Logs SM_WARN naming the cause so rebuild-fallback path is diagnosable.

Reads header → validates magic + version → outputs `WorldMtimeAtBakeTime` via the pointer-arg → reads tcParams + nmParams → `dtAllocTileCache` + `init` → loops TileCount records, allocates per-tile buffer via `dtAlloc(size, DT_ALLOC_PERM)`, reads bytes, calls `m_TileCache->addTile(data, size, DT_COMPRESSEDTILE_FREE_DATA, &ref)` — same lifetime contract as Build. Final pass: `buildNavMeshTilesAt` per (tx, ty). Init dtNavMeshQuery + pin to current thread.

**`NavMeshSystem::TryLoadFromDisk(const std::string& worldPath) → bool`**

```cpp
const std::string bakePath = DeriveBakePath(worldPath);
if (!fs::exists(bakePath)) return false;  // no bake, fall back to Rebuild

uint64_t storedWorldMtime = 0;
auto fresh = NavMesh::LoadFromFile(bakePath, &storedWorldMtime);
if (!fresh) return false;  // corrupt/version/IO — already logged

if (storedWorldMtime != GetFileMtimeEpoch(worldPath)) {
    SM_TRACE("NavMeshSystem: disk bake stale (world mtime drift); will Rebuild");
    return false;  // fresh NavMesh thrown away; minor waste, edge case
}

m_EntityToObstacle.clear();    // same invariant as Rebuild
m_LastWorldPath = worldPath;
std::atomic_store(&m_Current, std::shared_ptr<const NavMesh>(std::move(fresh)));
return true;
```

**GameThread world-load wiring** (extends Spec 1 Task 9):

```cpp
if (WorldManager::LoadWorldSnapshot(WORLD_PATH, &gameState.World)) {
    gameState.WorldLoaded = true;
    SM_TRACE("GameThread: default world loaded ...");

    // Spec 4: try disk bake first; fall back to runtime Rebuild on miss/stale/error.
    if (!NavMeshSystem::Instance().TryLoadFromDisk(WORLD_PATH)) {
        if (!m_AppContext->ECSCommandRing.Push(ECSCommand::RebuildNavMesh())) {
            SM_WARN("GameThread: ECSCommandRing full when posting initial RebuildNavMesh");
        }
    }
}
```

If load succeeds → navmesh published by `TryLoadFromDisk`, no RebuildNavMesh posted. NavObstacleSyncSystem reapplies persisted NavObstacleComponent entities normally (obstacles aren't baked, they come from ECS state).

If load fails (no file, stale, error) → existing RebuildNavMesh flow fires. Spec 4 is purely additive on the cold path.

---

## Editor UI

**Navigation panel layout:**

```
┌─ Navigation ───────────────────────────────────┐
│ Config (existing knobs unchanged)              │
│                                                │
│ [Rebuild NavMesh]    [Bake to Disk]            │
│                                                │
│ Status                                         │
│   (existing: tiles / polys / verts / memory)   │
│   ─────────────────                            │
│   Disk bake: yes / no                          │
│   World.json mtime: 2026-05-27 14:23 UTC       │
│   Last baked: 2026-05-27 14:23 UTC             │
│   Fresh? ✓ / ✗ stale (forces rebuild)          │
└────────────────────────────────────────────────┘
```

**Two distinct buttons:**
- **Rebuild NavMesh** → posts `RebuildNavMesh` command → full rebuild + auto-bake (existing path, unchanged behavior + new bake tail).
- **Bake to Disk** → posts `BakeNavMesh` command → save currently-published NavMesh to disk, no rebuild. SM_WARN if no NavMesh published yet.

`BakeNavMesh` is a new `ECSCommandType` variant. Mirrors `RebuildNavMesh` pattern from Spec 1: factory `ECSCommand::BakeNavMesh()`, dispatch in ProcessCommands switch, new `ECSCommandHooks::OnBakeNavMesh` callback wired in GameThread (calls `NavMeshSystem::SaveCurrentToDisk`).

**Status fields (RenderThread, read-only file metadata + header parse):**
- "Disk bake: yes/no" — `fs::exists(bakePath)`.
- "World.json mtime" — `fs::last_write_time(worldPath)` formatted via `std::chrono` to local time.
- "Last baked" — bake header's `BakeEpoch` field, formatted same way. Requires opening the .bin file (small read).
- "Fresh?" — bake header's `WorldMtimeAtBakeTime` == current `fs::mtime(world.json)`. Stale gets orange text.

**Path source:** RenderThread doesn't know the world path. Hardcode `WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH` for v1 (single-world today). Promote to `EditorContext::WorldPath` field if/when multi-world support emerges.

**Per-frame file metadata reads:** acceptable for a debug panel. If profiling shows hit, cache + invalidate on `RebuildNavMesh` / `BakeNavMesh` command observation.

---

## Testing

**`tests/test_navmesh.cpp` new tests (T19-T23):**

- **T19** Save → Load round-trip preserves TilesBuilt/PolyCount/VertCount + FindPath equivalence.
- **T20** Load file with bad Magic returns nullptr (SM_WARN logged).
- **T21** Load file with bad FormatVersion returns nullptr.
- **T22** Load missing file returns nullptr.
- **T23** `TryLoadFromDisk` rejects when stored WorldMtime ≠ current (sleeps 50ms + touches world.json to step past mtime truncation).

Helpers added:
- `TempBakePath()` — uses `std::filesystem::temp_directory_path()` for test files; tests clean up at end.

All tests use the existing `SpawnNavBox` + `DefaultCfg` helpers from Spec 1. No new fixture infrastructure.

**Coverage rationale:** T19 = round-trip integrity (the main thing). T20/T21 = version-guard rejection. T22 = missing-file fallback. T23 = staleness comparison (the main reason Spec 4 exists).

---

## Risks

1. **dtTileCacheParams/dtNavMeshParams raw bytes.** Both are POD primitives today (verified — no pointers/handles in either). If a future Recast version adds members, FormatVersion=1 bakes become unreadable. Mitigation: pin Recast submodule SHA in CMake (already pinned at v1.6.0-367 by Spec 1), and bump FormatVersion if the struct changes. Spec 4 v1 doesn't store struct sizes — v2 could embed `sizeof(dtTileCacheParams)` for a runtime sanity check.

2. **dtCompressedTile data lifetime.** Save copies `tile->data` bytes into the file. Load reads them into `dtAlloc(size, DT_ALLOC_PERM)` and passes to `addTile` with `DT_COMPRESSEDTILE_FREE_DATA` — TileCache owns + frees on dtor. Matches Build's contract. No leak.

3. **Mtime resolution.** `std::filesystem::last_write_time` is sub-second precision on Windows; we truncate to seconds for stable serialization. Two saves within the same second appear equal (false-fresh window). Mitigation: human-paced editor saves are >>1s apart. T23 test inserts `sleep_for(50ms)` to deterministically step the mtime.

4. **Empty navmesh.** Rebuild publishes null on empty soup. Save: SaveToFile returns false on null m_TileCache, no bake written. Load: TryLoadFromDisk sees no file → returns false → falls back to Rebuild → publishes null. Idempotent.

5. **Concurrent writes.** Both auto-bake (inside Rebuild) and the Bake button (BakeNavMesh command hook) run on GameThread. No concurrent file writes possible.

6. **Staleness false positives on git pull / mass touch.** `git reset` or batch `touch` could mark a bake stale even though contents match. Rebuild is the safe fallback (~50-100ms for current scenes). Acceptable.

7. **Status panel file IO every frame.** Reading bake header to display "Last baked" each frame is small (~28 bytes) but synchronous on RenderThread. If profile-flagged, cache + invalidate on command observation. v1 ships uncached.

---

## Out-of-scope (explicit deferral)

- **Content hash staleness** — mtime chosen for simplicity. Swap to hash field later (FormatVersion bump) if false positives become painful.
- **Compressed tile bytes** (LZ4 / fastlz) — Spec 1's NoopCompressor produces raw bytes. Bake file is whatever the noop wrote. Real compression is a separate future optimization.
- **Multi-world bake support** — `DEFAULT_WORLD_SNAPSHOT_PATH` is the only world path today. SetWorldPath API already accepts arbitrary paths so extension is trivial when needed.
- **Async load** — synchronous read on GameThread. Expected <10ms file IO + ~50ms deserialize for current scenes. Move to background load + atomic-publish-on-completion if startup latency stings later.
- **Obstacle baking** — obstacles are runtime ECS state, NOT baked. By design.
- **Geometry=Mesh plumbing fix** — independent Spec 1 carryover. Tracked separately.
- **Agent path caching** (Spec 3 v2 upgrade) — independent.
- **Ship-build asset pipeline** — bake produces the right artifact, but build-time bake-via-CLI tool is a separate spec when ship build appears.

---

## File change summary

**Modified:**
- `src/engine/src/navigation/NavMesh.{h,cpp}` (+ SaveToFile + static LoadFromFile factory)
- `src/engine/src/navigation/NavMeshSystem.{h,cpp}` (+ SaveCurrentToDisk + TryLoadFromDisk + SetWorldPath + m_LastWorldPath + auto-bake in Rebuild)
- `src/common/include/ECSCommands.h` (+ BakeNavMesh command + ECSCommandHooks::OnBakeNavMesh field)
- `src/engine/src/threading/GameThread.cpp` (+ TryLoadFromDisk before RebuildNavMesh post; + OnBakeNavMesh hook wires SaveCurrentToDisk)
- `src/engine/src/utilities/WorldManager.cpp` (+ SetWorldPath call after successful LoadWorldSnapshot)
- `src/editor/src/rendering/imgui/NavigationPanel.{h,cpp}` (+ Bake to Disk button + Status block extension)
- `tests/test_navmesh.cpp` (+ T19-T23 + TempBakePath helper)

**No new files. No GAME_API bump.**

**Commit estimate:** 5-6 commits.
