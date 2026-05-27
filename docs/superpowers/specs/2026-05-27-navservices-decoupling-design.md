# NavServices Decoupling — Design Spec

**Date:** 2026-05-27
**Status:** Approved
**Type:** Refactor (no new feature; restores architectural boundary)

---

## Goal

Remove the `game.dll → Engine.dll` link added in Spec 2 Task 9 (CMake commit `5731611`). Restore the original boundary where Game.dll only depends on the GameState contract (plus ecs.dll). Achieve this by funneling navigation services through a function-pointer table (`NavServices`) attached to `SystemContext`. Game systems (`NavObstacleSyncSystem`, `NavAgentSystem`) call through the table instead of `NavMeshSystem::Instance()` directly.

**Why this matters:** Spec 2's CMake change introduced a silent-ABI footgun — any non-`GAME_API_VERSION`-gated change to `ENGINE_API` symbols consumed by Game.dll would let Game.dll load successfully then crash on first call. Restoring the GameState-only boundary makes that class of bug impossible.

---

## Scope

**In scope:** Replace `NavMeshSystem::Instance().X()` calls in `src/game/src/NavObstacleSync.h` + `src/game/src/NavAgentSystem.h` with `ctx.Nav->X()` calls. Revert game's CMake link to Engine. Add `NavServices` POD struct + Engine-side forwarder + GameThread per-tick wiring.

**Out of scope:**
- Extracting navigation into its own DLL (decided against in brainstorming).
- Spec 3 v2 path caching for NavAgent.
- Spec 1 carryover: MeshSystem-from-GameThread access. `NavMeshSystem::Rebuild` still takes `MeshSystem*` directly — engine-only call site (ECSCommand hook fires inside GameThread), no need to wrap.
- Versioning the NavServices table (uint32 version field). Append-only field additions are sufficient forward compat.
- Generalizing into a broader engine-services table (RenderServices, AudioServices). YAGNI.

---

## Architecture

**File layout:**

```
src/common/include/
  ├── NavServices.h           # NEW — pure POD struct of function pointers
  └── Systems.h               # extend: + `const NavServices* Nav = nullptr;` on SystemContext
src/engine/src/navigation/
  ├── NavServicesImpl.h       # NEW — Init(NavServices&) factory declaration
  ├── NavServicesImpl.cpp     # NEW — forwarder functions calling into NavMeshSystem::Instance()
  └── NavMeshSystem.{h,cpp}   # unchanged
src/engine/src/threading/
  └── GameThread.cpp          # init NavServices once + thread &g_NavServices into SystemContext per tick
src/game/src/
  ├── NavObstacleSync.h       # drop NavMeshSystem/NavMesh includes; use ctx.Nav
  └── NavAgentSystem.h        # same + add m_PathScratch buffer
src/game/CMakeLists.txt
  - REMOVE Engine from target_link_libraries
  - REMOVE ${CMAKE_SOURCE_DIR}/src/engine/src from target_include_directories
src/engine/CMakeLists.txt
  + add NavServicesImpl.cpp to sources
  (note: add_dependencies(Engine game) stays removed — editor + runtime already depend on game directly per Spec 2 Task 9, build order preserved)
```

**Data flow:**

```
Engine startup (GameThread::Run, once before main loop):
  NavServices navServices{};
  NavServicesImpl::Init(navServices);

Per tick:
  SystemContext ctx{ world, dt, gameTime, &navServices };
  m_Scheduler.Run(ctx);

Game systems (header-only in src/game/src/):
  void Update(SystemContext& ctx) override {
      if (!ctx.Nav || !ctx.Nav->HasMesh()) return;
      ctx.Nav->AddCylinderObstacle(pos, r, h);
      // ...
  }
```

**Lifetime:** `navServices` local lives for the lifetime of GameThread::Run (function-scope stack variable). Function pointers reference engine code that lives as long as Engine.dll. SystemContext receives `const NavServices*` — read-only view, valid for the tick.

**Why `const NavServices*`:** game systems never mutate the table. Function pointers are constant post-Init. Const-correctness signals intent + prevents accidental field swaps.

---

## NavServices struct (`src/common/include/NavServices.h`)

