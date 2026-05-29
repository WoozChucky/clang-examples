# Multi-Class Navmesh Disk Bake

**Date:** 2026-05-29
**Status:** Approved, pending implementation plan

## Problem / Goal

The navmesh disk bake (`<world>.navmesh.bin`) persists a built navmesh so startup can skip the slow
Recast/Detour runtime build. It is **single-mesh only** — when the multi-class navmesh shipped, the
disk path was left single-class and `GameThread` was made to **force a runtime Rebuild whenever
`ClassCount > 1`** (so a stale single-mesh bake can't shadow a multi-class config). That means
multi-class scenes never use the disk cache and always pay the full **N×** runtime build at startup.

**Goal:** persist and reload **all N class meshes** so multi-class scenes skip the runtime rebuild —
the named follow-up to the multi-class navmesh work. Extend the bake to a multi-section container,
load all classes, and remove the `ClassCount > 1` short-circuit.

## Decisions (from brainstorming)

1. **One container file** (not N files): single `<world>.navmesh.bin` with a top header + N mesh
   sections. One staleness check, atomic, no orphan files when `ClassCount` shrinks.
2. **24-byte header preserved** so the editor's bake-status panel (`PeekBakeHeader`) keeps working
   unchanged.
3. **API split:** `NavMesh` serializes a single-mesh *section* (no file header); `NavMeshSystem` owns
   the *container* (header + N sections). The old single-file `NavMesh::SaveToFile`/`LoadFromFile`
   are **removed**.
4. **`worldMtime` is the sole staleness signal** — the class config lives in `world.json`'s
   `Environment.NavMeshConfig`, so any class/radius/count change bumps the mtime → bake stale →
   rebuild.

## Non-Goals

- No per-section independent staleness / partial reload — one container, all-or-nothing.
- No backward read of v1 (single-mesh) bakes — version bump rejects them → rebuild + rebake (breaking
  fine, early dev; see [[early-dev-breaking-ok]]).
- No change to obstacle handling — bake stays post-build/pre-obstacle; `NavObstacleSyncSystem`
  reapplies obstacles after load (fan-out + per-class dilation already handle N classes).
- No change to the navmesh build, class model, or movement.

## Background (verified)

- `NavMesh::SaveToFile(path, worldMtimeAtBake)` writes: header
  `magic(4)=0x484D534E, version(4)=kNavMeshBakeVersion(1), bakeEpoch(8), worldMtime(8)` then
  `dtTileCacheParams`, `dtNavMeshParams`, `tileCount(int)`, per-tile `dataSize(int)+blob`.
  `LoadFromFile(path,&outMtime)` reads it back, validating magic+version, reconstructing
  tilecache→navmesh→query. (`NavMesh.cpp` ~463-654.)
- `NavMeshSystem`: `Rebuild` auto-bakes via `Current(0)->SaveToFile(...)` gated `liveCount==1`
  (`NavMeshSystem.cpp:83`); `SaveCurrentToDisk()` (editor "Bake to Disk") → `Current(0)->SaveToFile`;
  `TryLoadFromDisk(worldPath)` → `NavMesh::LoadFromFile` → `PublishNavMesh(0,…)` + `m_ClassCount=1`.
  `m_Classes[kMaxNavClasses]`, `Current(uint8_t)`, `PublishNavMesh(uint8_t,…)`, `m_ClassCount` exist.
- `GameThread.cpp:70` startup: `const bool multiClass = navCfg && navCfg->ClassCount > 1;`
  `if (multiClass || !TryLoadFromDisk(...)) post RebuildNavMesh;` — the bypass to remove.
- Editor `NavigationPanel::PeekBakeHeader` reads the first 24 header bytes
  (magic/version/bakeEpoch/worldMtime) for the status block.
- `DeriveBakePath`, `GetFileMtimeEpoch` helpers in `NavMeshSystem.cpp`.

## Design

### 1. File format (container, version 2)

`<world>.navmesh.bin`:
```
magic      u32   0x484D534E
version    u32   2                      // bumped; v1 files rejected
bakeEpoch  u64   seconds since epoch     // (first 24 bytes unchanged → PeekBakeHeader still valid)
worldMtime u64   fs::mtime(world.json) at bake
classCount u8    number of sections that follow
[ section ] × classCount
```
Each **section** (single mesh, no header):
```
dtTileCacheParams   (raw POD)
dtNavMeshParams     (raw POD)
tileCount  int
[ dataSize int, tile blob (dataSize bytes) ] × tileCount
```

### 2. `NavMesh` — section read/write (`NavMesh.{h,cpp}`)

Replace the public single-file API with stream-based section helpers:
```cpp
// Serialize this mesh's tilecache state to a stream (no file header). Returns false on
// null cache or stream error. GameThread only.
bool WriteSection(std::ostream& os) const;

// Reconstruct a NavMesh from one section previously written by WriteSection. Returns
// nullptr on stream error / malformed data (logs SM_WARN). GameThread only.
static std::unique_ptr<NavMesh> ReadSection(std::istream& is);
```
Bodies are the existing `SaveToFile`/`LoadFromFile` minus the file-header read/write (the
`tcParams/nmParams/tileCount/tiles` portion + the reconstruction). `kNavMeshBakeMagic`/`Version` move
to the container (NavMeshSystem). Remove `NavMesh::SaveToFile`/`LoadFromFile`.

### 3. `NavMeshSystem` — container (`NavMeshSystem.{h,cpp}`)

- `bool SaveAllToDisk(const std::string& bakePath, uint64_t worldMtime) const` (or private helper):
  open `ofstream`; write header (`magic, version=2, bakeEpoch=now, worldMtime, m_ClassCount`); for
  `i in [0, m_ClassCount)` call `Current(i)->WriteSection(ofs)`. Returns false on null slot / IO error.
- `TryLoadFromDisk(worldPath)`: open `ifstream`; read+validate header (magic, version==2,
  `worldMtime == GetFileMtimeEpoch(worldPath)` else stale → false); read `classCount`; loop
  `NavMesh::ReadSection` × classCount (any null → fail → false, publish nothing); on full success,
  clear `m_EntityToObstacle`+`m_Obstacles`, `PublishNavMesh(i, section)` for each, clear stale slots
  `[classCount, kMaxNavClasses)`, set `m_ClassCount = classCount`, set `m_LastWorldPath`. Returns true.
- `Rebuild` auto-bake: **remove the `liveCount==1` gate** — after a successful build, if
  `!m_LastWorldPath.empty()`, `SaveAllToDisk(DeriveBakePath(m_LastWorldPath), GetFileMtimeEpoch(m_LastWorldPath))`.
- `SaveCurrentToDisk()` (editor button handler): route to `SaveAllToDisk(...)` (all live classes).
  Keep the name or rename to `SaveAllToDisk` and update the editor hook — implementation detail for
  the plan.

### 4. `GameThread` — remove the bypass (`GameThread.cpp`)

```cpp
// before:
const auto* navCfg = gameState.World.GetSingleton<NavMeshConfigComponent>();
const bool multiClass = navCfg && navCfg->ClassCount > 1;
if (multiClass || !NavMeshSystem::Instance().TryLoadFromDisk(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH)) { post Rebuild }
// after:
if (!NavMeshSystem::Instance().TryLoadFromDisk(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH)) { post Rebuild }
```
(`TryLoadFromDisk` now handles any class count; drop the `navCfg`/`multiClass` lines.)

### 5. Error handling

- Missing file → false → Rebuild (unchanged).
- Bad magic / version != 2 (incl. any v1 file) → false → Rebuild + rebake in v2.
- Truncated / `classCount` section count mismatch / any `ReadSection` null → false (publish nothing,
  keep previous) → Rebuild.
- Stale (`worldMtime` mismatch) → false → Rebuild.
- `SaveAllToDisk` with a null live slot or IO error → `SM_WARN`, return false (Rebuild still
  published the in-memory meshes; only the disk cache is skipped).

## Testing

- **Section round-trip** (`NavMesh`): build a mesh, `WriteSection` to a `std::stringstream`,
  `ReadSection` back, assert `GetStats()` matches and a `FindPath` matches.
- **Container round-trip** (`NavMeshSystem`): build 2 classes (distinct radii), `SaveAllToDisk` to a
  temp path, `TryLoadFromDisk`, assert `Current(0)`/`Current(1)` non-null, stats match the built
  meshes, and `m_ClassCount == 2`.
- **Staleness:** save with a deliberately-stale `worldMtime`, touch the world file forward,
  `TryLoadFromDisk` → false.
- **Corruption/version:** bad magic → false; a v1-style header (version 1) → false; truncated mid
  section → false; missing file → false. (Migrates today's T19–T23 to the container.)
- **Manual:** with a multi-class scene, start the editor twice — the second start loads from disk
  (no rebuild log); edit a class radius + save world → next start rebuilds (stale) and rebakes;
  `runtime.exe` loads the multi-class bake.

## Components & Boundaries

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `NavMesh::WriteSection`/`ReadSection` | One mesh ↔ a header-less byte section | Detour, std streams |
| `NavMeshSystem::SaveAllToDisk` / container load in `TryLoadFromDisk` | Container header + N sections; publish slots; staleness | NavMesh, fs |
| `GameThread` startup | Try disk for any class count, else Rebuild | NavMeshSystem |

## Files Touched

- `src/engine/src/navigation/NavMesh.{h,cpp}` — `WriteSection`/`ReadSection`; remove `SaveToFile`/`LoadFromFile`; move magic/version to the container.
- `src/engine/src/navigation/NavMeshSystem.{h,cpp}` — `SaveAllToDisk`, container read in `TryLoadFromDisk`, `Rebuild` auto-bake (gate removed), `SaveCurrentToDisk` → all classes.
- `src/engine/src/threading/GameThread.cpp` — remove the `ClassCount>1` bypass.
- `tests/test_navmesh.cpp` — rewrite the bake tests (T19–T23) for sections + the container; add the multi-class container round-trip.
- Editor: none required (24-byte header preserved). `NavigationPanel` optionally shows `classCount` later.

## Build / Reload Note

Engine + game-lib change → rebuild `ecs`/`engine`/`editor`/`game` as needed; no ECS struct change,
no `GAME_API_VERSION` bump. Existing on-disk `.navmesh.bin` files are v1 → rejected on first load →
auto-rebuilt + rebaked as v2 (no user action).
