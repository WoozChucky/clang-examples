# Navigation Agents Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive ECS entities along Recast navmesh paths by adding NavAgent + NavTarget components and a stateless Simulation-phase system that writes MoveIntentComponent toward the next waypoint each tick.

**Architecture:** NavAgentSystem (Simulation phase, header-only in `src/game/src/`) iterates `Each<NavAgentComponent, NavTargetComponent, TransformComponent>`. Reached-epsilon early-out skips FindPath for arrived agents; otherwise calls `NavMesh::FindPath(transform, target)`, walks toward `path[1]` at `MoveSpeed * dt` capped at `dirLen`, lazy-seeds + Modifies MoveIntentComponent. KinematicMovementSystem (Physics, already shipped) consumes the intent. Stateless v1: every tick re-queries; v2 cached-path upgrade documented in the spec.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), NVRHI deferred renderer, Recast/Detour libs vendored at `third_party/recastnavigation/` (linked into Engine by Spec 1). nlohmann/json for world.json persistence.

**Spec reference:** `docs/superpowers/specs/2026-05-27-navigation-agents-design.md` (commit `02db951`).

---

## Codebase orientation (read once before Task 1)

- **Specs 1 + 2 shipped** to main (merges `3801e00` + `33c967b`). NavMesh + NavMeshSystem + obstacle wiring + NavObstacleSync.h pattern all live.
- **Game→Engine link** established in Spec 2 — `src/game/CMakeLists.txt` adds `${CMAKE_SOURCE_DIR}/src/engine/src` to include dirs and `Engine` to link libraries. Spec 3 reuses this; no new CMake plumbing.
- **NavObstacleSync.h** at `src/game/src/NavObstacleSync.h` is the precedent for `NavAgentSystem.h` — header-only, ISystem inheritance, `world.Each<>` pattern, lives in game so it links against Engine via the already-configured deps.
- **MoveIntentComponent** lives in `src/common/include/ECS.h` (shipped by movement-decoupling spec). `KinematicMovementSystem` (Physics phase, in `src/game/src/game.cpp`) consumes it. Lazy-seed pattern: `if (!HasComponent<MoveIntent>) AddComponent({}); Modify<MoveIntent>(...)`. Match `PlayerMovementSystem` in `game.cpp` around lines 220-240 for the canonical idiom.
- **NavMesh::FindPath** signature: `std::vector<PathPoint> FindPath(const glm::vec3& start, const glm::vec3& end, float maxSearchRadius = 50.0f) const;` (in `src/engine/src/navigation/NavMesh.h:61`). Returns string-pulled path; `path[0]` is start-snapped to nearest poly, `path[1]` is first real waypoint.
- **`NavMeshSystem::Instance().Current()`** returns `std::shared_ptr<const NavMesh>`; load once into a local for the per-tick iteration to avoid repeated atomic_loads.
- **System registration** in `src/game/src/game.cpp::GameRegisterSystems` (around line 394). Simulation-phase systems come before Physics ones. `PlayerMovementSystem` at line 401, `KinematicMovementSystem` at line 402, `NavObstacleSyncSystem` at line 404. NavAgentSystem goes between PlayerMovement and KinematicMovement (registration order within Simulation phase).
- **Test pattern:** `tests/test_navmesh.cpp` already has `SpawnNavBox` + `SpawnCylinderObstacle` + `DefaultCfg` + `DrainTileCache` + `RunSync` from Specs 1/2. Add `SpawnAgent` + `RunAgentSystem` helpers + T14-T18.
- **`tests/CMakeLists.txt`** already includes `${CMAKE_SOURCE_DIR}/src/game/src` for test_navmesh (Spec 2 added this) — no CMake change needed for NavAgentSystem.h discovery.
- **Inspector pattern** — NavObstacleComponent's commit `65e2250` is the template: 2 new members per component (working copy + lastEdited tracker + modified flag), Add menu entry, Remove menu entry, edit block with snapshot-on-entity-change + refresh-if-clean + Modify push on dirty.

---

## Task 0: Create feature branch

**Files:** none (git only)

- [ ] **Step 1: Verify clean main**

```bash
git status -sb
# Expected: "## main...origin/main" with no uncommitted changes.
git log --oneline -3
# Expected: 02db951 (Spec 3 spec) + 33c967b (nav-obstacles merge) + earlier.
```

- [ ] **Step 2: Create branch**

```bash
git checkout -b feat/navigation-agents
git status -sb
# Expected: "## feat/navigation-agents"
```

- [ ] **Step 3: No commit yet** — administrative only.

---

