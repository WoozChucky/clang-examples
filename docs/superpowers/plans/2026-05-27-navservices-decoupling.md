# NavServices Decoupling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the `game.dll → Engine.dll` link added in Spec 2 Task 9. Route Game's navigation calls through a `NavServices` POD function-pointer table attached to `SystemContext`.

**Architecture:** `NavServices` lives in `src/common/include/` as a flat C function-pointer struct. Engine populates an instance at GameThread startup via `NavServicesImpl::Init`. `SystemContext` gains a `const NavServices* Nav = nullptr;` field threaded through every per-tick `m_Scheduler.Run`. NavObstacleSync + NavAgentSystem swap their direct `NavMeshSystem::Instance().X()` calls for `ctx.Nav->X()` calls. CMake reverts: game stops linking Engine + drops engine/src from include dirs.

**Tech Stack:** C++23, pure C function pointers across the DLL boundary, `<vector>` outparam for FindPath path data, `<glm/vec3.hpp>` shared types. No new third-party deps.

**Spec reference:** `docs/superpowers/specs/2026-05-27-navservices-decoupling-design.md` (commit `4b238c5`).

---

## Codebase orientation (read once before Task 1)

- **Game→Engine link came in Spec 2 Task 9 (commit `5731611`)** via 3-file CMake change. Today's `src/game/CMakeLists.txt:49-51` has `${CMAKE_SOURCE_DIR}/src/engine/src` include + `Engine` link. Task 7 removes both.
- **Current `SystemContext` (`src/common/include/Systems.h:10-14`)** has only `world / dt / gameTime`. Task 2 adds `Nav` field.
- **Current `NavObstacleSync.h`** (in `src/game/src/`) — header-only, calls `NavMeshSystem::Instance().{Current,AddCylinderObstacle,AddBoxObstacle,RemoveObstacle,FindObstacleForEntity,TrackObstacleForEntity,UntrackEntity}`. All 7 calls become `ctx.Nav->...`.
- **Current `NavAgentSystem.h`** (in `src/game/src/`) — header-only, calls `NavMeshSystem::Instance().Current()` then `nm->FindPath(start, end)`. Both calls collapse into one `ctx.Nav->FindPath(start, end, 50.0f, &m_PathScratch)` outparam variant.
- **`NavMeshSystem::Rebuild` stays engine-only.** Triggered via `RebuildNavMesh` ECSCommand hook from GameThread; Game.dll never calls `Rebuild` directly. No NavServices wrapper needed.
- **Test harness pattern.** `tests/test_navmesh.cpp` already links Engine + tests `NavMeshSystem::Instance()` directly. T09-T18 helpers (`RunSync`, `RunAgentSystem`) construct `SystemContext{ w, dt, 0.0 }` — Task 8 updates them to thread a `NavServices` pointer.
- **GameThread::Run main-loop tick body** (`src/engine/src/threading/GameThread.cpp` around lines 386-393): builds `SystemContext sysCtx{ gameState.World, gameState.DeltaTime, gameState.GameTime }` then calls `m_Scheduler.Run(sysCtx)`. Task 4 inserts `&navServices` as the 4th brace-init field.
- **`add_dependencies(Engine game)` stays removed** — editor + runtime each declare their own `add_dependencies(<exe> game)` per Spec 2 Task 9 final state. Game.dll still builds before editor.exe/runtime.exe run.
- **No GAME_API bump.** GameState layout unchanged. Editor hot-reload contract unchanged.

---

## Task 0: Create feature branch

**Files:** none (git only)

- [ ] **Step 1: Verify clean main**

```bash
git status -sb
# Expected: "## main...origin/main" — clean (caveman/.claude/ untracked OK).
git log --oneline -3
# Expected: 4b238c5 (NavServices design spec) + ec63dd3 (Spec 4 merge) + earlier.
```

- [ ] **Step 2: Create branch**

```bash
git checkout -b feat/navservices-decoupling
git status -sb
# Expected: "## feat/navservices-decoupling"
```

- [ ] **Step 3: No commit yet** — administrative only.

---

## Task 1: Create `NavServices` POD struct

**Files:**
- Create: `src/common/include/NavServices.h`

- [ ] **Step 1: Write the new header**