```cpp
#pragma once

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

#include "ECS.h"   // EntityId — already in common/include, no engine dep

// Engine-provided navigation service table. Engine populates an instance at
// startup (NavServicesImpl::Init in src/engine/src/navigation/); GameThread
// threads a pointer through SystemContext each tick. Game systems
// (NavObstacleSync, NavAgentSystem) call through this table instead of
// NavMeshSystem::Instance() — keeps Game.dll's link graph free of Engine.dll.
//
// All function pointers are non-null after Init. Callable from GameThread
// only (matches NavMeshSystem's GameThread-only contract).
struct NavServices {
    // ---- NavMesh existence + query ----

    // True if a navmesh has been built (current published shared_ptr non-null).
    // Cheap atomic_load on engine side.
    bool (*HasMesh)();

    // Fill outPath with string-pulled path positions. Empty on no path /
    // unreachable / off-mesh. Caller owns the vector (clear+populate semantics).
    void (*FindPath)(const glm::vec3& start, const glm::vec3& end,
                     float maxSearchRadius, std::vector<glm::vec3>* outPath);

    // ---- Obstacle add/remove (Spec 2) ----

    uint32_t (*AddCylinderObstacle)(const glm::vec3& pos, float radius, float height);
    uint32_t (*AddBoxObstacle)(const glm::vec3& bmin, const glm::vec3& bmax);
    void     (*RemoveObstacle)(uint32_t handle);

    // ---- EntityId → ObstacleHandle side table (Spec 2) ----

    void     (*TrackObstacleForEntity)(EntityId e, uint32_t handle);
    uint32_t (*FindObstacleForEntity)(EntityId e);   // 0 if not tracked
    void     (*UntrackEntity)(EntityId e);
};
```

**Why std::vector* outparam for FindPath:** matches existing `NavMesh::CollectPolyEdges(std::vector<glm::vec3>& outLines)` pattern. Avoids return-by-value vector across DLL boundary. Lets callers reuse a member buffer.

**Why drop `PathPoint::AreaId`:** NavAgentSystem only uses `path[1].Position`. AreaId unused in game code v1. Extending NavServices later with `FindPathWithAreas` is purely additive (append field).

**Why include `<vector>` + `<glm/vec3.hpp>`:** function pointer signatures reference these types directly. ECS.h already pulls them transitively in this codebase. Acceptable common-layer dep.

---

## SystemContext extension

`src/common/include/Systems.h`:

```cpp
#include "NavServices.h"   // pulls full struct + std::vector/glm dependencies

struct SystemContext {
    ECS&    world;
    double  dt;
    double  gameTime;
    const NavServices* Nav = nullptr;
};
```

**Default-nullptr keeps backward compat.** Tests that construct `SystemContext{ w, dt, 0.0 }` without Nav stay valid — they just lose access to ctx.Nav, which they bypass via direct `NavMeshSystem::Instance()` calls anyway.

**Why full include (not forward-decl):** SystemContext stores `const NavServices*`, but every game system that calls `ctx.Nav->Foo(...)` needs the struct definition. Including in Systems.h means systems don't need a separate include.

---

## Engine setup

**`src/engine/src/navigation/NavServicesImpl.h`** (Engine-private — not exposed to Game):

```cpp
#pragma once

#include "NavServices.h"

namespace NavServicesImpl {
    // Populate the function pointer table. Safe to call multiple times
    // (idempotent — writes the same pointers). GameThread or RenderThread
    // can call; subsequent calls are no-ops since pointers are constant.
    void Init(NavServices& out);
}
```

**`src/engine/src/navigation/NavServicesImpl.cpp`:**