## Task 1: ECS components — NavAgentComponent + NavTargetComponent + GAME_API bump

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/game/include/game.h` (GAME_API_VERSION 16 → 17)

- [ ] **Step 1: Add both components in `ECS.h`**

Add IMMEDIATELY AFTER the existing `NavObstacleComponent` struct (which ends around line 305, before the X-macro comment block):

```cpp
// Per-entity nav agent. Pairs with NavTargetComponent to drive intent-based
// movement: NavAgentSystem queries NavMesh::FindPath each tick, writes
// MoveIntent toward the next path waypoint at MoveSpeed * dt. Pure reader —
// system doesn't mutate this component.
struct NavAgentComponent {
    float MoveSpeed      = 3.0f;   // world units / second
    float Radius         = 0.5f;   // agent footprint; matches NavMeshConfigComponent::AgentRadius authoring
    float ReachedEpsilon = 0.10f;  // distance at which target is considered reached; stop emitting MoveIntent
};

// Per-entity destination. When attached to a NavAgent entity, system pathfinds
// toward Destination and writes MoveIntent. Game code sets/removes this to
// command the agent. Reaching the destination does NOT remove this component —
// gameplay decides (patrol systems keep it, single-move AI removes it).
struct NavTargetComponent {
    glm::vec3 Destination{0.0f};
};
```

- [ ] **Step 2: Register both in X-macro**

Modify `src/common/include/ECS.h` — append two new lines at the end of `ECS_FOR_EACH_REGISTERED_COMPONENT`. Current last line is `X(NavObstacleComponent)`. Append:

```cpp
    X(NavObstacleComponent) \
    X(NavAgentComponent) \
    X(NavTargetComponent)
```

- [ ] **Step 3: Bump GAME_API_VERSION**

Modify `src/game/include/game.h`:

```cpp
#define GAME_API_VERSION 17u  // was 16u
```

- [ ] **Step 4: Build to verify**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target ecs Engine editor game --config Debug
```

Expected: clean build. Explicit template instantiations for both new components emitted automatically via X-macro.

- [ ] **Step 5: Commit**

```bash
git add src/common/include/ECS.h src/game/include/game.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs): NavAgentComponent + NavTargetComponent + GAME_API 17

NavAgentComponent (MoveSpeed + Radius + ReachedEpsilon) is the
agent footprint + tuning. Radius is reserved for v2 cached-path
filter bucketing (unused in stateless v1). NavTargetComponent
(Destination) drives NavAgentSystem to pathfind + emit MoveIntent.

Both registered in the X-macro for explicit template instantiation.
GAME_API 16 -> 17 because ECS.h layout changed; editor + game must
be rebuilt + editor restarted."
```

---

## Task 2: JSON round-trip + test_worldserial T24/T25

**Files:**
- Modify: `src/common/include/ComponentSerialization.h`
- Modify: `src/engine/src/utilities/WorldManager.cpp`
- Modify: `tests/test_worldserial.cpp`

- [ ] **Step 1: Write failing tests first**

In `tests/test_worldserial.cpp`, inside the existing `Test_Navigation()` function (added in Spec 1 Task 2, extended in Spec 2 Task 2), append two new blocks at the end (before the closing `}`):

```cpp
    // T24: NavAgentComponent per-entity round-trip
    {
        NavAgentComponent agent{};
        agent.MoveSpeed      = 4.25f;
        agent.Radius         = 0.75f;
        agent.ReachedEpsilon = 0.20f;
        json j = agent;
        NavAgentComponent back = j.get<NavAgentComponent>();
        EXPECT(near(back.MoveSpeed,      4.25f));
        EXPECT(near(back.Radius,         0.75f));
        EXPECT(near(back.ReachedEpsilon, 0.20f));
    }

    // T25: NavTargetComponent per-entity round-trip
    {
        NavTargetComponent target{};
        target.Destination = glm::vec3(12.5f, 0.0f, -7.25f);
        json j = target;
        NavTargetComponent back = j.get<NavTargetComponent>();
        EXPECT(near(back.Destination.x,  12.5f));
        EXPECT(near(back.Destination.y,   0.0f));
        EXPECT(near(back.Destination.z, -7.25f));
    }
```

