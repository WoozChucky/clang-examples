# Navigation Core — Design Spec

**Date:** 2026-05-27
**Status:** Approved
**Subspec of:** Recast/Detour integration (Spec 1 of ~3)

---

## Goal

Stand up the foundation of Recast/Detour pathfinding in the engine: navmesh build pipeline from author-tagged scene geometry, a thread-safe `NavMesh` resource, a query API, an editor panel, and debug visualization. **No agents, no dynamic obstacles, no disk bake** — those are subsequent specs.

After Spec 1 ships you can: tag entities as nav sources, tune Recast knobs per scene, rebuild the navmesh from an editor button, see the resulting poly mesh in the viewport, and call `FindPath` from game code.

---

## Scope decomposition (why Spec 1 alone)

Full Recast integration carries enough surface that a single spec would be ~3x the size of anything we've shipped. Split as:

- **Spec 1 — `navigation-core` (this spec):** build pipeline, `NavMesh` resource, query API, editor panel, debug viz.
- **Spec 2 — `navigation-obstacles`:** `dtTileCache::addObstacle` wiring, `NavObstacleComponent`, sync system, `ShowObstacles` toggle, `DebugAppendCylinder` helper.
- **Spec 3 — `navigation-agents`:** `NavAgentComponent`, `NavTargetComponent`, `NavAgentSystem` (Simulation phase) writing `MoveIntent`, repath logic, `ShowNavPaths` toggle.
- **Spec 4 — `navigation-bake` (optional polish):** serialize `dtTileCache` to `.navmesh.bin` alongside `world.json`, staleness check, load fallback. Triggered only if Spec 1 rebuild times prove painful.

Spec 1 builds `dtTileCache` (not plain `dtNavMesh`) from day one so Spec 2 plugs obstacles in without rewriting the build path.

---

## Architecture overview

**Code layout:**

```
src/engine/src/navigation/
  ├── NavMesh.{h,cpp}         # wraps dtNavMesh + dtTileCache; owns the resource
  ├── NavMeshBuilder.{h,cpp}  # Recast TileCache build pipeline (triangle soup → dtTileCache)
  └── NavMeshSystem.{h,cpp}   # engine-side service: holds current navmesh, exposes build/query
src/common/include/ECS.h
  + NavMeshGeometrySource enum
  + NavMeshSourceComponent     (per-entity opt-in)
  + NavMeshConfigComponent     (singleton, persisted)
src/common/include/ComponentSerialization.h
  + NavMeshConfigComponent in Environment block round-trip
  + NavMeshSourceComponent per-entity round-trip
src/common/include/ECSCommands.h
  + NavMeshSource Apply/Remove dispatch
  + RebuildNavMeshCommand variant
src/engine/src/threading/ECSCommandProcessor.h
  + handler for RebuildNavMeshCommand
src/editor/src/rendering/imgui/panels/
  + NavigationPanel.{h,cpp}    # config knobs + Rebuild button + status
src/engine/src/rendering/passes/DebugRenderPass.cpp
  + ShowNavMesh block (poly edge lines)
src/editor/src/EditorPreferences.h
  + ShowNavMesh persistence key
tests/test_navmesh.cpp         # triangle collection + smoke build
```

**Threading model:**

- `NavMeshSystem` lives in Engine. Owns `std::shared_ptr<const NavMesh>` (atomic-published).
- **Build runs on GameThread** — it is the ECS authority. Triggered by:
  - World load (post `RebuildNavMeshCommand` at end of load)
  - `RebuildNavMeshCommand` drained from `ECSCommandRing` (editor button)
- Build is **synchronous in Spec 1** (off-thread async deferred to Spec 4 if measured >50ms hitch). Expected <100ms for current scene sizes.
- **Query (`FindPath`)** is read-only on the published `shared_ptr<const NavMesh>`. Safe from any thread (subject to the `dtNavMeshQuery` single-thread caveat below).
- **RenderThread** loads the `shared_ptr` in `DebugRenderPass` to draw poly edges. Same load-pointer-before-snapshot rule we already use for ECS.

---

## ECS components

