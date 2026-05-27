# Navigation Mesh Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the Spec 1 Risk 1 carryover — make `NavMeshSourceComponent(Geometry=Mesh)` actually contribute triangles to the navmesh instead of SM_WARN+skipping.

**Architecture:** NavMeshSystem singleton gains a per-MeshId CPU-data cache. GameThread populates it via a two-stage flow: stash verts/indices in a ticketId-keyed pending map when the RequestMesh command is sent to RenderThread, then transfer to the MeshId-keyed final cache when the MeshUpload response arrives. NavMeshBuilder reads the cache directly (no MeshSystem* parameter). All access is single-threaded GameThread.

**Tech Stack:** C++23, std::unordered_map, std::vector + std::span, existing GameThread/RenderThread response channel. No new third-party deps.

**Spec reference:** `docs/superpowers/specs/2026-05-28-navigation-mesh-input-design.md` (commit `0dfe370`).

---

## Codebase orientation (read once before Task 1)

- **Spec 1 Risk 1 origin:** `NavMeshBuilder::CollectTriangles` takes `const MeshSystem*`. GameThread's `OnRebuildNavMesh` hook lambda (`GameThread.cpp:228-232`) captures `nullptr` because MeshSystem is RenderThread-owned and unreachable from GameThread. Result: every Geometry=Mesh entity SM_WARN+skips at `NavMeshBuilder.cpp:~178` ("no MeshSystem available").
- **GameThread mesh-drain pipeline** lives in `src/engine/src/threading/GameThread.cpp`:
  - **Mesh upload command construction** at line ~280-330: pops `ModelLoadResult` from local queue, copies verts/indices into staging-pool, pushes `RequestMesh` command to `GRCommandRing`. After successful push: `res.MeshUploaded = true` then `res` falls out of for-iteration scope → `res.vertices` and `res.indices` are destroyed.
  - **Mesh upload response handler** at line ~353-373: pops `RendererResponse` from `RGCommandRing`, the `MeshUpload` case assigns `response.Mesh.Handle.Index` to `MeshComponent::MeshId` on the entity matching `response.TicketId`.
  - **The two sites are separated in time** — upload happens this tick; response may come this tick or several ticks later (RenderThread schedules its own work). So GameThread cannot piggyback the cache write at the upload site (no MeshId yet) OR the response site (no verts/indices anymore).
- **Solution:** function-scope `std::unordered_map<EntityId, PendingMeshData> pendingMeshData` in `RunLoop`. Mesh-drain section inserts BEFORE `res` dies; response-drain section moves into NavMeshSystem cache when MeshId arrives, erases pending entry.
- **NavMeshSystem singleton** at `src/engine/src/navigation/NavMeshSystem.{h,cpp}`. Already has `m_LastWorldPath`, `m_EntityToObstacle`, etc. — adding `m_MeshCpuData` map fits the established pattern. New methods are GameThread-only callers (same contract as obstacle methods).
- **`NavMeshBuilder::CollectTriangles` Mesh-branch** (`NavMeshBuilder.cpp:~163-188`): currently checks `meshSystem != nullptr`, calls `meshSystem->GetMeshCpuData(mc->MeshId)`. Replace with `NavMeshSystem::Instance().GetMeshCpuData(mc->MeshId, ...)`. SM_WARN messages on cache miss stay identical-modulo-source.
- **`NavMeshSystem::Rebuild` signature has 3 params today**: `(const ECS&, const NavMeshConfigComponent&, const MeshSystem*)`. Drop the third. Single caller in production (`GameThread.cpp:228-232` hook lambda) — passes `nullptr`. 13 callers in test_navmesh.cpp all pass `nullptr`. Sweep all 14.
- **test_navmesh.cpp Rebuild sweep:** `grep -c "Rebuild(w, DefaultCfg(), nullptr)" tests/test_navmesh.cpp` returns 13. Replace each with `Rebuild(w, DefaultCfg())`. Plus T26 (new test) + T27 (new test) get the new signature naturally.
- **MeshVertex layout** (`src/common/include/ApplicationContext.h:57-62`): `{ float px, py, pz; float nx, ny, nz; float u, v; }`. Triangulated test data in T26 must use these field names (NOT `position[3]`).
- **MeshComponent layout** (`src/common/include/ECS.h:58-61`): `{ uint32_t MeshId = 0; bool Visible = false; }`. T26 sets both.
- **Single test infra precedent:** test_navmesh tests already call `NavMeshSystem::Instance().Rebuild(...)` directly (engine-side tests). Same pattern for `StoreMeshCpuData(...)` in T26.