- [ ] **Step 2: Run test — expect compile failure**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_worldserial --config Debug
```

Expected: FAIL with `nlohmann::json` cannot serialize `NavAgentComponent` / `NavTargetComponent`.

- [ ] **Step 3: Add `to_json` / `from_json` for both components**

In `src/common/include/ComponentSerialization.h`, insert AFTER the existing `NavObstacleComponent` block (added in Spec 2 Task 2, around line 206):

```cpp
inline void to_json(nlohmann::json& j, const NavAgentComponent& t) {
    j = nlohmann::json{
        {"MoveSpeed",      t.MoveSpeed},
        {"Radius",         t.Radius},
        {"ReachedEpsilon", t.ReachedEpsilon}
    };
}
inline void from_json(const nlohmann::json& j, NavAgentComponent& t) {
    if (j.contains("MoveSpeed"))      j.at("MoveSpeed").get_to(t.MoveSpeed);
    if (j.contains("Radius"))         j.at("Radius").get_to(t.Radius);
    if (j.contains("ReachedEpsilon")) j.at("ReachedEpsilon").get_to(t.ReachedEpsilon);
}

inline void to_json(nlohmann::json& j, const NavTargetComponent& t) {
    j = nlohmann::json{
        {"Destination", t.Destination}
    };
}
inline void from_json(const nlohmann::json& j, NavTargetComponent& t) {
    if (j.contains("Destination")) j.at("Destination").get_to(t.Destination);
}
```

- [ ] **Step 4: WorldManager save side**

In `src/engine/src/utilities/WorldManager.cpp`, after the existing `NavObstacleComponent` save block (Spec 2 Task 2):

```cpp
        if (world->HasComponent<NavAgentComponent>(entity)) {
            jEntity["NavAgentComponent"] = *(world->GetComponent<NavAgentComponent>(entity));
        }
        if (world->HasComponent<NavTargetComponent>(entity)) {
            jEntity["NavTargetComponent"] = *(world->GetComponent<NavTargetComponent>(entity));
        }
```

- [ ] **Step 5: WorldManager load side**

After the existing `NavObstacleComponent` load:

```cpp
            if (jEntity.contains("NavAgentComponent"))
                world->AddComponent(createdEntity, jEntity["NavAgentComponent"].get<NavAgentComponent>());
            if (jEntity.contains("NavTargetComponent"))
                world->AddComponent(createdEntity, jEntity["NavTargetComponent"].get<NavTargetComponent>());
```

- [ ] **Step 6: Run test to verify pass**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_worldserial --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```

Expected: success line — T20-T25 all pass.

- [ ] **Step 7: Regression check**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_collision test_menu test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_collision.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: all pass.

- [ ] **Step 8: Commit**

```bash
git add src/common/include/ComponentSerialization.h src/engine/src/utilities/WorldManager.cpp tests/test_worldserial.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): JSON round-trip for NavAgent + NavTarget

Both components serialized per-entity in world.json. NavAgent
contains-guarded reads (lenient per NavMeshSource/NavObstacle
pattern — allows older saves to load with defaults). T24 + T25 in
test_worldserial pin both round-trips with non-default values."
```

---

## Task 3: ECSCommands dispatch — Apply/Remove/Duplicate for both components

**Files:**
- Modify: `src/common/include/ECSCommands.h`

- [ ] **Step 1: Add both to `ApplyComponentCommand`**

In `src/common/include/ECSCommands.h`, find the `ApplyComponentCommand` function. Append two new `else if` branches AFTER the `NavObstacleComponent` branch (from Spec 2 Task 3):

```cpp
        } else if (componentData.Type == std::type_index(typeid(NavAgentComponent))) {
            if (auto* a = componentData.Get<NavAgentComponent>()) {
                world.AddComponent(entity, *a);
            }
        } else if (componentData.Type == std::type_index(typeid(NavTargetComponent))) {
            if (auto* t = componentData.Get<NavTargetComponent>()) {
                world.AddComponent(entity, *t);
            }
        }
```

- [ ] **Step 2: Add both to `RemoveComponentByType`**

In the same file, find `RemoveComponentByType`. Append after the `NavObstacleComponent` branch:

```cpp
        } else if (typeIndex == std::type_index(typeid(NavAgentComponent))) {
            world.RemoveComponent<NavAgentComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(NavTargetComponent))) {
            world.RemoveComponent<NavTargetComponent>(entity);
        }
```

- [ ] **Step 3: Add both to `DuplicateEntityComponents`**

In the same file, find `DuplicateEntityComponents`. Append after the `NavObstacleComponent` line (from Spec 2 Task 3):

```cpp
        if (auto* c = world.GetComponent<NavAgentComponent>(src))        world.AddComponent(dst, *c);
        if (auto* c = world.GetComponent<NavTargetComponent>(src))       world.AddComponent(dst, *c);
```

- [ ] **Step 4: Build to verify**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target ecs Engine editor game --config Debug
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/common/include/ECSCommands.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs-cmds): NavAgent + NavTarget Apply/Remove/Duplicate dispatch