```cpp
// Authoring intent: must be set explicitly. Unset = SM_WARN + skip at build.
enum class NavMeshGeometrySource : uint8_t {
    Unset    = 0,
    Collider = 1,  // voxelize ColliderComponent triangles
    Mesh     = 2,  // voxelize MeshComponent triangles
};

// Per-entity opt-in. Tagging an entity = it contributes triangles to the navmesh.
struct NavMeshSourceComponent {
    uint8_t AreaId = 63;                                       // RC_WALKABLE_AREA-equivalent (0-63)
    NavMeshGeometrySource Geometry = NavMeshGeometrySource::Unset;  // author must pick
};

// Singleton: one per scene, persisted in world.json Environment block.
struct NavMeshConfigComponent {
    float CellSize       = 0.3f;   // voxel XZ size (m)
    float CellHeight     = 0.2f;   // voxel Y size (m)
    float AgentRadius    = 0.5f;   // agent capsule radius (m)
    float AgentHeight    = 1.8f;   // agent capsule height (m)
    float AgentMaxClimb  = 0.4f;   // step-up height (m)
    float AgentMaxSlope  = 45.0f;  // max walkable slope (degrees)
    float TileSize       = 32.0f;  // tile XZ size (voxels per tile edge)
    int   MaxObstacles   = 128;    // dtTileCache pre-alloc (unused in Spec 1, plumbed for Spec 2)
};
```

Both registered in `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro.

`NavMeshSourceComponent` gets full plumbing: X-macro + `ECSCommands.h` (`ApplyComponentCommand` + `RemoveComponentByType`) + inspector UI (enum dropdown with "-- choose --" / "Collider" / "Mesh") + JSON serialization (per-entity).

`NavMeshConfigComponent` gets X-macro + Environment-block serialization (mirrors `FogComponent` pattern) + Navigation panel UI. No inspector entry (singleton edited via dedicated panel).

**`GAME_API_VERSION`: 14 → 15** (ECS layout change requires editor restart + game rebuild).

### Geometry-source semantics

```
NavMeshSource-tagged entity:
  Geometry = Unset     → SM_WARN("entity '{}' Geometry=Unset, skipped"); skip
  Geometry = Collider  → has ColliderComponent? voxelize collider : SM_WARN + skip
  Geometry = Mesh      → has MeshComponent?     voxelize mesh     : SM_WARN + skip
```

Author must explicitly pick `Collider` or `Mesh`. No silent fallback — the only way to "have a NavMeshSource entity that builds clean" is to assign Geometry deliberately. Surfaces authoring mistakes loudly.

**Typical usage:**
- Static environment with detailed meshes (walls, ramps, terrain) → `Mesh`
- Performance-sensitive cases (high-tri props you only need as blockers) → `Collider`
- Invisible blockers (collider only, no mesh) → `Collider` by necessity
- Special intent (oversized buffer collider for gameplay no-go area) → `Collider`

---

## Build pipeline (`NavMeshBuilder`)

**Steps, in order:**

1. **Walk ECS** for entities with `NavMeshSourceComponent`. Apply the geometry-source semantics table above.

2. **Collider triangulation helpers** (small, in `NavMeshBuilder.cpp`):
   ```cpp
   void TriangulateBox    (vec3 halfExt, mat4 worldXform, uint8_t areaId,
                           std::vector<float>& verts, std::vector<int>& tris,
                           std::vector<uint8_t>& areas);
   void TriangulateSphere (float radius, mat4 worldXform, uint8_t areaId, int segments, ...);
   void TriangulateCapsule(float radius, float halfHeight, mat4 worldXform, uint8_t areaId,
                           int segments, ...);
   ```
   Box = 12 tris. Sphere/capsule with `segments = 8` is sufficient — Recast voxelizes anyway, higher tri count gives no benefit beyond voxel resolution.

3. **Mesh triangle access** — pulls verts/indices from `MeshSystem` via `MeshHandle`.
   **Open dependency (Risk 1):** `MeshSystem` must retain CPU-side vertex/index buffers after GPU upload, or build path can't read them. Verify in Task 1 of plan; if not retained, add a `KeepCpuCopy` flag on `MeshDesc` set when a mesh is referenced by a NavMeshSource (or re-read from disk on rebuild as a slower alternative).

4. **Transform to world space** using each entity's `TransformComponent` (model matrix per source).

5. **Concatenate** all sources into single buffers: `vector<float> verts`, `vector<int> tris`, `vector<uint8_t> areas`. Compute world AABB.

6. **Configure `rcConfig`** from `NavMeshConfigComponent` (cell sizes, agent params, slope, tile size). Walkable thresholds derived per Recast docs.

7. **TileCache build** per tile within world bounds:
   - `rcCreateHeightfield` → `rcMarkWalkableTriangles` → `rcRasterizeTriangles`
   - `rcBuildCompactHeightfield` → `rcErodeWalkableArea` → `rcBuildLayerRegions`
   - `dtTileCache::buildNavMeshTile` per tile layer
   - Boilerplate is well-documented in Recast's `Sample_TempObstacles.cpp`; copy + adapt.

8. **Produce** `dtTileCache` + `dtNavMesh` + `dtNavMeshQuery` → wrap in `NavMesh` → atomic-publish via `NavMeshSystem`.

---

## Query API (`NavMesh` + `NavMeshSystem`)

```cpp
class NavMesh {
public:
    struct PathPoint { glm::vec3 Position; uint8_t AreaId; };