---

## Task 0: Create feature branch

**Files:** none (git only)

- [ ] **Step 1: Verify clean main**

```bash
git status -sb
# Expected: "## main...origin/main" — clean (.claude/ untracked OK).
git log --oneline -3
# Expected: 0dfe370 (mesh-input design spec) + 38041a0 (navservices-decoupling merge) + earlier.
```

- [ ] **Step 2: Create branch**

```bash
git checkout -b feat/navigation-mesh-input
git status -sb
# Expected: "## feat/navigation-mesh-input"
```

- [ ] **Step 3: No commit yet** — administrative only.

---

## Task 1: NavMeshSystem mesh-data cache (Store + Get + map)

**Files:**
- Modify: `src/engine/src/navigation/NavMeshSystem.h`
- Modify: `src/engine/src/navigation/NavMeshSystem.cpp`

- [ ] **Step 1: Add public method declarations + private map to `NavMeshSystem.h`**

Add includes near the top (after existing `<unordered_map>`):

```cpp
#include <span>
#include <vector>

#include "ApplicationContext.h"   // MeshVertex
```

Inside `class NavMeshSystem` public section, AFTER the existing `SetWorldPath`/`GetWorldPath`/`SaveCurrentToDisk`/`TryLoadFromDisk` block:

```cpp
    // ---- Mesh CPU-data cache (Spec 5: navigation-mesh-input) ----

    // Append-only cache populated by GameThread when a MeshUpload response
    // arrives from RenderThread. Mirrors MeshSystem's append-only behavior —
    // entries are never removed (MeshSystem has no RemoveMesh today). Takes
    // ownership of the buffers via move. GameThread only.
    void StoreMeshCpuData(uint32_t meshId,
                          std::vector<MeshVertex>&& vertices,
                          std::vector<uint32_t>&& indices);

    // Cache lookup for NavMeshBuilder's Mesh-source branch. Returns false on
    // cache miss (caller falls back to its existing SM_WARN + skip path).
    // Spans valid until the next cache mutation; readers are single-threaded
    // GameThread per the NavMeshSystem contract.
    bool GetMeshCpuData(uint32_t meshId,
                        std::span<const MeshVertex>& outVerts,
                        std::span<const uint32_t>& outIndices) const;
```

In the `private:` section, after `m_LastWorldPath`:

```cpp
    struct CachedMesh {
        std::vector<MeshVertex> Vertices;
        std::vector<uint32_t>   Indices;
    };
    std::unordered_map<uint32_t, CachedMesh> m_MeshCpuData;
```

- [ ] **Step 2: Implement the two methods in `NavMeshSystem.cpp`**

Append at the end of the file:

```cpp
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
```

- [ ] **Step 3: Build**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine --config Debug
```

Expected: clean build. New methods compile; no callers yet (Task 3 wires them up).

- [ ] **Step 4: Commit**

```bash
git add src/engine/src/navigation/NavMeshSystem.h src/engine/src/navigation/NavMeshSystem.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): NavMeshSystem mesh CPU-data cache

Per-MeshId cache populated by GameThread (Task 4) when MeshUpload
response arrives from RenderThread. Append-only — mirrors
MeshSystem's append-only behavior (no RemoveMesh exists today).
GameThread sole writer + reader.

NavMeshBuilder (Task 3) consults the cache for Geometry=Mesh
entities via GetMeshCpuData instead of receiving a MeshSystem*
parameter. Closes the Spec 1 Risk 1 footgun where the lambda in
GameThread captured nullptr for MeshSystem.

Spans returned by GetMeshCpuData are valid until next cache
mutation; single-threaded read/write means no race window."
```

---

## Task 2: test_navmesh T26 + T27 (cache populated + cache miss)

**Files:**
- Modify: `tests/test_navmesh.cpp`

- [ ] **Step 1: Write failing tests first**

After the existing T25 (`T25_navservices_findpath_empty_when_no_mesh`), append:

```cpp
// ---------- Navigation Mesh Input: T26-T27 (Spec 5) ----------