Standard plumbing so the inspector can add/edit/remove per-entity
and duplicated entities carry both. Mirrors NavObstacle pattern."
```

---

## Task 4: NavAgentSystem.h header + test_navmesh T14-T18

**Files:**
- Create: `src/game/src/NavAgentSystem.h` (header-only ISystem)
- Modify: `tests/test_navmesh.cpp` (helpers + T14-T18)

- [ ] **Step 1: Create `NavAgentSystem.h`**

```cpp
#pragma once

#include <algorithm>

#include <glm/glm.hpp>

#include "ECS.h"
#include "Systems.h"
#include "lib.h"   // SM_WARN (currently unused; reserved for future rate-limited no-path logging)

#include "navigation/NavMeshSystem.h"
#include "navigation/NavMesh.h"

// Simulation-phase system that drives nav-agent entities by writing
// MoveIntentComponent toward the next waypoint of a freshly-queried path.
//
// v1 STATELESS DESIGN:
//   Each tick: FindPath(transform, target) -> walk toward path[1] capped at
//   MoveSpeed * dt. No path cache. CPU cost = O(agents * FindPath).
//   Acceptable for ~10 agents on current scene scale.
//
// v2 UPGRADE PATH (when agent count > 20 or FindPath cost dominates):
//   Add cached path + PathIndex + TimeSinceRepath to NavAgentComponent.
//   Repath only on target-change OR PathIndex-stale OR time-elapsed.
//   See navigation-agents-design.md for migration notes.
class NavAgentSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto nav = NavMeshSystem::Instance().Current();
        if (!nav) return;  // no navmesh built yet — nothing to path
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
                const auto path = nav->FindPath(tr.Position, target.Destination);
                if (path.size() < 2) {
                    return;  // no path (unreachable / off-mesh start/end / FindPath fail)
                }

                // Walk toward path[1] (path[0] is start-position-snapped-to-navmesh).
                const glm::vec3 nextWaypoint = path[1].Position;
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
};
```

- [ ] **Step 2: Write failing tests in `tests/test_navmesh.cpp`**

Add the NavAgentSystem include near the top (after the existing `NavObstacleSync.h` include from Spec 2):

```cpp
#include "NavAgentSystem.h"
```

Below the existing test functions (after T13 — `T13_rebuild_clears_obstacle_map`), add the helpers + T14-T18:

```cpp
// ---------- Spec 3: NavAgent helpers + tests ----------

static EntityId SpawnAgent(ECS& w, const glm::vec3& pos, float speed = 3.0f) {
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ pos, glm::vec3(0.0f), glm::vec3(1.0f) });
    NavAgentComponent agent{};
    agent.MoveSpeed = speed;
    w.AddComponent(e, agent);
    return e;
}

static void RunAgentSystem(ECS& w, NavAgentSystem& sys, double dt = 0.016) {
    SystemContext ctx{ w, dt, 0.0 };
    sys.Update(ctx);
}

static void T14_agent_with_no_target_does_not_emit_intent() {
    ECS w;
    SpawnFloorAndBuild(w);
    SpawnAgent(w, glm::vec3(-2, 0.5f, 0));
    NavAgentSystem sys;
    RunAgentSystem(w, sys);
    // The Each gate requires NavTarget — no entity has MoveIntent.
    int intentCount = 0;
    w.Each<MoveIntentComponent>([&](EntityId){ ++intentCount; });
    EXPECT(intentCount == 0);
}

static void T15_agent_writes_intent_toward_path_waypoint() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnAgent(w, glm::vec3(-3, 0.5f, 0));
    w.AddComponent(e, NavTargetComponent{ glm::vec3(3, 0.5f, 0) });
    NavAgentSystem sys;
    RunAgentSystem(w, sys);

    auto* mi = w.GetComponent<MoveIntentComponent>(e);
    EXPECT(mi != nullptr);
    if (mi) {
        // Intent points roughly toward +X (target direction across open floor).
        EXPECT(mi->DesiredDelta.x > 0.0f);
        // Magnitude capped at MoveSpeed * dt = 3.0 * 0.016 = 0.048.
        const float mag = glm::length(mi->DesiredDelta);
        EXPECT(mag <= 3.0f * 0.016f + 1e-4f);
        EXPECT(mag > 0.0f);
    }
}

static void T16_agent_reached_target_stops_emitting() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnAgent(w, glm::vec3(0, 0.5f, 0));
    // Target within ReachedEpsilon (default 0.10) of position.
    w.AddComponent(e, NavTargetComponent{ glm::vec3(0.05f, 0.5f, 0) });
    NavAgentSystem sys;
    RunAgentSystem(w, sys);
    // No MoveIntent component should have been added (agent never emitted).
    EXPECT(!w.HasComponent<MoveIntentComponent>(e));
}