```cpp
#include "navigation/NavServicesImpl.h"

#include "navigation/NavMeshSystem.h"
#include "navigation/NavMesh.h"

namespace {

bool ForwardHasMesh() {
    return NavMeshSystem::Instance().Current() != nullptr;
}

void ForwardFindPath(const glm::vec3& start, const glm::vec3& end,
                     float maxSearchRadius, std::vector<glm::vec3>* outPath) {
    if (!outPath) return;
    outPath->clear();
    auto nm = NavMeshSystem::Instance().Current();
    if (!nm) return;
    const auto path = nm->FindPath(start, end, maxSearchRadius);
    outPath->reserve(path.size());
    for (const auto& pt : path) outPath->push_back(pt.Position);
}

uint32_t ForwardAddCylinderObstacle(const glm::vec3& pos, float r, float h) {
    return NavMeshSystem::Instance().AddCylinderObstacle(pos, r, h);
}

uint32_t ForwardAddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax) {
    return NavMeshSystem::Instance().AddBoxObstacle(bmin, bmax);
}

void ForwardRemoveObstacle(uint32_t h) {
    NavMeshSystem::Instance().RemoveObstacle(h);
}

void ForwardTrackObstacleForEntity(EntityId e, uint32_t h) {
    NavMeshSystem::Instance().TrackObstacleForEntity(e, h);
}

uint32_t ForwardFindObstacleForEntity(EntityId e) {
    return NavMeshSystem::Instance().FindObstacleForEntity(e);
}

void ForwardUntrackEntity(EntityId e) {
    NavMeshSystem::Instance().UntrackEntity(e);
}

} // namespace

namespace NavServicesImpl {

void Init(NavServices& out) {
    out.HasMesh                = &ForwardHasMesh;
    out.FindPath               = &ForwardFindPath;
    out.AddCylinderObstacle    = &ForwardAddCylinderObstacle;
    out.AddBoxObstacle         = &ForwardAddBoxObstacle;
    out.RemoveObstacle         = &ForwardRemoveObstacle;
    out.TrackObstacleForEntity = &ForwardTrackObstacleForEntity;
    out.FindObstacleForEntity  = &ForwardFindObstacleForEntity;
    out.UntrackEntity          = &ForwardUntrackEntity;
}

} // namespace NavServicesImpl
```

**GameThread.cpp wiring** — once at thread startup + per-tick SystemContext population:

```cpp
// Once at GameThread::Run startup (after Engine init, before main loop):
NavServices navServices{};
NavServicesImpl::Init(navServices);

// Per tick (replaces existing SystemContext construction at ~line 391):
SystemContext sysCtx{ gameState.World, gameState.DeltaTime, gameState.GameTime, &navServices };
m_Scheduler.Run(sysCtx);
```

**`navServices` lifetime:** function-scope stack local in GameThread::Run. Function pointers reference engine-static code that lives as long as Engine.dll. Pointer threaded into ctx is valid for the entire main loop.

---

## Game system rewrites

**`src/game/src/NavObstacleSync.h`** — replace `NavMeshSystem::Instance().X()` with `ctx.Nav->X()`:

```cpp
#pragma once

#include <unordered_map>
#include <unordered_set>

#include <glm/glm.hpp>

#include "ECS.h"
#include "Systems.h"  // SystemContext + NavServices pulled in transitively
#include "lib.h"      // SM_WARN

// (drop the navigation/NavMeshSystem.h + navigation/NavMesh.h includes)

class NavObstacleSyncSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (!ctx.Nav || !ctx.Nav->HasMesh()) return;

        m_VisitedThisTick.clear();

        ctx.world.Each<NavObstacleComponent, TransformComponent>(
            [&](EntityId e, const NavObstacleComponent& obs, const TransformComponent& tr) {
                m_VisitedThisTick.insert(e);
                const glm::vec3 worldPos = tr.Position + obs.Offset;

                auto& prev = m_EntityCache[e];
                const uint32_t existing = ctx.Nav->FindObstacleForEntity(e);

                const bool needsAdd     = (existing == 0);
                const bool moved        = glm::distance(prev.Position, worldPos) > kPositionEpsilon;
                const bool shapeChanged = prev.Shape != obs.Shape || prev.Size != obs.Size;
                const bool needsRebind  = !needsAdd && (moved || shapeChanged);

                if (needsRebind) {
                    ctx.Nav->RemoveObstacle(existing);
                    ctx.Nav->UntrackEntity(e);
                }
                if (needsAdd || needsRebind) {
                    uint32_t h = 0;
                    if (obs.Shape == NavObstacleShape::Cylinder) {
                        h = ctx.Nav->AddCylinderObstacle(worldPos, obs.Size.x, obs.Size.y);
                    } else {
                        h = ctx.Nav->AddBoxObstacle(worldPos - obs.Size, worldPos + obs.Size);
                    }
                    if (h != 0) {
                        ctx.Nav->TrackObstacleForEntity(e, h);
                        prev = { worldPos, obs.Shape, obs.Size };
                    } else {
                        SM_WARN("NavObstacleSync: AddObstacle failed for entity %llu "
                                "(MaxObstacles cap reached?)", e);
                    }
                }
            });

        // GC entities that disappeared this tick.
        for (auto it = m_EntityCache.begin(); it != m_EntityCache.end(); ) {
            if (!m_VisitedThisTick.contains(it->first)) {
                if (uint32_t h = ctx.Nav->FindObstacleForEntity(it->first); h != 0) {
                    ctx.Nav->RemoveObstacle(h);
                    ctx.Nav->UntrackEntity(it->first);
                }
                it = m_EntityCache.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Name/Phase/private members unchanged.
};
```