static void T26_geometry_mesh_uses_cached_cpu_data() {
    ECS w;

    // Manually triangulated 4x4 floor (square at y=0, two triangles).
    // Field names match MeshVertex layout: px,py,pz, nx,ny,nz, u,v.
    std::vector<MeshVertex> verts = {
        {-2.0f, 0.0f, -2.0f,  0,0,1,  0,0},
        { 2.0f, 0.0f, -2.0f,  0,0,1,  1,0},
        { 2.0f, 0.0f,  2.0f,  0,0,1,  1,1},
        {-2.0f, 0.0f,  2.0f,  0,0,1,  0,1},
    };
    std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };

    constexpr uint32_t kMeshId = 42;
    NavMeshSystem::Instance().StoreMeshCpuData(kMeshId, std::move(verts), std::move(indices));

    // Entity references the cached mesh via MeshComponent + NavMeshSource(Geometry=Mesh).
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ glm::vec3(0,0,0), glm::vec3(0), glm::vec3(1) });
    MeshComponent mc{};
    mc.MeshId = kMeshId;
    mc.Visible = true;
    w.AddComponent(e, mc);
    NavMeshSourceComponent src{};
    src.AreaId = 63;
    src.Geometry = NavMeshGeometrySource::Mesh;
    w.AddComponent(e, src);

    // Rebuild (no MeshSystem* — NavMeshBuilder consults the cache).
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());

    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (!nm) return;
    EXPECT(nm->GetStats().PolyCount > 0);  // floor became walkable navmesh

    // FindPath across the floor returns 2-waypoint straight line.
    auto path = nm->FindPath(glm::vec3(-1.5f, 0.5f, 0), glm::vec3(1.5f, 0.5f, 0));
    EXPECT(path.size() >= 2);
}

static void T27_geometry_mesh_cache_miss_skips_entity() {
    ECS w;
    // Spawn a collider floor so navmesh isn't empty.
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));

    // Add a Mesh-source entity whose MeshId is NOT in the cache.
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ glm::vec3(2, 0.5f, 0), glm::vec3(0), glm::vec3(1) });
    MeshComponent mc{};
    mc.MeshId = 999;  // never stored in cache
    mc.Visible = true;
    w.AddComponent(e, mc);
    NavMeshSourceComponent src{};
    src.Geometry = NavMeshGeometrySource::Mesh;
    w.AddComponent(e, src);

    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());

    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);  // floor still built; Mesh-source skipped + SM_WARN
}
```

Wire into `main()` after T25:

```cpp
    T25_navservices_findpath_empty_when_no_mesh();
    T26_geometry_mesh_uses_cached_cpu_data();
    T27_geometry_mesh_cache_miss_skips_entity();
```

- [ ] **Step 2: Run tests — expect compile failure**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh --config Debug
```