static void T17_agent_unreachable_target_no_intent() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnAgent(w, glm::vec3(0, 0.5f, 0));
    // Target way off the navmesh (far above floor).
    w.AddComponent(e, NavTargetComponent{ glm::vec3(0, 100.0f, 0) });
    NavAgentSystem sys;
    RunAgentSystem(w, sys);
    EXPECT(!w.HasComponent<MoveIntentComponent>(e));
}

static void T18_agent_routes_around_obstacle() {
    ECS w;
    SpawnFloorAndBuild(w);
    SpawnCylinderObstacle(w, glm::vec3(0, 0, 0), 1.5f, 2.0f);
    NavObstacleSyncSystem obsSync;
    RunSync(w, obsSync);
    DrainTileCache(NavMeshSystem::Instance());

    const EntityId e = SpawnAgent(w, glm::vec3(-3, 0.5f, 0));
    w.AddComponent(e, NavTargetComponent{ glm::vec3(3, 0.5f, 0) });
    NavAgentSystem sys;
    RunAgentSystem(w, sys);

    auto* mi = w.GetComponent<MoveIntentComponent>(e);
    EXPECT(mi != nullptr);
    if (mi) {
        // Path curves around the obstacle. First waypoint should NOT be straight
        // along +X (would mean walking into the cylinder). Z component should be
        // non-trivial (steering around).
        EXPECT(std::abs(mi->DesiredDelta.z) > 0.001f);
    }
}
```

Update `main()` — call T14-T18 after T13:

```cpp
    T13_rebuild_clears_obstacle_map();
    T14_agent_with_no_target_does_not_emit_intent();
    T15_agent_writes_intent_toward_path_waypoint();
    T16_agent_reached_target_stops_emitting();
    T17_agent_unreachable_target_no_intent();
    T18_agent_routes_around_obstacle();
```

- [ ] **Step 3: Build — expect tests to compile + pass**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: `All navmesh tests passed.` All 18 tests green (T01-T13 from Specs 1+2, T14-T18 new).

- [ ] **Step 4: Regression check**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_collision test_worldserial test_menu --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_collision.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
```

Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/game/src/NavAgentSystem.h tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): NavAgentSystem + agent-behavior tests

Header-only ISystem in Simulation phase. Stateless v1: each tick
calls NavMesh::FindPath(transform, target), walks toward path[1]
at MoveSpeed * dt capped at dirLen. Reached-epsilon early-out
skips FindPath for arrived agents (zero-cost stationary AI).
Lazy-seeds MoveIntentComponent on first move; subsequent ticks
Modify in place (matches PlayerMovementSystem from movement-
decoupling spec).

T14 no target → no intent. T15 open-floor path emits intent
toward target. T16 reached → silent. T17 unreachable → silent.
T18 cylinder forces curved path with non-trivial Z component.

v2 cached-path upgrade documented in spec — collapses FindPath
cost from O(agents/tick) to O(agents/2s) when agent count > 20."
```

---

## Task 5: ShowNavPaths debug viz + EditorPreferences

**Files:**
- Modify: `src/engine/src/rendering/RenderStats.h` (ShowNavPaths field)
- Modify: `src/editor/src/EditorPreferences.h` (round-trip "navpaths" key)
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` (checkbox)
- Modify: `src/engine/src/rendering/passes/DebugRenderPass.cpp` (ShowNavPaths block)

- [ ] **Step 1: Add `ShowNavPaths` field to `DebugDrawSettings`**

In `src/engine/src/rendering/RenderStats.h`, extend the struct:

```cpp
struct DebugDrawSettings {
    bool ShowLightGizmos   = false;
    bool ShowCameraFrustum = false;
    bool ShowSelectedAABB  = false;
    bool Wireframe         = false;
    bool ShowGrid          = false;
    bool ShowColliders     = false;
    bool ShowNavMesh       = false;
    bool ShowObstacles     = false;
    bool ShowNavPaths      = false;
};
```

- [ ] **Step 2: Round-trip in EditorPreferences**

In `src/editor/src/EditorPreferences.h`, extend `PrefsToJson` debugDraw block — add after `"obstacles"`:

```cpp
        {"debugDraw", {
            {"lightGizmos",   debug.ShowLightGizmos},
            {"cameraFrustum", debug.ShowCameraFrustum},
            {"selectedAABB",  debug.ShowSelectedAABB},
            {"wireframe",     debug.Wireframe},
            {"grid",          debug.ShowGrid},
            {"colliders",     debug.ShowColliders},
            {"navmesh",       debug.ShowNavMesh},
            {"obstacles",     debug.ShowObstacles},
            {"navpaths",      debug.ShowNavPaths},
        }},
```