Create `src/common/include/NavServices.h`:

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
// All function pointers are non-null after NavServicesImpl::Init. Callable
// from GameThread only (matches NavMeshSystem's GameThread-only contract).
//
// Field append-only for forward compatibility — reordering or removing fields
// breaks Game.dll binary compat (function pointer offsets shift).
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

- [ ] **Step 2: Build to verify the header is syntactically valid**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target ecs Engine --config Debug
```

Expected: clean build. The header isn't included anywhere yet — pure file-add. Build sanity confirms the include + types are well-formed.

- [ ] **Step 3: Commit**

```bash
git add src/common/include/NavServices.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(common): NavServices POD function-pointer table

Engine-provided navigation service table. POD struct of 8 C function
pointers covering the NavMeshSystem API surface that Game systems
need: HasMesh + FindPath + 3 obstacle add/remove + 3 entity tracking.

Lives in common/include so Game.dll can use it without linking
Engine.dll. Function pointer signatures use glm::vec3 and EntityId
(both already available in common/) + std::vector<vec3>* outparam
for FindPath (matches existing NavMesh::CollectPolyEdges pattern,
avoids return-by-value vector across DLL boundary).

Field order is binary-compat for Game.dll — append-only additions
are forward-compatible; reordering/removing breaks ABI."
```

---

## Task 2: Extend `SystemContext` with `Nav` field

**Files:**
- Modify: `src/common/include/Systems.h`

- [ ] **Step 1: Add the field + include**

Modify `src/common/include/Systems.h`. At the top, after the existing `#include "ECS.h"` line (line 7), add:

```cpp
#include "NavServices.h"   // pulls full struct + std::vector/glm dependencies
```

Then extend the `SystemContext` struct (currently lines 10-14):

```cpp
// Minimal per-tick context handed to every system.
struct SystemContext {
    ECS&   world;
    double dt;        // seconds since last tick (clamped by GameThread)
    double gameTime;  // absolute time
    const NavServices* Nav = nullptr;  // engine-provided nav table; nullptr in test harness or pre-init
};
```

- [ ] **Step 2: Build everything to confirm header propagates**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target ecs Engine editor game --config Debug
```

Expected: clean build. All existing `SystemContext{ w, dt, gameTime }` constructions stay valid (Nav defaults to nullptr). Adding the field is backward-compatible at source level.

- [ ] **Step 3: Commit**

```bash
git add src/common/include/Systems.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(common): SystemContext gains const NavServices* Nav field

Default-nullptr keeps existing SystemContext{ w, dt, gameTime }
constructions valid (test harness paths). GameThread (Task 4)
populates with &navServices each tick. Game systems (Tasks 5+6)
read ctx.Nav->X() instead of NavMeshSystem::Instance().X().

NavServices.h pulled in transitively so systems that use ctx.Nav
don't need a separate include."
```

---

## Task 3: Create `NavServicesImpl` (engine-side forwarders)

**Files:**
- Create: `src/engine/src/navigation/NavServicesImpl.h`
- Create: `src/engine/src/navigation/NavServicesImpl.cpp`
- Modify: `src/engine/CMakeLists.txt` (add new .cpp to Engine sources)

- [ ] **Step 1: Create `NavServicesImpl.h`**

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

- [ ] **Step 2: Create `NavServicesImpl.cpp`**

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

uint32_t ForwardAddCylinderObstacle(const glm::vec3& pos, float radius, float height) {
    return NavMeshSystem::Instance().AddCylinderObstacle(pos, radius, height);
}

uint32_t ForwardAddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax) {
    return NavMeshSystem::Instance().AddBoxObstacle(bmin, bmax);
}

void ForwardRemoveObstacle(uint32_t handle) {
    NavMeshSystem::Instance().RemoveObstacle(handle);
}

void ForwardTrackObstacleForEntity(EntityId e, uint32_t handle) {
    NavMeshSystem::Instance().TrackObstacleForEntity(e, handle);
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

- [ ] **Step 3: Add to Engine `CMakeLists.txt`**

In `src/engine/CMakeLists.txt`, find the Navigation sources block (added in Spec 1 — currently has `NavMeshBuilder.cpp`, `NavMesh.cpp`, `NavMeshSystem.cpp`). Append:

```cmake
    # Navigation (Recast/Detour)
    src/navigation/NavMeshBuilder.cpp
    src/navigation/NavMesh.cpp
    src/navigation/NavMeshSystem.cpp
    src/navigation/NavServicesImpl.cpp
