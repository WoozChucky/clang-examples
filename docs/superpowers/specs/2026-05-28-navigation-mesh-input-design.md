# Navigation Mesh Input — Design Spec

**Date:** 2026-05-28
**Status:** Approved
**Type:** Bug fix / functionality unlock — closes the Spec 1 Risk 1 carryover

---

## Goal

Make `Geometry=Mesh` work end-to-end. Currently entities tagged `NavMeshSourceComponent { Geometry = Mesh }` SM_WARN + skip at build time because `NavMeshBuilder::CollectTriangles` receives a `nullptr` MeshSystem from GameThread (MeshSystem lives on RenderThread, not exposed via ApplicationContext per the Spec 1 Risk 1 documented workaround).

After Spec 5 ships, any entity with MeshComponent + NavMeshSourceComponent(Geometry=Mesh) contributes its real mesh triangles to the navmesh build — terrain meshes, ramps, multi-floor geometry all become walkable surfaces.

---

## Architecture

GameThread keeps a per-mesh CPU-data cache (owned by NavMeshSystem singleton). Cache populated when GameThread drains RendererResponse from RenderThread (the existing data flow where GameThread learns the MeshHandle assigned to a freshly-loaded mesh). NavMeshBuilder consults the cache for Mesh-source entities — no MeshSystem dependency.

**No cross-thread access:** GameThread is sole writer (during response drain) and sole reader (during nav Rebuild). MeshSystem stays RenderThread-owned, unchanged. The two threads each hold an independent CPU copy of mesh data — MeshSystem for backend hot-swap (Spec 1 Task 4), NavMeshSystem for navmesh build (this spec).

**Lifecycle:** append-only, mirrors MeshSystem's append-only behavior (no RemoveMesh / ReplaceMesh exists today — see grep result in Risks below). Cache never invalidates entries.

**Memory cost:** ~2x CPU RAM per mesh. Acceptable at current scene scale. Lazy/selective caching deferred until measured pain.

---

## API surface changes

```cpp
// src/engine/src/navigation/NavMeshSystem.h — new public methods

// Append-only cache write. Takes ownership of the buffers via move.
// Called from GameThread mesh-drain section after a RendererResponse
// completes for a successfully-loaded mesh.
void StoreMeshCpuData(uint32_t meshId,
                      std::vector<MeshVertex>&& vertices,
                      std::vector<uint32_t>&& indices);

// Cache read. Returns false if meshId not in cache (cache miss — caller's
// existing SM_WARN + skip behavior is preserved). Spans valid until next
// cache mutation; readers are single-threaded.
bool GetMeshCpuData(uint32_t meshId,
                    std::span<const MeshVertex>& outVerts,
                    std::span<const uint32_t>& outIndices) const;

// New private member
struct CachedMesh {
    std::vector<MeshVertex> Vertices;
    std::vector<uint32_t>   Indices;
};
std::unordered_map<uint32_t, CachedMesh> m_MeshCpuData;
```

```cpp
// src/engine/src/navigation/NavMeshBuilder.h — signature change

// Old:
NavMeshTriangleSoup CollectTriangles(const ECS& world, const MeshSystem* meshSystem);

// New:
NavMeshTriangleSoup CollectTriangles(const ECS& world);
```

```cpp
// src/engine/src/navigation/NavMeshSystem.h — signature change

// Old:
void Rebuild(const ECS& world, const NavMeshConfigComponent& cfg, const MeshSystem* meshSystem);

// New:
void Rebuild(const ECS& world, const NavMeshConfigComponent& cfg);
```

`MeshSystem*` parameter disappears from BOTH `CollectTriangles` and `Rebuild`. No caller passes it. The Spec 1 "nullptr capture" footgun in GameThread's hook lambda goes away — there's nothing to pass.

---

## Data flow

```
1. Worker thread (existing) loads .obj/.gltf via assimp → ModelLoadResult.

2. GameThread drains ModelLoadResult → builds RequestMesh RendererCommand
   (verts/indices into staging-pool) → pushes to RendererCommandRing.

3. RenderThread drains RequestMesh → calls MeshSystem::AddMesh(verts, indices, ...)
   → MeshSystem appends entry with cpuVertices/cpuIndices retained.
   → Returns MeshHandle via RendererResponse.

4. GameThread drains RendererResponse:
   - Assigns MeshId to MeshComponent (existing, GameThread.cpp:~366).
   - NEW: cache the same verts/indices that we just shipped to RenderThread,
     BEFORE staging-pool release.
       NavMeshSystem::Instance().StoreMeshCpuData(
           meshId,
           std::vector<MeshVertex>(stagingVerts, stagingVerts + count),
           std::vector<uint32_t>(stagingIndices, stagingIndices + count));

5. NavMeshSystem::Rebuild(world, cfg) → NavMeshBuilder::CollectTriangles(world)
   - For each NavMeshSourceComponent(Geometry=Mesh) entity:
       MeshComponent has MeshId. Call:
         NavMeshSystem::Instance().GetMeshCpuData(meshId, outVerts, outIndices)
       If true → triangulate world-space triangles (existing logic, just
       reads from cache instead of MeshSystem).
       If false → SM_WARN "MeshId X has no CPU data; skipped" (unchanged).
```

---

## File changes