Extend `PrefsFromJson` — add this line after the `obstacles` read:

```cpp
        if (d.contains("navpaths")      && d["navpaths"].is_boolean())      debug.ShowNavPaths      = d["navpaths"].get<bool>();
```

- [ ] **Step 3: Add Render Stats checkbox**

In `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`, append after the existing `Obstacles` checkbox:

```cpp
    changed |= ImGui::Checkbox("Nav Paths",      &dd.ShowNavPaths);
```

- [ ] **Step 4: Add ShowNavPaths block to `DebugRenderPass.cpp`**

In `src/engine/src/rendering/passes/DebugRenderPass.cpp`:

(a) Update the early-out chain to include the new toggle. Find:
```cpp
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid && !s.ShowColliders && !s.ShowNavMesh && !s.ShowObstacles)
        return;
```
Replace with:
```cpp
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid && !s.ShowColliders && !s.ShowNavMesh && !s.ShowObstacles && !s.ShowNavPaths)
        return;
```

(b) After the existing `if (s.ShowObstacles) { ... }` block (added in Spec 2 Task 7), append:

```cpp
    if (s.ShowNavPaths) {
        const auto nav = NavMeshSystem::Instance().Current();
        if (nav) {
            const glm::vec4 pathCol(1.0f, 1.0f, 0.3f, 1.0f);  // yellow — path lines
            const glm::vec4 destCol(1.0f, 1.0f, 1.0f, 1.0f);  // white  — destination markers
            std::vector<NavMesh::PathPoint> path;
            world->Each<NavAgentComponent, NavTargetComponent, TransformComponent>(
                [&](EntityId, const NavAgentComponent&,
                    const NavTargetComponent& target, const TransformComponent& tr) {
                    // Viz does its own FindPath — matches the stateless agent design.
                    // v2: agent caches path; viz reads cached vector instead.
                    path = nav->FindPath(tr.Position, target.Destination);
                    if (path.size() >= 2) {
                        for (size_t i = 0; i + 1 < path.size(); ++i) {
                            DebugAppendLine(m_Verts, path[i].Position, path[i+1].Position, pathCol);
                        }
                    }
                    DebugAppendSphere(m_Verts, target.Destination, 0.25f, destCol, 12);
                });
        }
    }
```

(NavMeshSystem.h + NavMesh.h includes are already in DebugRenderPass.cpp from Spec 1 Task 7 — verify before adding duplicates.)

- [ ] **Step 5: Build + run regression tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine editor test_editorprefs test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorprefs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: editor + Engine build clean. Both tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/rendering/RenderStats.h src/editor/src/EditorPreferences.h src/editor/src/rendering/imgui/RenderStatsPanel.cpp src/engine/src/rendering/passes/DebugRenderPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): ShowNavPaths debug viz (yellow lines + white destination markers)

ShowNavPaths joins ShowNavMesh + ShowObstacles in DebugDrawSettings,
persisted via editor_preferences.json under the existing debugDraw
block (additive — old prefs files load with ShowNavPaths=false).
Render Stats panel grows a Nav Paths checkbox.

DebugRenderPass iterates NavAgent + NavTarget + Transform entities,
calls NavMesh::FindPath, draws path as yellow connected line
segments + a white sphere at the destination. Viz does its own
FindPath (matches stateless agent design); v2 cached-path collapses
both. Yellow distinct from static-collider yellow by line-vs-
wireframe shape; white destination sphere unmistakable in palette."
```

---

## Task 6: NavAgent + NavTarget inspector UI

**Files:**
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.h` (member state for both)
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp` (Add/Remove menu + edit blocks)

- [ ] **Step 1: Add member state to `EcsInspectorPanel.h`**

In the `private:` section, immediately after the existing NavObstacle block (`editNavObstacle` / `lastEditedNavObstacleEntity` / `navObstacleModified` from Spec 2), append:

```cpp
    NavAgentComponent  editNavAgent{};
    EntityId           lastEditedNavAgentEntity = INVALID_ENTITY;
    bool               navAgentModified = false;

    NavTargetComponent editNavTarget{};
    EntityId           lastEditedNavTargetEntity = INVALID_ENTITY;
    bool               navTargetModified = false;