```

- [ ] **Step 4: Build**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine --config Debug
```

Expected: Engine.dll rebuilds clean. NavServicesImpl.obj produced. No callers yet — pure compile-test.

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/navigation/NavServicesImpl.h src/engine/src/navigation/NavServicesImpl.cpp src/engine/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(engine): NavServicesImpl forwarders + Init factory

Engine-side anonymous-namespace forwarder functions that call into
NavMeshSystem::Instance() and NavMesh::FindPath. NavServicesImpl::Init
populates a NavServices struct with the function pointer addresses.

ForwardFindPath projects PathPoint{Position, AreaId} to bare vec3 via
the outparam vector — game-side callers (NavAgentSystem) only use
Position, AreaId would be a future field-append on NavServices.

Header is engine-private (lives in engine/src/navigation/). Init()
called once from GameThread::Run (Task 4) before main loop. No
callers yet."
```

---

## Task 4: GameThread wires NavServices per tick

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp`

- [ ] **Step 1: Add include + init the table once**

In `src/engine/src/threading/GameThread.cpp`, add include near the top (after existing navigation includes from Specs 1+4):

```cpp
#include "navigation/NavServicesImpl.h"
```

Find the point in `GameThread::Run` AFTER engine init completes but BEFORE the main loop begins. Look for the first time `gameState` is fully constructed and the loop is about to start — around line 80-100 (after `gameState.World.SetSingleton(...)` calls, before the `while (m_Running)` or similar loop entry).

Insert before the main loop:

```cpp
    // NavServices function-pointer table — engine-provided nav API surface
    // that game systems (NavObstacleSync, NavAgentSystem) call through
    // instead of NavMeshSystem::Instance() directly. Restores Game.dll's
    // GameState-only boundary (Game.dll no longer links Engine.dll).
    NavServices navServices{};
    NavServicesImpl::Init(navServices);
```