    // String-pulled path (dtFindStraightPath internally). Empty result = no path / out of radius.
    std::vector<PathPoint> FindPath(glm::vec3 start, glm::vec3 end,
                                    float maxSearchRadius = 50.0f) const;

    // Snap world point to closest navmesh poly. Useful for "clicked here, where on navmesh?"
    bool ClosestPoint(glm::vec3 world, glm::vec3& out) const;

    // Debug viz: collect poly outline edges as line segments (consumed by DebugRenderPass).
    void CollectPolyEdges(std::vector<glm::vec3>& outLines) const;

private:
    friend class NavMeshBuilder;
    dtTileCache*           m_TileCache = nullptr;
    dtNavMesh*             m_NavMesh   = nullptr;
    mutable dtNavMeshQuery m_Query;   // single-thread caller responsibility
    NavMeshAlloc           m_Alloc;   // RAII wrapper for Recast/Detour allocations
};

class NavMeshSystem {
public:
    static NavMeshSystem& Instance();

    // Build navmesh from current ECS state. GameThread only.
    void Rebuild(const ECS& world, const NavMeshConfigComponent& cfg);

    // Get current navmesh. Any thread. May be null before first build.
    std::shared_ptr<const NavMesh> Current() const;

private:
    std::shared_ptr<const NavMesh> m_Current;  // std::atomic_store/load
};
```

**`dtNavMeshQuery` thread-safety:** not thread-safe per Detour docs. Spec 1: one query owned by `NavMesh`, called only from GameThread. RenderThread only calls `CollectPolyEdges` (read-only poly walk). Spec 3 keeps single-query assumption (all agents tick on GameThread). Multi-threaded pathing (future) needs thread-local queries. **Debug-only assert** in `FindPath`: `SM_ASSERT(std::this_thread::get_id() == g_GameThreadId)`.

**Build trigger flow:**

- **World load:** after `WorldManager::Load` populates ECS, `game.cpp` posts `RebuildNavMeshCommand{}` into `ECSCommandRing`. Drained on next GameThread tick → `ECSCommandProcessor` calls `NavMeshSystem::Instance().Rebuild(world, *configSingleton)`.
- **Editor button:** push `RebuildNavMeshCommand{}` directly. Same drain path.
- **Single code path** for rebuild — Spec 2 (obstacle add/remove triggers rebuild) reuses it.

`ECSCommands.h`: new variant `RebuildNavMeshCommand{}` (no payload). `ECSCommandProcessor::ProcessCommands` adds dispatch case.

---

## Editor panel + debug viz

**`NavigationPanel.{h,cpp}` (new):**

```
┌─ Navigation ──────────────────────────────────┐
│ Config (NavMeshConfigComponent)                │
│   Cell Size       [0.30] m                    │
│   Cell Height     [0.20] m                    │
│   Agent Radius    [0.50] m                    │
│   Agent Height    [1.80] m                    │
│   Max Climb       [0.40] m                    │
│   Max Slope       [45.0] deg                  │
│   Tile Size       [32  ] voxels               │
│   Max Obstacles   [128 ]                      │
│                                               │
│ [Rebuild NavMesh]                             │
│                                               │
│ Status                                        │
│   Source entities: 7  (Mesh: 3, Collider: 4) │
│   Tiles built: 4                              │
│   Polys: 142                                  │
│   Last build: 32 ms                           │
│   Memory: 18 KB                               │
└───────────────────────────────────────────────┘
```

Each config knob edits the singleton via `ModifyComponent` ECSCommand (existing pattern). Rebuild button pushes `RebuildNavMeshCommand`. Status reads from `NavMeshSystem::Current()`.

**Debug viz toggle:** `ShowNavMesh` in Render Stats panel next to `ShowColliders` / `ShowGrid`. Persisted via `EditorPreferences` (additive JSON key, backward-compat — missing key defaults off).

**`DebugRenderPass.cpp` ShowNavMesh block:**
- Load `shared_ptr<const NavMesh>` from `NavMeshSystem`.
- `CollectPolyEdges` → local `vector<vec3>`.
- `DebugAppendLines` (existing helper).
- Interior poly edges: cyan. Tile/boundary edges: brighter cyan. No new debug primitive.

**Runtime (`runtime.exe`):** no panel, no overlay. Navmesh still builds (game queries it). No nav viz in runtime build.

---

## Testing

**`tests/test_navmesh.cpp` (new):**

- **T01** Empty ECS → `Rebuild` → `NavMesh` non-null, zero polys; `FindPath` returns empty vector.
- **T02** 10×10 floor box collider + `NavMeshSource(Geometry=Collider)` + default config → `Rebuild` → navmesh polys > 0; `FindPath(corner → opposite corner)` returns straight-line path of 2 points.
- **T03** Floor + 2×2 wall collider centered → `FindPath` around wall returns >2 waypoints (curved).
- **T04** `NavMeshSource(Geometry=Unset)` → `SM_WARN` logged, entity skipped, navmesh polys < T02.
- **T05** `NavMeshSource(Geometry=Mesh)` on entity with no `MeshComponent` → `SM_WARN`, skipped.
- **T06** Sphere collider tagged source → triangulated → Recast routes around it (smoke).
- **T07** `NavMeshSystem::Current()` returns identical `shared_ptr` from multiple threads (concurrent-load smoke).

**`tests/test_worldserial.cpp` additions:**

- **T20** `NavMeshConfigComponent` round-trip (write custom values to world.json Environment, read back, all fields match).
- **T21** `NavMeshSourceComponent` round-trip per-entity (AreaId + Geometry enum).
- **T22** Default `NavMeshConfigComponent` applied if Environment block lacks the key (backward compat — existing world.json without the new block loads cleanly).

---

## Risks

1. **`MeshSystem` CPU-side data retention.** Biggest unknown. If meshes drop CPU verts after GPU upload, `NavMeshBuilder` can't read them. Verify Task 1 of plan; choose retention flag or disk re-read mitigation based on what `MeshSystem` already does.

2. **Recast TileCache boilerplate (~200 lines of glue).** Follow `Sample_TempObstacles.cpp` from upstream — copy + adapt, don't write from scratch.

3. **TileCache pre-alloc constants** (`MaxObstacles`, `MaxTiles`, `MaxPolysPerTile`, `MaxVertsPerPoly`) need defaults for current scene scale. Use Recast sample defaults; document constants in `NavMeshBuilder.cpp` with rationale + when-to-increase notes.

4. **GameThread blocking build.** Estimated <100ms current scenes. If measured >50ms, move build off-thread (existing TaskQueue pattern from assimp model loads), atomic-publish on completion. Stays in Spec 1 only if necessary.

5. **Recast/Detour allocator lifetime.** TileCache uses several internal allocators (`dtTileCacheAlloc`, `dtTileCacheCompressor`, `dtTileCacheMeshProcess`). Wrap in `NavMeshAlloc` RAII so rebuild teardown is clean, no leaks on shutdown.

6. **`dtNavMeshQuery` single-thread caveat.** Debug-assert thread id in `FindPath`. Forces Spec 3 / future multi-thread paths to address explicitly.

---

## Out-of-scope (explicit deferral)

- **Dynamic obstacles** — Spec 2.
- **Agent runtime + path follow** — Spec 3.
- **Disk bake + load fallback** — Spec 4 (conditional).
- **DetourCrowd** (multi-agent avoidance) — future, when crowd density warrants.
- **Multi-agent-class navmeshes** (small dog vs human vs vehicle) — future, requires list-of-navmeshes + `AgentSizeClass`.
- **Off-mesh connections** (jumps, ladders, teleports) — future.
- **Path smoothing beyond Detour string-pulling** — agent concern (Spec 3).
- **Async / multi-threaded pathing** — stays on GameThread until proven bottleneck.

---

## File change summary

**New files:**
- `src/engine/src/navigation/NavMesh.{h,cpp}`
- `src/engine/src/navigation/NavMeshBuilder.{h,cpp}`
- `src/engine/src/navigation/NavMeshSystem.{h,cpp}`
- `src/editor/src/rendering/imgui/panels/NavigationPanel.{h,cpp}`
- `tests/test_navmesh.cpp`

**Modified:**
- `src/common/include/ECS.h` (+ enum + 2 components + X-macro entries)
- `src/common/include/ComponentSerialization.h` (Environment + per-entity)
- `src/common/include/ECSCommands.h` (NavMeshSource Apply/Remove, RebuildNavMeshCommand)
- `src/engine/src/threading/ECSCommandProcessor.h` (handler)
- `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp` (NavMeshSource UI with enum dropdown)
- `src/editor/src/EditorPreferences.h` (ShowNavMesh key)
- `src/engine/src/rendering/passes/DebugRenderPass.cpp` (ShowNavMesh block)
- `src/engine/CMakeLists.txt` (new sources + link Recast/Detour/DetourTileCache/DebugUtils)
- `src/editor/CMakeLists.txt` (NavigationPanel)
- `tests/CMakeLists.txt` (test_navmesh target)
- `src/game/include/game.h` (GAME_API_VERSION 14→15)
- `src/game/src/game.cpp` (post RebuildNavMeshCommand at end of world load)

**Commit estimate:** 6-8 commits, cadence similar to collision-v1 / movement-decoupling.