**`src/game/src/NavAgentSystem.h`** — same pattern + add `m_PathScratch` member to reuse the FindPath outparam buffer across ticks:

```cpp
#pragma once

#include <algorithm>
#include <vector>

#include <glm/glm.hpp>

#include "ECS.h"
#include "Systems.h"
#include "lib.h"

// (drop navigation/NavMeshSystem.h + navigation/NavMesh.h includes)

class NavAgentSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (!ctx.Nav || !ctx.Nav->HasMesh()) return;
        const float dt = static_cast<float>(ctx.dt);

        ctx.world.Each<NavAgentComponent, NavTargetComponent, TransformComponent>(
            [&](EntityId e, const NavAgentComponent& agent,
                const NavTargetComponent& target, const TransformComponent& tr) {

                const glm::vec3 toTarget = target.Destination - tr.Position;
                const float dist2 = glm::dot(toTarget, toTarget);
                if (dist2 < agent.ReachedEpsilon * agent.ReachedEpsilon) return;

                ctx.Nav->FindPath(tr.Position, target.Destination, 50.0f, &m_PathScratch);
                if (m_PathScratch.size() < 2) return;

                const glm::vec3 nextWaypoint = m_PathScratch[1];
                const glm::vec3 dir = nextWaypoint - tr.Position;
                const float dirLen = glm::length(dir);
                if (dirLen < 1e-5f) return;

                const float stepLen = std::min(agent.MoveSpeed * dt, dirLen);
                const glm::vec3 desiredDelta = (dir / dirLen) * stepLen;

                if (!ctx.world.HasComponent<MoveIntentComponent>(e)) {
                    ctx.world.AddComponent(e, MoveIntentComponent{});
                }
                ctx.world.Modify<MoveIntentComponent>(e, [&](MoveIntentComponent& m){
                    m.DesiredDelta = desiredDelta;
                });
            });
    }
    const char* Name() const override { return "NavAgentSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }

private:
    std::vector<glm::vec3> m_PathScratch;  // reused across ticks; cleared in FindPath
};
```

**Net change per system:** ~5-10 lines diff. Drop two `#include`s, swap `NavMeshSystem::Instance().X()` calls for `ctx.Nav->X()`, NavAgent gains a `m_PathScratch` member + signature change for `path[1].Position` → `m_PathScratch[1]`.

---

## CMake revert

**`src/game/CMakeLists.txt`:**

- Remove `${CMAKE_SOURCE_DIR}/src/engine/src` from `target_include_directories(game PRIVATE ...)`.
- Remove `Engine` from `target_link_libraries(game PRIVATE ...)`.
- Keep `CommonHeaders` (needed for NavServices.h via common/include).
- Keep `ecs` link.

**`src/engine/CMakeLists.txt`:**

- Add `src/navigation/NavServicesImpl.cpp` to Engine sources.
- `add_dependencies(Engine game)` stays removed (editor + runtime each carry their own `add_dependencies(<exe> game)` per Spec 2 Task 9 final state — build order preserved without Engine→game edge).

**Net link graph after revert:**

```
ecs.dll       — base (no dependents within nav)
Engine.dll    — links ecs.dll + Recast + ... (unchanged)
Game.dll      — links ecs.dll only (no Engine.dll)
editor.exe    — links Engine.dll, ecs.dll; depends-on game.dll (runtime load)
runtime.exe   — links Engine.dll, ecs.dll; depends-on game.dll (runtime load)
```

Game.dll back to "GameState contract only."

---

## Testing

**Existing test_navmesh tests T01-T23:** call `NavMeshSystem::Instance()` directly — engine-side tests, unchanged.