(Exact insertion line will depend on the actual code structure — read the file to find the right spot. The `navServices` local must live longer than the main loop, so place it in `GameThread::Run`'s function scope, after the gameState/setup block, before the main `while` loop.)

- [ ] **Step 2: Thread `&navServices` into SystemContext per tick**

Find the existing SystemContext construction (Spec 1 Task 6 added it, around line 391):

```cpp
SystemContext sysCtx{ gameState.World, gameState.DeltaTime, gameState.GameTime };
m_Scheduler.Run(sysCtx);
```

Append `&navServices` as 4th brace-init element:

```cpp
SystemContext sysCtx{ gameState.World, gameState.DeltaTime, gameState.GameTime, &navServices };
m_Scheduler.Run(sysCtx);
```

- [ ] **Step 3: Build**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine --config Debug
```

Expected: clean build. SystemContext's new `Nav` field gets populated each tick.

- [ ] **Step 4: Commit**

```bash
git add src/engine/src/threading/GameThread.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(engine): GameThread populates SystemContext::Nav per tick

NavServices table allocated as function-scope local in GameThread::Run,
initialized once via NavServicesImpl::Init before main loop. Per-tick
SystemContext construction passes &navServices as the 4th brace-init
field (Nav).

navServices lives for the entire GameThread::Run scope; function
pointers reference engine code that lives as long as Engine.dll.
Systems receive a const NavServices* valid for the entire tick body."
```

---

## Task 5: Rewrite `NavObstacleSync.h` to use `ctx.Nav`

**Files:**
- Modify: `src/game/src/NavObstacleSync.h`

- [ ] **Step 1: Replace the header content**

Open `src/game/src/NavObstacleSync.h`. Drop the existing `navigation/NavMeshSystem.h` include (if present) — game systems no longer reach into engine headers. Replace the file content with:

```cpp
#pragma once

#include <unordered_map>
#include <unordered_set>

#include <glm/glm.hpp>

#include "ECS.h"
#include "Systems.h"   // SystemContext + NavServices pulled in transitively
#include "lib.h"       // SM_WARN

// Physics-phase system that keeps dtTileCache obstacles in sync with the ECS
// NavObstacleComponent set. Per-tick diff:
//   - Component appears (no cached handle)   -> AddObstacle + Track + cache state
//   - Component disappears (entity vanished) -> RemoveObstacle + Untrack + erase cache
//   - Position moved > kPositionEpsilon      -> RemoveObstacle + AddObstacle (rebind)
//   - Shape or Size changed                  -> same rebind
//
// dtTileCache::update is NOT called here — GameThread::Run calls
// NavMeshSystem::Instance().Tick(dt) once per tick after all systems run, so
// other sites (Spec 3 agents) can request a tick without going through this
// system.
//
// Engine-decoupled v2: all NavMeshSystem calls now go through
// ctx.Nav->X() (function-pointer table on SystemContext) instead of
// NavMeshSystem::Instance().X(). Keeps Game.dll's link graph free of
// Engine.dll.
class NavObstacleSyncSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (!ctx.Nav || !ctx.Nav->HasMesh()) return;  // no navmesh built yet — nothing to carve

        m_VisitedThisTick.clear();

        ctx.world.Each<NavObstacleComponent, TransformComponent>(
            [&](EntityId e, const NavObstacleComponent& obs, const TransformComponent& tr) {
                m_VisitedThisTick.insert(e);
                const glm::vec3 worldPos = tr.Position + obs.Offset;

                auto& prev = m_EntityCache[e];   // default-constructs to zero state
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
                    } else {  // Box
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

        // GC entities that disappeared this tick (component removed or entity destroyed).
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

    const char* Name() const override { return "NavObstacleSyncSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Physics; }

private:
    struct CachedState {
        glm::vec3        Position{0.0f};
        NavObstacleShape Shape{NavObstacleShape::Cylinder};
        glm::vec3        Size{0.0f};
    };
    std::unordered_map<EntityId, CachedState> m_EntityCache;
    std::unordered_set<EntityId>              m_VisitedThisTick;
    static constexpr float kPositionEpsilon = 0.05f;  // 5 cm — below default cell size 0.3m
};
```

The structural changes from current:
- Drop `#include "navigation/NavMeshSystem.h"`.
- `auto& nav = NavMeshSystem::Instance();` line removed.
- `if (!nav.Current()) return;` → `if (!ctx.Nav || !ctx.Nav->HasMesh()) return;`
- `nav.FindObstacleForEntity(e)` → `ctx.Nav->FindObstacleForEntity(e)`.
- `nav.RemoveObstacle(existing)` → `ctx.Nav->RemoveObstacle(existing)`.
- `nav.UntrackEntity(e)` → `ctx.Nav->UntrackEntity(e)`.
- `nav.AddCylinderObstacle(...)` → `ctx.Nav->AddCylinderObstacle(...)`.
- `nav.AddBoxObstacle(...)` → `ctx.Nav->AddBoxObstacle(...)`.
- `nav.TrackObstacleForEntity(e, h)` → `ctx.Nav->TrackObstacleForEntity(e, h)`.
- `NavMeshSystem::ObstacleHandle h = 0;` → `uint32_t h = 0;` (ObstacleHandle is just a typedef for uint32_t; NavServices exposes the raw type).
- GC loop: `nav.FindObstacleForEntity` / `nav.RemoveObstacle` / `nav.UntrackEntity` → `ctx.Nav->...`.

- [ ] **Step 2: Build to verify (will fail until Tasks 6+7 land in tandem since game still links Engine — but verify NavObstacleSync header alone compiles)**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target game --config Debug
```

Expected: clean build. game.dll still links Engine.dll at this point (Task 7 removes it), but NavObstacleSync.h itself no longer references engine-private headers — it only uses `ctx.Nav->X()` calls through the NavServices pointer.

- [ ] **Step 3: Run regression tests (T09-T13 obstacle tests — these use SystemContext without Nav populated)**

Tests use the existing `RunSync(w, sync)` helper which constructs `SystemContext{ w, 0.016 }` — `Nav` defaults to nullptr. The new NavObstacleSync `Update` early-returns on `!ctx.Nav`. **Tests will pass trivially but no longer exercise the sync behavior.** That's expected at this checkpoint — Task 8 updates the test helpers to thread a NavServices pointer.

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: tests still build clean. T09-T13 may "pass" by short-circuiting (early-return on null Nav). Task 8 restores real coverage.

- [ ] **Step 4: Commit**

```bash
git add src/game/src/NavObstacleSync.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(navigation): NavObstacleSync uses ctx.Nav table

Drop navigation/NavMeshSystem.h include. Replace
NavMeshSystem::Instance().X() calls with ctx.Nav->X() throughout.
Early-return on !ctx.Nav || !ctx.Nav->HasMesh() (instead of
checking nav.Current()).

NavMeshSystem::ObstacleHandle (uint32_t typedef) becomes bare
uint32_t at the call site since NavServices exposes the raw type.

Tests at this checkpoint use SystemContext{ w, dt } with Nav=nullptr;
NavObstacleSync early-returns. Task 8 restores test coverage by
threading a NavServices pointer through the helpers."
```

---

## Task 6: Rewrite `NavAgentSystem.h` to use `ctx.Nav`

**Files:**
- Modify: `src/game/src/NavAgentSystem.h`

- [ ] **Step 1: Replace the header content**

Open `src/game/src/NavAgentSystem.h`. Drop the existing `navigation/NavMeshSystem.h` + `navigation/NavMesh.h` includes. Replace the file content with:

```cpp
#pragma once

#include <algorithm>
#include <vector>

#include <glm/glm.hpp>

#include "ECS.h"
#include "Systems.h"   // SystemContext + NavServices pulled in transitively
#include "lib.h"       // SM_WARN (currently unused; reserved for future rate-limited no-path logging)

// Simulation-phase system that drives nav-agent entities by writing
// MoveIntentComponent toward the next waypoint of a freshly-queried path.
//
// v1 STATELESS DESIGN:
//   Each tick: ctx.Nav->FindPath(transform, target) -> walk toward path[1]
//   capped at MoveSpeed * dt. No path cache. CPU cost = O(agents *
//   FindPath). Acceptable for ~10 agents on current scene scale.
//
// v2 UPGRADE PATH (when agent count > 20 or FindPath cost dominates):
//   Add cached path + PathIndex + TimeSinceRepath to NavAgentComponent.
//   Repath only on target-change OR PathIndex-stale OR time-elapsed.
//   See navigation-agents-design.md for migration notes.
//
// Engine-decoupled v2: NavMesh queries now go through ctx.Nav->FindPath
// (function-pointer table on SystemContext) instead of NavMesh::FindPath
// directly. m_PathScratch is a per-system reusable buffer so the FindPath
// outparam vector doesn't reallocate per call.
class NavAgentSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (!ctx.Nav || !ctx.Nav->HasMesh()) return;  // no navmesh built yet — nothing to path
        const float dt = static_cast<float>(ctx.dt);

        ctx.world.Each<NavAgentComponent, NavTargetComponent, TransformComponent>(
            [&](EntityId e, const NavAgentComponent& agent,
                const NavTargetComponent& target, const TransformComponent& tr) {

                // Reached check first — arrived agents skip FindPath entirely.
                const glm::vec3 toTarget = target.Destination - tr.Position;
                const float dist2 = glm::dot(toTarget, toTarget);
                if (dist2 < agent.ReachedEpsilon * agent.ReachedEpsilon) {
                    return;  // arrived; stop emitting intent (does NOT remove target)
                }

                // Stateless query — every tick. v2 caches this.
                ctx.Nav->FindPath(tr.Position, target.Destination, 50.0f, &m_PathScratch);
                if (m_PathScratch.size() < 2) {
                    return;  // no path (unreachable / off-mesh start/end / FindPath fail)
                }

                // Walk toward path[1] (path[0] is start-position-snapped-to-navmesh).
                const glm::vec3 nextWaypoint = m_PathScratch[1];
                const glm::vec3 dir = nextWaypoint - tr.Position;
                const float dirLen = glm::length(dir);
                if (dirLen < 1e-5f) return;  // degenerate (waypoint coincides with position)

                const glm::vec3 normalized = dir / dirLen;
                const float stepLen = std::min(agent.MoveSpeed * dt, dirLen);
                const glm::vec3 desiredDelta = normalized * stepLen;

                // Lazy-seed MoveIntent on first move (same pattern as PlayerMovementSystem
                // from movement-decoupling spec). KinematicMovementSystem (Physics phase)
                // consumes + clears.
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
    // Reusable buffer for ctx.Nav->FindPath outparam — cleared by FindPath
    // each call, never grows beyond the longest path encountered. Avoids
    // per-tick reallocation in the hot path.
    std::vector<glm::vec3> m_PathScratch;
};
```

Structural changes from current:
- Drop `#include "navigation/NavMeshSystem.h"` and `#include "navigation/NavMesh.h"`.
- `const auto nav = NavMeshSystem::Instance().Current(); if (!nav) return;` → `if (!ctx.Nav || !ctx.Nav->HasMesh()) return;`
- `const auto path = nav->FindPath(tr.Position, target.Destination); if (path.size() < 2) return;` → `ctx.Nav->FindPath(tr.Position, target.Destination, 50.0f, &m_PathScratch); if (m_PathScratch.size() < 2) return;`
- `const glm::vec3 nextWaypoint = path[1].Position;` → `const glm::vec3 nextWaypoint = m_PathScratch[1];` (NavServices::FindPath outputs vec3 directly, no PathPoint::Position projection needed).
- New private member: `std::vector<glm::vec3> m_PathScratch;`.

- [ ] **Step 2: Build to verify the header compiles**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target game --config Debug
```

Expected: clean build. game.dll still links Engine.dll at this checkpoint.

- [ ] **Step 3: Run tests (T14-T18 — same expected behavior as Task 5 — early-return on null Nav)**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: build clean, T14-T18 short-circuit. Task 8 restores coverage.

- [ ] **Step 4: Commit**

```bash
git add src/game/src/NavAgentSystem.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(navigation): NavAgentSystem uses ctx.Nav table

Drop navigation/NavMeshSystem.h + navigation/NavMesh.h includes.
Replace NavMeshSystem::Instance().Current()->FindPath(...) with
ctx.Nav->FindPath(start, end, 50.0f, &m_PathScratch).

m_PathScratch is a per-system std::vector<vec3> reused across ticks.
FindPath's outparam clears + populates; no reallocation in steady
state once max path length is observed.

NavServices::FindPath outputs vec3 directly (PathPoint::Position
projection happens engine-side in ForwardFindPath); no .Position
member access at the call site.

Tests at this checkpoint short-circuit on null ctx.Nav; Task 8
restores coverage."
```

---

## Task 7: Revert game CMake (remove Engine link + include dir)

**Files:**
- Modify: `src/game/CMakeLists.txt`

- [ ] **Step 1: Remove Engine link and engine/src include dir**

Open `src/game/CMakeLists.txt`. Find lines 47-51 (Spec 2 Task 9 additions):

```cmake
target_include_directories(game PUBLIC include)
# Navigation system headers (NavMeshSystem.h) live under Engine's src tree.
# NavObstacleSync.h pulls them in; game links Engine.dll for the imports.
target_include_directories(game PRIVATE ${CMAKE_SOURCE_DIR}/src/engine/src)

target_link_libraries(game PRIVATE glm::glm CommonHeaders ecs Engine)
```

Replace with:

```cpp
target_include_directories(game PUBLIC include)

target_link_libraries(game PRIVATE glm::glm CommonHeaders ecs)
```

(Drop the engine/src include line entirely AND the trailing `Engine` from the link list. Comment explaining the original reason is now historical — strip it.)

- [ ] **Step 2: Build game and verify no Engine symbols remain in the import table**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target game --config Debug
```

Expected: clean build. game.dll links only `ecs.dll` + CommonHeaders interface lib + glm headers + Windows runtime.

Verify no Engine.dll dependency:

```bash
dumpbin /DEPENDENTS out/build/msvc-win64-vs2026-community/bin/Debug/Game.dll | grep -i "Engine\|ecs"
# Expected output (one line):
#    ecs.dll
# (Engine.dll should NOT appear.)
```

If `Engine.dll` appears in the import list, the link removed but something still drags it in — diagnose by searching for any remaining engine-internal include in game/src/ headers.

- [ ] **Step 3: Build editor + runtime + Engine to ensure nothing breaks**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine editor game --config Debug
```

Expected: all 3 build clean. Editor + runtime still depend on game via `add_dependencies(editor game)` / `add_dependencies(runtime game)` (Spec 2 Task 9 final state — preserved).

- [ ] **Step 4: Commit**

```bash
git add src/game/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "build(game): drop Engine link + engine/src include dir

Reverts Spec 2 Task 9's game→Engine coupling. game.dll's import
table now lists only ecs.dll (verified via dumpbin). Engine.dll
no longer appears in Game.dll dependencies.

Game systems (NavObstacleSync, NavAgentSystem) now call through
ctx.Nav function-pointer table threaded into SystemContext by
GameThread instead of NavMeshSystem::Instance() directly.

editor + runtime still independently depend on game via
add_dependencies; Game.dll build order preserved without the
deprecated Engine→game edge."
```

---

## Task 8: Update test_navmesh helpers + add T24/T25 NavServices coverage

**Files:**
- Modify: `tests/test_navmesh.cpp`

- [ ] **Step 1: Add `NavServicesImpl` include + static NavServices init helper**

In `tests/test_navmesh.cpp`, add include near the existing navigation includes:

```cpp
#include "navigation/NavServicesImpl.h"
```

After the existing helper section (around the existing `DefaultCfg()` / `SpawnNavBox` / `DrainTileCache` block), add a NavServices accessor that initializes once and returns a static pointer:

```cpp
// Process-wide NavServices instance for tests that thread it through
// SystemContext. Lazy-initialized on first call so tests don't pay the
// init cost when they only test engine-internal NavMesh directly.
static const NavServices* TestNavServices() {
    static NavServices svc{};
    static bool initialized = false;
    if (!initialized) {
        NavServicesImpl::Init(svc);
        initialized = true;
    }
    return &svc;
}
```

- [ ] **Step 2: Update `RunSync` to thread NavServices into SystemContext**

Find the existing `RunSync` helper (added Spec 2 Task 5 — around the obstacle test block):

```cpp
static void RunSync(ECS& w, NavObstacleSyncSystem& sync) {
    SystemContext ctx{ w, 0.016 };
    sync.Update(ctx);
}
```

Replace with:

```cpp
static void RunSync(ECS& w, NavObstacleSyncSystem& sync) {
    SystemContext ctx{ w, 0.016, 0.0, TestNavServices() };
    sync.Update(ctx);
}
```

- [ ] **Step 3: Update `RunAgentSystem` to thread NavServices into SystemContext**

Find the existing `RunAgentSystem` helper (added Spec 3 Task 4):

```cpp
static void RunAgentSystem(ECS& w, NavAgentSystem& sys, double dt = 0.016) {
    SystemContext ctx{ w, dt, 0.0 };
    sys.Update(ctx);
}
```

Replace with:

```cpp
static void RunAgentSystem(ECS& w, NavAgentSystem& sys, double dt = 0.016) {
    SystemContext ctx{ w, dt, 0.0, TestNavServices() };
    sys.Update(ctx);
}
```

- [ ] **Step 4: Add T24 + T25 NavServices direct-call tests**

After the existing T23 — `T23_try_load_from_disk_stale_returns_false` — append:

```cpp
// ---------- NavServices decoupling: T24-T25 ----------

static void T24_navservices_table_forwards_to_navmeshsystem() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);

    const NavServices* svc = TestNavServices();
    EXPECT(svc != nullptr);
    EXPECT(svc->HasMesh != nullptr);
    EXPECT(svc->FindPath != nullptr);
    EXPECT(svc->AddCylinderObstacle != nullptr);
    EXPECT(svc->HasMesh());

    std::vector<glm::vec3> path;
    svc->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0), 50.0f, &path);
    EXPECT(path.size() >= 2);

    const uint32_t h = svc->AddCylinderObstacle(glm::vec3(0, 0, 0), 1.0f, 2.0f);
    EXPECT(h != 0);
    svc->TrackObstacleForEntity(42, h);
    EXPECT(svc->FindObstacleForEntity(42) == h);
    svc->UntrackEntity(42);
    EXPECT(svc->FindObstacleForEntity(42) == 0);
    svc->RemoveObstacle(h);
}