Expected: FAIL with `NavMeshSystem::Rebuild` takes 3 args, not 2 (T26/T27 call the 2-arg form which doesn't exist yet). Drives Task 3.

- [ ] **Step 3: Commit (failing tests first per TDD)**

```bash
git add tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "test(navigation): T26+T27 Geometry=Mesh end-to-end (failing — drives Task 3)

T26: manually populate NavMeshSystem cache with a 4x4 floor (2 tris),
spawn entity with MeshComponent + NavMeshSource(Geometry=Mesh),
Rebuild, assert PolyCount > 0 + FindPath returns straight path.

T27: cache miss (MeshId not stored) on a Mesh-source entity sits
alongside a collider floor; Rebuild succeeds (floor built), Mesh
entity silently skipped with SM_WARN (existing fallback preserved).

Both tests call Rebuild with 2 args (NavMeshSystem::Rebuild
signature changes in Task 3 to drop the MeshSystem* parameter).
Currently fail to compile — intentional TDD red phase."
```

---

## Task 3: Drop MeshSystem* parameter from CollectTriangles + Rebuild; sweep all callers

**Files:**
- Modify: `src/engine/src/navigation/NavMeshBuilder.h`
- Modify: `src/engine/src/navigation/NavMeshBuilder.cpp`
- Modify: `src/engine/src/navigation/NavMeshSystem.h`
- Modify: `src/engine/src/navigation/NavMeshSystem.cpp`
- Modify: `tests/test_navmesh.cpp` (sweep 13 existing `Rebuild(w, cfg, nullptr)` callers)
- Modify: `src/engine/src/threading/GameThread.cpp` (sweep the OnRebuildNavMesh hook lambda's Rebuild call — drop nullptr arg)

- [ ] **Step 1: Update `NavMeshBuilder.h` signature**

In `src/engine/src/navigation/NavMeshBuilder.h`, change the `CollectTriangles` declaration:

```cpp
// Old:
NavMeshTriangleSoup CollectTriangles(const ECS& world, const MeshSystem* meshSystem);

// New:
NavMeshTriangleSoup CollectTriangles(const ECS& world);
```

Also drop `class MeshSystem;` forward declaration if it appears at the top of the file (no longer needed by the public signature).

- [ ] **Step 2: Update `NavMeshBuilder.cpp` implementation**

In `src/engine/src/navigation/NavMeshBuilder.cpp`:

1. Drop `#include "MeshSystem.h"` if present (no longer needed).
2. Add `#include "navigation/NavMeshSystem.h"` if not already present (for the GetMeshCpuData call).
3. Change the function signature:

```cpp
// Old:
NavMeshTriangleSoup CollectTriangles(const ECS& world, const MeshSystem* meshSystem)

// New:
NavMeshTriangleSoup CollectTriangles(const ECS& world)
```

4. Inside the function, find the Mesh-branch (around line 174-200). Replace the `meshSystem`-based mesh-data lookup. Current code reads roughly:

```cpp
} else { // NavMeshGeometrySource::Mesh
    const auto* mc = world.GetComponent<MeshComponent>(e);
    if (!mc) {
        SM_WARN("NavMeshSource entity %llu Geometry=Mesh but no MeshComponent; skipped", e);
        return;
    }
    if (!meshSystem) {
        SM_WARN("NavMeshSource entity %llu Geometry=Mesh but no MeshSystem available; skipped", e);
        return;
    }
    const auto cpu = meshSystem->GetMeshCpuData(mc->MeshId);
    if (!cpu.valid) {
        SM_WARN("NavMeshSource entity %llu MeshId %u has no CPU data; skipped", e, mc->MeshId);
        return;
    }
    // ... triangulation loop using cpu.vertices / cpu.indices ...
}
```

Replace with:

```cpp
} else { // NavMeshGeometrySource::Mesh
    const auto* mc = world.GetComponent<MeshComponent>(e);
    if (!mc) {
        SM_WARN("NavMeshSource entity %llu Geometry=Mesh but no MeshComponent; skipped", e);
        return;
    }
    std::span<const MeshVertex> meshVerts;
    std::span<const uint32_t>   meshIndices;
    if (!NavMeshSystem::Instance().GetMeshCpuData(mc->MeshId, meshVerts, meshIndices)) {
        SM_WARN("NavMeshSource entity %llu MeshId %u has no CPU data; skipped", e, mc->MeshId);
        return;
    }
    // ... triangulation loop using meshVerts / meshIndices ...
}
```

The "no MeshSystem available" SM_WARN goes away (the cache lookup replaces it). The "no MeshComponent" and "no CPU data" SM_WARNs stay — same fallback semantics.

The triangulation loop inside uses `cpu.vertices` / `cpu.indices` — rename to `meshVerts` / `meshIndices` to match the new locals. Existing access is `v.px, v.py, v.pz` (per the MeshVertex layout) — unchanged.

Add `#include <span>` at the top of NavMeshBuilder.cpp if not already present.

- [ ] **Step 3: Update `NavMeshSystem.h` Rebuild signature**

In `src/engine/src/navigation/NavMeshSystem.h`:

```cpp
// Old:
void Rebuild(const ECS& world, const NavMeshConfigComponent& cfg, const MeshSystem* meshSystem);

// New:
void Rebuild(const ECS& world, const NavMeshConfigComponent& cfg);
```

Drop the `class MeshSystem;` forward declaration if it's at the top of the file.

- [ ] **Step 4: Update `NavMeshSystem.cpp` Rebuild implementation**

In `src/engine/src/navigation/NavMeshSystem.cpp`, change the function signature + the internal `CollectTriangles` call:

```cpp
// Old:
void NavMeshSystem::Rebuild(const ECS& world,
                            const NavMeshConfigComponent& cfg,
                            const MeshSystem* meshSystem)
{
    NavMeshTriangleSoup soup = NavMeshBuilder::CollectTriangles(world, meshSystem);
    // ... rest unchanged ...
}

// New:
void NavMeshSystem::Rebuild(const ECS& world,
                            const NavMeshConfigComponent& cfg)
{
    NavMeshTriangleSoup soup = NavMeshBuilder::CollectTriangles(world);
    // ... rest unchanged ...
}
```

Drop `#include "MeshSystem.h"` from NavMeshSystem.cpp if present and unused after this change.

- [ ] **Step 5: Sweep `tests/test_navmesh.cpp` — drop nullptr from 13 Rebuild calls**

Find and replace all 13 occurrences (verified via `grep -c "Rebuild(w, DefaultCfg(), nullptr)"`):

```cpp
// Old:
NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);

// New:
NavMeshSystem::Instance().Rebuild(w, DefaultCfg());
```

Verify after edit:

```bash
grep -n "Rebuild(.*nullptr)" tests/test_navmesh.cpp
```

Expected: no matches.

- [ ] **Step 6: Sweep `src/engine/src/threading/GameThread.cpp` — drop nullptr from hook lambda**

Find the existing OnRebuildNavMesh hook lambda (around line 228-232):

```cpp
hooks.OnRebuildNavMesh = [](ECS& w) {
    const auto* cfg = w.GetSingleton<NavMeshConfigComponent>();
    NavMeshConfigComponent defaultCfg{};
    NavMeshSystem::Instance().Rebuild(w, cfg ? *cfg : defaultCfg, nullptr);
};
```

Replace with:

```cpp
hooks.OnRebuildNavMesh = [](ECS& w) {
    const auto* cfg = w.GetSingleton<NavMeshConfigComponent>();
    NavMeshConfigComponent defaultCfg{};
    NavMeshSystem::Instance().Rebuild(w, cfg ? *cfg : defaultCfg);
};
```

Drop the comment "MeshSystem is currently not reachable from GameThread (lives on RenderThread, not exposed via ApplicationContext)" if present — it's no longer true / no longer relevant.

- [ ] **Step 7: Build + run all tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine editor game test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: clean build. All 27 tests pass (T01-T25 still pass via signature sweep, T26 + T27 now pass with the new GetMeshCpuData path).

If T26 fails with `PolyCount > 0`: check the floor mesh size + Recast config defaults. A 4×4 flat floor at y=0 should produce a non-zero polycount under default config (`CellSize=0.3`, `AgentHeight=2.0`). If still failing, the issue is likely the bmax-padding logic from Spec 1 commit `d4a05f3` (flat-only floors need padded bmax.y for `rcFilterWalkableLowHeightSpans`). T26's floor is flat — the bmax padding from that earlier fix should keep it walkable. Verify the fix is still in place.

- [ ] **Step 8: Regression sweep**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_collision test_worldserial test_menu test_followcam test_playermove test_editorprefs --config Debug
for t in test_ecs test_alloc test_collision test_worldserial test_menu test_followcam test_playermove test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
```

Expected: all 8 pass.

- [ ] **Step 9: Commit**

```bash
git add src/engine/src/navigation/NavMeshBuilder.h src/engine/src/navigation/NavMeshBuilder.cpp src/engine/src/navigation/NavMeshSystem.h src/engine/src/navigation/NavMeshSystem.cpp tests/test_navmesh.cpp src/engine/src/threading/GameThread.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(navigation): drop MeshSystem* param; use NavMeshSystem cache

NavMeshBuilder::CollectTriangles + NavMeshSystem::Rebuild lose
their MeshSystem* parameter. NavMeshBuilder's Mesh-source branch
now calls NavMeshSystem::Instance().GetMeshCpuData() directly
instead of meshSystem->GetMeshCpuData().

GameThread's OnRebuildNavMesh hook lambda drops the trailing
nullptr arg — the Spec 1 Risk 1 footgun (lambda captured nullptr
because MeshSystem unreachable from GameThread) is now structurally
impossible: there's no parameter to pass.

13 existing test_navmesh Rebuild calls swept to the 2-arg form.
T26 + T27 pass (T26: real Mesh-source contributes triangles via
the cache; T27: cache miss preserves existing SM_WARN fallback).

GameThread mesh-drain still doesn't populate the cache yet — that's
Task 4. Until then T26 passes only because the test populates the
cache manually via StoreMeshCpuData."
```

---

## Task 4: GameThread populates the cache (two-stage flow via pending map)

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp`

- [ ] **Step 1: Add the pending-mesh-data struct + function-scope map**

In `src/engine/src/threading/GameThread.cpp::RunLoop`, find the section where the existing function-scope locals are declared (around lines 156-158, near `NavServices navServices{}; NavServicesImpl::Init(navServices);`).

Add include at top of file if not already present:
```cpp
#include "navigation/NavMeshSystem.h"   // for StoreMeshCpuData (likely already present from Spec 4)
```

Insert immediately after the `navServices` block, before the main `while (Running())` loop:

```cpp
    // Spec 5: per-MeshId verts/indices in flight from the mesh-upload command
    // (GameThread sends to RenderThread) → the MeshUpload response (RenderThread
    // sends back). Keyed by ticketId (entityId) at upload time, transferred to
    // NavMeshSystem's MeshId-keyed cache when the response arrives. Function-
    // scope: lives for the entire RunLoop, sole owner is GameThread.
    struct PendingMeshData {
        std::vector<MeshVertex> Vertices;
        std::vector<uint32_t>   Indices;
    };
    std::unordered_map<EntityId, PendingMeshData> pendingMeshData;
```

Add `#include <unordered_map>` at the top of GameThread.cpp if not already present (it likely is — Spec 1+2 use it).

- [ ] **Step 2: Capture verts/indices at the mesh-upload command site**

Find the mesh-upload command construction inside the `ProcessCompletedModelLoads` block (around line 280-330). After the successful `m_AppContext->GRCommandRing.Push(meshCmd)` + `res.MeshUploaded = true;` (around line 329), and BEFORE the next loop iteration (where `res` would be destroyed), add:

```cpp
                        // Spec 5: stash verts/indices keyed by ticketId. When the MeshUpload
                        // response arrives with the assigned MeshId, we transfer ownership
                        // into NavMeshSystem's cache (see ProcessRenderResponses below).
                        // Vectors are no longer needed by the upload path (staging-pool
                        // memcpy already happened above).
                        pendingMeshData[res.ticketId] = PendingMeshData{
                            std::move(res.vertices),
                            std::move(res.indices)
                        };
```

The exact location: this should go inside the `if (!res.MeshUploaded)` block, just after `res.MeshUploaded = true;` (currently line 329).

Note: the `res.MeshUploaded = true; ... res.MeshUploaded = true;` line itself does not change; the new code goes ONE LINE BELOW it, still inside the same `if (!res.MeshUploaded)` block.

- [ ] **Step 3: Transfer to NavMeshSystem cache when MeshUpload response arrives**

Find the `ProcessRenderResponses` block around line 353. Inside the `case RendererResponseType::MeshUpload:` branch (around line 358-373), after the existing `gameState.World.Modify<MeshComponent>(...)` call (around line 365-368) and before the SM_TRACE log, add:

```cpp
                            // Spec 5: transfer the verts/indices we stashed at upload
                            // time into NavMeshSystem's MeshId-keyed cache. Mesh-source
                            // entities now contribute triangles to the navmesh build.
                            auto it = pendingMeshData.find(response.TicketId);
                            if (it != pendingMeshData.end()) {
                                NavMeshSystem::Instance().StoreMeshCpuData(
                                    response.Mesh.Handle.Index,
                                    std::move(it->second.Vertices),
                                    std::move(it->second.Indices));
                                pendingMeshData.erase(it);
                            }
```

- [ ] **Step 4: Build + run all tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine editor game test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: clean build. All 27 tests pass (T26/T27 still green — they don't depend on this Task; they exercise StoreMeshCpuData directly).

- [ ] **Step 5: Full regression sweep**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs --config Debug
for t in test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "All tests green."
```

Expected: each prints success, final "All tests green."

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/threading/GameThread.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(engine): GameThread populates NavMeshSystem mesh cache

Two-stage flow: function-scope pendingMeshData map in RunLoop is
keyed by ticketId. When GameThread sends a RequestMesh command to
RenderThread (mesh-drain section, ~line 329), it stashes the verts
and indices via std::move from the ModelLoadResult. When the
MeshUpload response arrives (~line 365) carrying the assigned
MeshId, GameThread transfers ownership into NavMeshSystem's
MeshId-keyed cache via StoreMeshCpuData and erases the pending
entry.

Closes the Spec 1 Risk 1 carryover end-to-end. Geometry=Mesh
entities now contribute triangles to the navmesh build. Worker
thread loads .obj/.gltf → GameThread sends to RenderThread for GPU
upload + stashes a copy locally → response triggers cache populate
→ next NavMeshSystem::Rebuild's NavMeshBuilder finds the mesh data
in the cache and triangulates."
```

---

## Task 5: Final whole-feature review

- [ ] **Step 1: Verify clean tree + full test sweep**

```bash
git status -sb
# Expected: clean.

cmake --build out/build/msvc-win64-vs2026-community --target Engine editor game test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs --config Debug

for t in test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "All tests green."
```

Expected: all 9 test suites pass + final line "All tests green."

Run `dumpbin /DEPENDENTS Game.dll` to confirm the NavServices-decoupling boundary is preserved (Game.dll still only depends on ecs.dll + CRT, no Engine.dll):

```bash
dumpbin /DEPENDENTS out/build/msvc-win64-vs2026-community/bin/Debug/Game.dll
# Expected: ecs.dll + CRT dlls only. Engine.dll MUST NOT appear.
```

- [ ] **Step 2: Dispatch final whole-feature reviewer subagent**

Per `superpowers:subagent-driven-development`, provide:
- Spec: `docs/superpowers/specs/2026-05-28-navigation-mesh-input-design.md` (commit `0dfe370`)
- Plan: `docs/superpowers/plans/2026-05-28-navigation-mesh-input.md`
- Full branch diff: `git diff main..feat/navigation-mesh-input`
- 4 commits one per task.
- Per-task review summaries (T1+T3 spec+code-quality; T2+T4 spec only since they're TDD-paired with novel work).

Reviewer verdict drives merge-readiness. User does GUI smoke before merge (Geometry=Mesh tagged entity should now contribute walkable area instead of SM_WARN+skipping — visible in ShowNavMesh debug viz).

GUI smoke checklist for the user:
1. Editor launches clean. Console shows mesh loads with their MeshIds.
2. Spawn a new entity, add MeshComponent (point at any loaded mesh), Add NavMeshSourceComponent, set Geometry=Mesh.
3. Click Rebuild NavMesh in Navigation panel.
4. Toggle ShowNavMesh in Render Stats → lime-green poly edges appear over the mesh's footprint (vs Spec 1 behavior: empty navmesh + console SM_WARN "no MeshSystem available").
5. Set NavTarget on an agent across the mesh-walkable area → agent walks the meshed terrain.
6. Reload world (or restart editor) → cache repopulates as meshes finish loading; click Rebuild after load completes; same outcome.

---

## Self-review notes

**Spec coverage check:**

- ✅ NavMeshSystem::StoreMeshCpuData + GetMeshCpuData + m_MeshCpuData map + CachedMesh struct — Task 1
- ✅ NavMeshBuilder::CollectTriangles drops MeshSystem* — Task 3
- ✅ NavMeshSystem::Rebuild drops MeshSystem* — Task 3
- ✅ NavMeshBuilder Mesh-branch uses NavMeshSystem::Instance().GetMeshCpuData() — Task 3
- ✅ 13 existing test_navmesh Rebuild calls swept — Task 3
- ✅ GameThread OnRebuildNavMesh hook drops trailing nullptr — Task 3
- ✅ T26 Geometry=Mesh end-to-end — Task 2 (TDD red) → Task 3 (green) → unchanged in Task 4
- ✅ T27 cache miss preserves SM_WARN — Task 2 (TDD red) → Task 3 (green)
- ✅ GameThread two-stage capture (pending map → cache) at mesh-upload + response-drain — Task 4
- ✅ Final whole-feature review — Task 5

No gaps.

**Type-consistency check:**

- `uint32_t meshId` type used consistently across StoreMeshCpuData, GetMeshCpuData, MeshComponent::MeshId, response.Mesh.Handle.Index, T26/T27 test code.
- `std::vector<MeshVertex>` + `std::vector<uint32_t>` move-by-rvalue in Store; `std::span<const T>` out-param in Get — consistent across Task 1 method bodies, Task 3 NavMeshBuilder call site, Task 4 GameThread call site.
- `PendingMeshData` struct in Task 4 mirrors `CachedMesh` struct in Task 1 — same field types/names. Could share the type via `NavMeshSystem::CachedMesh` but keep them separate for clarity (one is "in flight", one is "settled").
- `NavMeshSystem::Rebuild(w, cfg)` 2-arg form used consistently across Task 2 tests + Task 3 sweep + Task 4 verification.
- `EntityId` type for ticketId — consistent.

**Placeholder scan:** None. Every code step has complete code; every command has expected output.

**Commit count:** 4 commits (Tasks 1-4, Task 0 administrative, Task 5 review-only). Within the spec's 3-4 estimate.