**New T24+T25 covering NavServices forwarders:**

- **T24** Build navmesh → `NavServicesImpl::Init` → call each forwarder → verify behavior matches direct NavMeshSystem call (`HasMesh`, `FindPath`, obstacle add/remove, entity tracking).
- **T25** Empty navmesh → `HasMesh()` returns false → `FindPath` leaves outPath empty (no crash on null).

**Updated T09-T18 helpers** (`RunSync`, `RunAgentSystem`) — `SystemContext` construction now needs a NavServices pointer:

```cpp
// In test helpers — init once at test start, thread through SystemContext.
static NavServices g_TestNav{};
static bool g_TestNavInit = false;
static void EnsureNavServicesInit() {
    if (!g_TestNavInit) {
        NavServicesImpl::Init(g_TestNav);
        g_TestNavInit = true;
    }
}

static void RunSync(ECS& w, NavObstacleSyncSystem& sync) {
    EnsureNavServicesInit();
    SystemContext ctx{ w, 0.016, 0.0, &g_TestNav };
    sync.Update(ctx);
}

static void RunAgentSystem(ECS& w, NavAgentSystem& sys, double dt = 0.016) {
    EnsureNavServicesInit();
    SystemContext ctx{ w, dt, 0.0, &g_TestNav };
    sys.Update(ctx);
}
```

**No test changes for T19-T23 (bake tests)** — they call NavMesh::SaveToFile / LoadFromFile directly, not through systems.

---

## Risks

1. **Per-tick atomic_load amplification.** `ForwardFindPath` calls `NavMeshSystem::Instance().Current()` → `atomic_load` per FindPath call. NavAgentSystem previously cached one atomic_load per tick; now does one per agent. At ≤10 agents × 60Hz = 600 atomic_loads/sec — negligible. Mitigation if hot later: BeginFrame/EndFrame pair on NavServices that pins the current NavMesh.

2. **vector<vec3>* outparam ABI risk across DLL boundary.** Both Engine.dll + Game.dll built with same MSVC runtime → safe in practice. If runtime mismatch ever happens (release Engine + debug Game), vector layout differs and corruption follows. Project ships matched configs; acceptable.

3. **NavServices init order.** GameThread initializes `navServices` once before main loop. Systems only run inside `m_Scheduler.Run(ctx)` which is in the main loop. If any future system runs before Init (architectural mistake), it sees `ctx.Nav == nullptr` and early-returns. Safe degradation.

4. **Field append-only.** Reordering or removing NavServices fields breaks Game.dll binary compat (function pointer offsets shift). Append-only additions are forward-compatible. Document in struct comment.

5. **NavMeshSystem::Rebuild stays engine-only.** Takes `MeshSystem*` directly (Spec 1 Risk 1 carryover). Not wrapped through NavServices. Triggered via the existing `RebuildNavMesh` ECSCommand hook from GameThread — engine wires the hook, game just pushes the command. Game never calls Rebuild directly.

6. **Test infra reaches into engine.** `tests/test_navmesh.cpp` links Engine and calls `NavServicesImpl::Init`. Engine-private header used in test scope — acceptable per existing test design (tests aren't shipped Game.dll consumers).

---

## File change summary

**New:**
- `src/common/include/NavServices.h`
- `src/engine/src/navigation/NavServicesImpl.h`
- `src/engine/src/navigation/NavServicesImpl.cpp`

**Modified:**
- `src/common/include/Systems.h` (+ NavServices include + Nav field on SystemContext)
- `src/engine/src/threading/GameThread.cpp` (init navServices + thread &navServices into per-tick SystemContext)
- `src/engine/CMakeLists.txt` (+ NavServicesImpl.cpp source)
- `src/game/CMakeLists.txt` (REMOVE Engine link + REMOVE engine/src include dir)
- `src/game/src/NavObstacleSync.h` (replace Instance() calls with ctx.Nav-> + drop nav headers)
- `src/game/src/NavAgentSystem.h` (same + add m_PathScratch member)
- `tests/test_navmesh.cpp` (+ T24/T25 NavServices coverage; update T09-T18 helpers to thread NavServices into SystemContext)

**Commit estimate:** 4-6 commits. Mostly mechanical replacement.

**No GAME_API bump.** GameState layout unchanged.