```

- [ ] **Step 2: Add Component menu entries**

In `EcsInspectorPanel.cpp`, find the existing "Add NavMesh Obstacle Component" block. Immediately after it (before the `ImGui::Separator();`), insert:

```cpp
                if (!ctx.WorldSnapshot->HasComponent<NavAgentComponent>(entity)) {
                    if (ImGui::MenuItem("Add NavMesh Agent Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, NavAgentComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<NavTargetComponent>(entity)) {
                    if (ImGui::MenuItem("Add NavMesh Target Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, NavTargetComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }
```

- [ ] **Step 3: Remove Component menu entries**

Find the existing "Remove NavMesh Obstacle Component" block. Immediately after it (before `ImGui::EndPopup();`), insert:

```cpp
                if (ctx.WorldSnapshot->HasComponent<NavAgentComponent>(entity)) {
                    if (ImGui::MenuItem("Remove NavMesh Agent Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<NavAgentComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<NavTargetComponent>(entity)) {
                    if (ImGui::MenuItem("Remove NavMesh Target Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<NavTargetComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }
```

- [ ] **Step 4: Per-component edit UI**

Find the existing "Edit NavMesh Obstacle Component" block. Immediately after its closing braces (before the `} else if (selectedEntity != INVALID_ENTITY) {`), insert:

```cpp
            // Edit NavMesh Agent Component
            if (ctx.WorldSnapshot->HasComponent<NavAgentComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("NavMesh Agent Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* a = ctx.WorldSnapshot->GetComponent<NavAgentComponent>(selectedEntity);
                    if (a) {
                        if (lastEditedNavAgentEntity != selectedEntity) {
                            editNavAgent = *a;
                            lastEditedNavAgentEntity = selectedEntity;
                            navAgentModified = false;
                        }
                        if (!navAgentModified) {
                            editNavAgent = *a;
                        }
                        if (ImGui::DragFloat("Move Speed",      &editNavAgent.MoveSpeed,      0.05f, 0.0f, 50.0f, "%.2f m/s")) navAgentModified = true;
                        if (ImGui::DragFloat("Radius",          &editNavAgent.Radius,         0.01f, 0.05f, 5.0f, "%.2f m"))   navAgentModified = true;
                        if (ImGui::DragFloat("Reached Epsilon", &editNavAgent.ReachedEpsilon, 0.01f, 0.01f, 2.0f, "%.2f m"))   navAgentModified = true;
                        ImGui::Spacing();
                        if (navAgentModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editNavAgent);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            navAgentModified = false;
                        }
                    }
                }
            }

            // Edit NavMesh Target Component
            if (ctx.WorldSnapshot->HasComponent<NavTargetComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("NavMesh Target Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* t = ctx.WorldSnapshot->GetComponent<NavTargetComponent>(selectedEntity);
                    if (t) {
                        if (lastEditedNavTargetEntity != selectedEntity) {
                            editNavTarget = *t;
                            lastEditedNavTargetEntity = selectedEntity;
                            navTargetModified = false;
                        }
                        if (!navTargetModified) {
                            editNavTarget = *t;
                        }
                        if (ImGui::InputFloat3("Destination", &editNavTarget.Destination.x)) navTargetModified = true;
                        ImGui::Spacing();
                        if (navTargetModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editNavTarget);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            navTargetModified = false;
                        }
                    }
                }
            }
```

- [ ] **Step 5: Build**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target editor --config Debug
```

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/rendering/imgui/EcsInspectorPanel.h src/editor/src/rendering/imgui/EcsInspectorPanel.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): NavAgent + NavTarget components in EcsInspectorPanel

Add Component / Remove Component menu entries + per-component edit
UI for both. NavAgent: DragFloat MoveSpeed (0.05 step, 0-50 m/s),
Radius (0.01 step, 0.05-5.0 m), ReachedEpsilon (0.01 step, 0.01-
2.0 m). NavTarget: InputFloat3 Destination. Mirrors the
NavObstacle inspector pattern (working-copy member + lastEdited
tracker + modified flag) and pushes ECSCommand::ModifyComponent
on changes."
```

---

## Task 7: Register NavAgentSystem + final whole-feature review

**Files:**
- Modify: `src/game/src/game.cpp` (include + system registration)

- [ ] **Step 1: Include sync system header**

In `src/game/src/game.cpp`, near the top of the file with other system/header includes (after the existing `#include "NavObstacleSync.h"` from Spec 2):

```cpp
#include "NavAgentSystem.h"
```

- [ ] **Step 2: Register system in `GameRegisterSystems`**

In `src/game/src/game.cpp::GameRegisterSystems`, locate the line that registers `PlayerMovementSystem`. The current Simulation+Physics block reads:
```cpp
    s->Register(std::make_unique<PlayerMovementSystem>());            // Simulation: writes MoveIntent
    s->Register(std::make_unique<KinematicMovementSystem>());         // Physics: resolves intent + applies Transform
    s->Register(std::make_unique<NavObstacleSyncSystem>());           // Physics: syncs NavObstacleComponent → dtTileCache
```

Insert `NavAgentSystem` BETWEEN PlayerMovement and KinematicMovement so the registration order within Simulation phase is PlayerMovement → NavAgent (nav wins for click-to-move on player; out-of-scope today but design accommodates it):

```cpp
    s->Register(std::make_unique<PlayerMovementSystem>());            // Simulation: writes MoveIntent from input
    s->Register(std::make_unique<NavAgentSystem>());                  // Simulation: writes MoveIntent from navmesh path
    s->Register(std::make_unique<KinematicMovementSystem>());         // Physics: resolves intent + applies Transform
    s->Register(std::make_unique<NavObstacleSyncSystem>());           // Physics: syncs NavObstacleComponent → dtTileCache
```

- [ ] **Step 3: Build + run all tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine editor game test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs --config Debug
for t in test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "All tests green."
```

Expected: each test prints its success line, final line "All tests green."

- [ ] **Step 4: Commit**

```bash
git add src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): register NavAgentSystem (Simulation phase, after PlayerMovement)

NavAgent writes MoveIntent second within Simulation phase, so if
an entity holds both PlayerComponent and NavAgentComponent the nav
intent wins (Modify overwrites). Enables future click-to-move on
player by adding NavTarget — no Spec 3 work, just authoring
composition.

KinematicMovementSystem (Physics) then consumes MoveIntent for
both player-driven and nav-driven entities through the same
intent → resolution pipeline."
```

- [ ] **Step 5: Dispatch final whole-feature reviewer**

Per `superpowers:subagent-driven-development`, dispatch the final reviewer subagent. Provide:
- Spec: `docs/superpowers/specs/2026-05-27-navigation-agents-design.md` (commit `02db951`)
- Full branch diff: `git diff main..feat/navigation-agents`
- All 7 commits on the branch (one per task).
- Per-task reviews summary (all APPROVED).

Reviewer verdict (READY TO MERGE / READY WITH NOTES / NEEDS CHANGES) drives merge-readiness. User does the GUI smoke before merge.

---

## Self-review notes

**Spec coverage check:**

- ✅ NavAgentComponent + NavTargetComponent + 2 X-macro entries — Task 1
- ✅ GAME_API_VERSION 16→17 — Task 1
- ✅ JSON round-trip both components — Task 2
- ✅ WorldManager save/load both — Task 2
- ✅ test_worldserial T24 + T25 — Task 2
- ✅ ECSCommands Apply/Remove/Duplicate for both — Task 3
- ✅ NavAgentSystem.h (stateless v1, Simulation phase, reached-epsilon early-out, FindPath, MoveIntent lazy-seed) — Task 4
- ✅ test_navmesh T14-T18 + SpawnAgent/RunAgentSystem helpers — Task 4
- ✅ ShowNavPaths toggle + EditorPreferences "navpaths" key + checkbox — Task 5
- ✅ DebugRenderPass ShowNavPaths block (yellow path lines + white destination markers, viz does own FindPath) — Task 5
- ✅ Inspector Add/Remove menu + edit UI for both components — Task 6
- ✅ NavAgentSystem registration after PlayerMovementSystem (Simulation phase) — Task 7
- ✅ Final whole-feature review — Task 7 Step 5

No gaps.

**Type-consistency check:**

- `NavAgentComponent` field set (MoveSpeed, Radius, ReachedEpsilon) — consistent across T1 (declaration), T2 (JSON), T4 (system reads MoveSpeed + ReachedEpsilon; Radius reserved), T6 (inspector all 3).
- `NavTargetComponent::Destination` — consistent T1 / T2 / T4 / T5 / T6.
- `NavMesh::FindPath(start, end)` signature — consistent T4 (system) / T5 (viz). Both return `std::vector<NavMesh::PathPoint>` and use `path.size() >= 2` check with `path[1].Position` access.
- `MoveIntentComponent::DesiredDelta` — consistent T4 (system writes). Lazy-seed pattern `HasComponent + AddComponent + Modify` matches Spec 1's movement-decoupling pattern.
- `SystemContext{ world, dt, gameTime }` — consistent T4 (test helper `RunAgentSystem`).
- `SystemPhase::Simulation` for NavAgentSystem — consistent T4 (declaration) / T7 (registration before KinematicMovementSystem).

**Placeholder scan:** No placeholders, no TBDs, no "implement later" markers. Every code step has complete code; every command shows expected output.

**Commit count:** 7 commits (Tasks 1-7, Task 0 administrative). Within spec's 5-7 estimate.