**Modified:**
- `src/engine/src/navigation/NavMeshSystem.h` — 2 new public methods + CachedMesh struct + m_MeshCpuData map; Rebuild signature drops MeshSystem*.
- `src/engine/src/navigation/NavMeshSystem.cpp` — 3 new method impls (Store/Get); Rebuild internal call to CollectTriangles drops the meshSystem arg.
- `src/engine/src/navigation/NavMeshBuilder.h` — CollectTriangles signature drops MeshSystem*.
- `src/engine/src/navigation/NavMeshBuilder.cpp` — drops the param; mesh-branch internally calls `NavMeshSystem::Instance().GetMeshCpuData()` instead of `meshSystem->GetMeshCpuData()`.
- `src/engine/src/threading/GameThread.cpp`:
  - Mesh-drain section (~line 366): clone verts/indices before staging-pool release, call `NavMeshSystem::Instance().StoreMeshCpuData(...)`.
  - Hook lambda for OnRebuildNavMesh: drops trailing `nullptr` arg from Rebuild call.
- `tests/test_navmesh.cpp` — sweep all 25 existing `Rebuild(w, cfg, nullptr)` calls to drop the nullptr; add T26 + T27.

**No new files. No GAME_API bump. No CMake changes. No NavServices changes.**

---

## Testing

**Existing T01-T25:** all `Rebuild(w, cfg, nullptr)` calls become `Rebuild(w, cfg)`. Trivial sweep, no behavior change. 25 tests stay green.

**New T26 — Geometry=Mesh contributes triangles when cache has the mesh:**

Manually build a triangulated 4×4 floor (2 triangles), call `StoreMeshCpuData` directly (bypassing the GameThread drain pipeline since tests don't run RenderThread), spawn an entity with MeshComponent + NavMeshSourceComponent(Geometry=Mesh), Rebuild, assert PolyCount > 0 and FindPath across the floor returns >=2 waypoints.

**New T27 — Geometry=Mesh with missing cache still SM_WARNs + skips:**

Spawn a collider floor (so navmesh isn't empty) + a Mesh-source entity whose MeshId is NOT in the cache. Rebuild. Verify Current() is non-null (collider floor built) — the Mesh-source entity is silently skipped with the existing SM_WARN. Pins the cache-miss fallback path.

---

## Risks

1. **Memory growth on long-lived editor session.** Cache is append-only; re-imports leak old entries. Current scene scale: irrelevant. If 100+ unique mesh loads per session, add refcount or LRU eviction (future spec).

2. **Cache miss race during initial load.** If a NavMeshSource(Geometry=Mesh) entity exists in world.json AND the user triggers Rebuild before the mesh-load worker has drained → cache miss → SM_WARN + skip. Subsequent Rebuild after load completes succeeds. Mitigation: editor "Rebuild NavMesh" is idempotent; clicking again after load completes works. Production trigger (world load → auto Rebuild) hits the same race. Document.

3. **Test infra populates cache directly.** Tests bypass GameThread → RendererResponse pipeline. Acceptable for unit tests. Editor GUI smoke covers the real path.

4. **CollectTriangles + Rebuild signature change.** Internal API surface. Only callers: `NavMeshSystem::Rebuild` (engine-internal — updated in same spec) and `NavMeshSystem::Rebuild`'s 26 test invocations (swept in same spec). Zero ABI risk; no game-side caller.

5. **Mesh CPU data ownership.** `StoreMeshCpuData` takes ownership via `std::vector<...>&&`. GameThread drain code must NOT use the source vectors after Store. Implementer must verify no post-Store reads of the moved-from buffers.

---

## Out-of-scope (explicit)

- **Lazy/selective caching** (only meshes referenced by NavMeshSource entities) — defer until memory concern emerges.
- **Per-mesh refcounting / LRU eviction** — defer.
- **MeshSystem RemoveMesh / ReplaceMesh** — doesn't exist today. When/if added, cache extension is trivial (new RendererResponse type + GameThread handler).
- **Editor auto-rebuild on mesh-load completion** — currently user manually clicks. Auto-fire needs a "nav dirty" signal — separate UX spec.
- **Mesh transform cache** — entity may re-position via Transform changes. NavMeshBuilder always reads current Transform; mesh data is invariant; transform applied per-rebuild. No invalidation on Transform change.
- **Multi-thread mesh data access** — single-threaded by design.
- **Bake compatibility** — the on-disk .navmesh.bin (Spec 4) is built from already-resolved triangle soup. Cache is the SOURCE of those triangles for Mesh-source entities, only relevant when Rebuild fires. If a saved scene loads a fresh bake without re-running NavMeshBuilder, cache state is irrelevant. No bake format change.

---

## Why this design (vs alternatives evaluated)

- **Alternative B (NavServices accessor reading RenderThread's MeshSystem)**: requires MeshSystem refactor — store cpuVertices/cpuIndices behind atomic-published `shared_ptr<const MeshCpuData>`. Adds async-state machinery to MeshSystem (currently no async behavior). More files touched. Avoids 2x memory but introduces real cross-DLL ABI surface. Rejected as over-engineering for current scale.

- **Alternative C (NavMeshBuilder runs on RenderThread)**: breaks the Spec 1 invariant that nav build is GameThread-local. dtNavMeshQuery thread-pin assertion would need to move. Big blast radius. Rejected.

- **Engine-internal cache (this spec)**: zero engine refactor outside navigation/, zero thread-safety code, uses existing GameThread → RendererResponse data flow. Simplest workable solution that closes the Spec 1 Risk 1 carryover.

**Lifecycle answer: MeshSystem is append-only today** (no RemoveMesh / ReplaceMesh / EraseMesh / UnloadMesh per grep of MeshSystem.h). Cache trivially mirrors that. Future MeshSystem removal API would extend the cache via the same RendererResponse channel that already exists.

---

## Commit estimate

3-4 commits:
1. NavMeshSystem cache (Store/Get + map + tests T26/T27).
2. NavMeshBuilder signature change (drop MeshSystem*; use NavMeshSystem cache).
3. NavMeshSystem::Rebuild signature change (drop MeshSystem* param) + sweep 25 test call sites.
4. GameThread wiring (mesh-drain Store call + hook lambda nullptr drop).