static void T25_navservices_findpath_empty_when_no_mesh() {
    ECS empty;
    NavMeshSystem::Instance().Rebuild(empty, DefaultCfg(), nullptr);  // empty soup → null current

    const NavServices* svc = TestNavServices();
    EXPECT(!svc->HasMesh());

    std::vector<glm::vec3> path;
    svc->FindPath(glm::vec3(0), glm::vec3(1), 50.0f, &path);
    EXPECT(path.empty());
}
```

Wire into `main()` after T23:

```cpp
    T23_try_load_from_disk_stale_returns_false();
    T24_navservices_table_forwards_to_navmeshsystem();
    T25_navservices_findpath_empty_when_no_mesh();
```

- [ ] **Step 5: Build + run all tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: `All navmesh tests passed.` All 25 tests green. T09-T18 now exercise real sync + agent behavior again (NavServices threaded through SystemContext). T24/T25 pin the NavServices forwarder layer directly.

- [ ] **Step 6: Run full regression sweep**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs --config Debug
for t in test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "All tests green."
```

Expected: each test prints its success line, final line "All tests green."

- [ ] **Step 7: Commit**

```bash
git add tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "test(navigation): thread NavServices through test SystemContext + T24/T25

RunSync + RunAgentSystem helpers now construct SystemContext with
TestNavServices() as the 4th brace-init field. T09-T18 obstacle +
agent tests resume real behavior coverage (Tasks 5+6 made the
systems early-return on null ctx.Nav).

T24 verifies all 8 NavServices function pointers are set after
NavServicesImpl::Init and that the forwarders observably match
direct NavMeshSystem behavior (HasMesh, FindPath, obstacle
add/remove, entity tracking).
T25 verifies FindPath leaves outPath empty when no navmesh is
published (HasMesh returns false, no crash on null current)."
```

- [ ] **Step 8: Dispatch final whole-feature reviewer**

Per `superpowers:subagent-driven-development`, dispatch the final reviewer subagent. Provide:
- Spec: `docs/superpowers/specs/2026-05-27-navservices-decoupling-design.md` (commit `4b238c5`)
- Plan: `docs/superpowers/plans/2026-05-27-navservices-decoupling.md`
- Full branch diff: `git diff main..feat/navservices-decoupling`
- 8 commits one per task.
- Per-task reviews summary (all APPROVED).

Reviewer verdict drives merge-readiness. User does GUI smoke before merge (no GAME_API bump = no editor restart strictly needed, but editor + Engine + game all rebuild for the new SystemContext field).

---

## Self-review notes

**Spec coverage check:**

- ✅ NavServices POD struct with 8 function pointers — Task 1
- ✅ SystemContext gains `const NavServices* Nav` field — Task 2
- ✅ NavServicesImpl.{h,cpp} with anonymous-namespace forwarders + Init factory — Task 3
- ✅ NavServicesImpl.cpp added to Engine CMakeLists — Task 3
- ✅ GameThread initializes navServices once + threads &navServices into per-tick SystemContext — Task 4
- ✅ NavObstacleSync.h drops engine includes, replaces Instance() calls with ctx.Nav-> — Task 5
- ✅ NavAgentSystem.h same + adds m_PathScratch member — Task 6
- ✅ game CMake removes Engine link + engine/src include dir — Task 7
- ✅ dumpbin verification step in Task 7 confirms Engine.dll no longer in Game.dll dependencies
- ✅ Test helpers updated to thread NavServices through SystemContext — Task 8
- ✅ T24 NavServices forwarder coverage + T25 empty-mesh edge — Task 8
- ✅ Final whole-feature review — Task 8 Step 8

No gaps.

**Type-consistency check:**

- `NavServices` struct field set (8 fields) consistent across declaration (Task 1) + Init implementation (Task 3) + game system call sites (Tasks 5+6) + tests (Task 8).
- `uint32_t` obstacle handle type used at all call sites (replaces `NavMeshSystem::ObstacleHandle` typedef which was also uint32_t — same type, simpler name).
- `std::vector<glm::vec3>*` outparam for FindPath consistent (NavServices declaration, ForwardFindPath, NavAgentSystem m_PathScratch, T24 test path local).
- `const NavServices*` (pointer-to-const) consistent on SystemContext field + return type of TestNavServices() helper.
- `NavServicesImpl::Init(NavServices&)` signature consistent across header + cpp + GameThread caller + test helper.

**Placeholder scan:** No TBD/TODO/placeholder. Every code step has complete code. Task 4 Step 1 has a "find the right insertion point" note instead of an exact line number — but with surrounding context (after gameState setup, before main loop) it's an unambiguous spot.

**Commit count:** 8 commits (Tasks 1-8, Task 0 admin). Within spec's 4-6 estimate ± headroom (slight overshoot because TDD discipline keeps test commits separate from system-rewrite commits).
