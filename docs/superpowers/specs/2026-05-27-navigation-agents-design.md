# Navigation Agents — Design Spec

**Date:** 2026-05-27
**Status:** Approved
**Subspec of:** Recast/Detour integration (Spec 3 of ~3 — final functional spec; Spec 4 disk bake is conditional polish)
**Depends on:**
- `2026-05-27-navigation-core-design.md` (Spec 1 — NavMesh + NavMeshSystem + FindPath, merged as `feat/navigation-core`).
- `2026-05-27-navigation-obstacles-design.md` (Spec 2 — dynamic obstacles, merged as `feat/navigation-obstacles`).
- `2026-05-27-movement-intent-decoupling-design.md` (MoveIntentComponent + KinematicMovementSystem in Physics phase).

---

## Goal

Connect the navmesh + obstacle stack to the existing intent-driven movement pipeline. A `NavAgentComponent` + `NavTargetComponent` pair on any entity drives a Simulation-phase `NavAgentSystem` that calls `NavMesh::FindPath` per tick and writes `MoveIntentComponent` toward the next waypoint. `KinematicMovementSystem` (Physics) then resolves intent against colliders + applies Transform — same path as player movement. After Spec 3 ships, any ECS entity becomes a nav-driven agent by adding two components and setting a destination.

---

## Scope (Spec 3 within the larger decomposition)

- **Spec 1 — `navigation-core` (shipped):** build pipeline, NavMesh resource, query API, editor panel, debug viz.
- **Spec 2 — `navigation-obstacles` (shipped):** dynamic obstacle wiring.
- **Spec 3 — `navigation-agents` (this spec):** NavAgent + NavTarget + sync system + path viz.
- **Spec 4 — `navigation-bake` (conditional polish):** disk persistence of `dtTileCache`.

---

## Architecture overview

**Code layout:**

```
src/common/include/ECS.h
  + NavAgentComponent (MoveSpeed + Radius + ReachedEpsilon)
  + NavTargetComponent (vec3 Destination)
  + 2 X-macro entries
src/common/include/ComponentSerialization.h
  + Both components' JSON round-trip
src/common/include/ECSCommands.h
  + Both components' Apply/Remove/Duplicate dispatch
src/engine/src/utilities/WorldManager.cpp
  + Both components save/load loops
src/game/src/NavAgentSystem.h           # NEW — header-only ISystem (Simulation phase)
src/game/src/game.cpp
  + Register NavAgentSystem (Simulation phase, after PlayerMovementSystem)
src/game/include/game.h
  + GAME_API_VERSION 16 → 17
src/engine/src/rendering/RenderStats.h
  + ShowNavPaths toggle field
src/editor/src/EditorPreferences.h
  + "navpaths" persistence key
src/editor/src/rendering/imgui/RenderStatsPanel.cpp
  + Nav Paths checkbox
src/engine/src/rendering/passes/DebugRenderPass.cpp
  + ShowNavPaths block (yellow line segments + white destination spheres)
src/editor/src/rendering/imgui/EcsInspectorPanel.{h,cpp}
  + Add/Remove menu entries + per-component edit UI for both components
tests/test_navmesh.cpp
  + T14-T18 agent behavior tests + SpawnAgent / RunAgentSystem helpers
tests/test_worldserial.cpp
  + T24 NavAgent round-trip + T25 NavTarget round-trip
```

**Threading + phase ordering:**

```
Simulation phase:
  PlayerMovementSystem      → writes MoveIntent from input (player only)
  NavAgentSystem (NEW)      → writes MoveIntent from navmesh path (NavAgent entities)

Physics phase:
  KinematicMovementSystem   → consumes MoveIntent, resolves vs colliders, applies Transform
  NavObstacleSyncSystem     → mirrors NavObstacle entities to dtTileCache (Spec 2)

PostSimulation phase:
  CameraZoomSystem
  IsometricFollowCameraSystem  → reads post-resolved Transform
```

NavAgentSystem registers after PlayerMovementSystem within Simulation phase. Registration order matters only for entities holding BOTH `PlayerComponent` and `NavAgentComponent` — NavAgent writes second, so nav wins (Modify overwrites). This is intentional: enables click-to-move on the player by adding `NavTarget` (out of scope for Spec 3 but the design accommodates it).

**Game→Engine link** established in Spec 2 (for `NavObstacleSync.h` to call `NavMeshSystem::Instance()`) — Spec 3 reuses it for `NavAgentSystem.h`. No new CMake plumbing.

**GAME_API_VERSION:** 16 → 17 (ECS.h layout changed).

---

## Stateless v1 vs cached-path v2

**Spec 3 ships STATELESS v1:** each tick, NavAgentSystem calls `NavMesh::FindPath(transform.Position, target.Destination)` for every NavAgent+NavTarget+Transform entity. No path cache, no PathIndex tracking, no per-tick repath logic. CPU cost = O(agents × FindPath).

**Why stateless:**
- Pure reader semantics — system never mutates ECS state beyond MoveIntent.
- Trivially correct under all obstacle/navmesh changes (next FindPath sees post-change world).
- Tiny implementation surface — easy to review, easy to test.
- Acceptable for current scene scale (≤10 agents at 60Hz → ~60ms/sec FindPath budget).

**Documented v2 upgrade path** (for when agent count > ~20 or FindPath cost dominates the frame):

```cpp
struct NavAgentComponent {
    float MoveSpeed       = 3.0f;
    float Radius          = 0.5f;
    float ReachedEpsilon  = 0.10f;

    // v2 additions:
    std::vector<glm::vec3> CachedPath;        // last FindPath result
    int                    PathIndex   = 0;   // current waypoint
    glm::vec3              LastTarget{0.0f};  // for target-change detection
    float                  TimeSinceRepath = 0.0f;
};
```

v2 system: check `LastTarget != target.Destination` OR `PathIndex >= CachedPath.size()` OR `TimeSinceRepath > kRepathInterval` (e.g., 0.5s) before re-querying. Collapses FindPath frequency from O(agents/tick) to ~O(agents/2 seconds). Strict requirement at 20+ agents on current scene scale.

The stateless v1 NavAgentComponent reserves `Radius` and `ReachedEpsilon` fields even though only ReachedEpsilon is used in v1 — Radius pre-allocates the slot for v2 path-filter bucketing without a GAME_API_VERSION bump.

---

## ECS components

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

Both registered in `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro + standard `ECSCommandProcessor` dispatch (Apply/Remove/Duplicate) + JSON round-trip + inspector UI. Same plumbing pattern as NavMeshSourceComponent (Spec 1) and NavObstacleComponent (Spec 2).

---

## NavAgentSystem

Lives in `src/game/src/NavAgentSystem.h` (header-only, mirrors `NavObstacleSync.h` from Spec 2 — testable from test_navmesh without dragging in game.cpp's other systems). Registered from `game.cpp::GameRegisterSystems` after `PlayerMovementSystem` (both Simulation phase).

```cpp
class NavAgentSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto nav = NavMeshSystem::Instance().Current();
        if (!nav) return;  // no navmesh built yet
        const float dt = static_cast<float>(ctx.dt);

        ctx.world.Each<NavAgentComponent, NavTargetComponent, TransformComponent>(
            [&](EntityId e, const NavAgentComponent& agent,
                const NavTargetComponent& target, const TransformComponent& tr) {

                // Reached check first — arrived agents skip FindPath entirely.
                const glm::vec3 toTarget = target.Destination - tr.Position;
                const float dist2 = glm::dot(toTarget, toTarget);
                if (dist2 < agent.ReachedEpsilon * agent.ReachedEpsilon) return;

                // Stateless query — every tick. v2 caches this.
                const auto path = nav->FindPath(tr.Position, target.Destination);
                if (path.size() < 2) return;  // unreachable / off-mesh / FindPath fail

                const glm::vec3 nextWaypoint = path[1].Position;
                const glm::vec3 dir = nextWaypoint - tr.Position;
                const float dirLen = glm::length(dir);
                if (dirLen < 1e-5f) return;

                const float stepLen = std::min(agent.MoveSpeed * dt, dirLen);
                const glm::vec3 desiredDelta = (dir / dirLen) * stepLen;

                // Lazy-seed MoveIntent on first move (same pattern as PlayerMovementSystem).
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

**Key invariants:**
- Pure reader of NavAgentComponent + NavTargetComponent + TransformComponent.
- Only writes MoveIntentComponent.
- `path[0]` is the start position snapped to nearest navmesh poly — `path[1]` is the first real waypoint. Agent walks toward `path[1]` (ignoring `path[0]` because it's effectively current position).
- `stepLen` capped to `dirLen` so agents never overshoot a waypoint within a tick.
- ReachedEpsilon check before FindPath — arrived agents skip the query entirely.

---

## ShowNavPaths debug viz

Same toggle pattern as Spec 1 (ShowNavMesh) + Spec 2 (ShowObstacles):
- `RenderStats.h::DebugDrawSettings`: `bool ShowNavPaths = false;`
- `EditorPreferences.h`: `"navpaths"` round-trip in debugDraw block.
- `RenderStatsPanel.cpp`: `changed |= ImGui::Checkbox("Nav Paths", &dd.ShowNavPaths);` after Obstacles checkbox.
- `DebugRenderPass.cpp`: extend early-out + new block after ShowObstacles:

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

**Color rationale:** yellow `(1.0, 1.0, 0.3)` for path lines reads as "path" universally — close to but distinguishable from static-collider yellow `(1.0, 0.85, 0.10)` (lines vs. wireframe shape disambiguates). White destination sphere is unmistakable.

---

## Editor inspector UI

Mirrors the NavObstacle inspector pattern from Spec 2 (commit `65e2250`). Two new collapsing headers — one each for NavAgent and NavTarget. Member state, Add/Remove menu entries, and edit UI follow the canonical working-copy + lastEdited + modified-flag pattern.

NavAgent edit fields: DragFloat MoveSpeed (0.05 step, 0-50 m/s), Radius (0.01 step, 0.05-5.0 m), ReachedEpsilon (0.01 step, 0.01-2.0 m).

NavTarget edit field: InputFloat3 Destination.

---

## Testing

**`tests/test_navmesh.cpp` new tests (T14-T18):**

- **T14** Agent with no NavTarget → no MoveIntent emitted (Each gate works).
- **T15** Agent with target on open floor → MoveIntent points toward target (+X direction), magnitude ≤ MoveSpeed × dt.
- **T16** Agent inside ReachedEpsilon of target → no MoveIntent emitted.
- **T17** Agent with off-navmesh target (Y=100m above floor) → FindPath returns empty, no MoveIntent.
- **T18** Agent + obstacle on direct path → MoveIntent has non-trivial Z component (steering around the obstacle, not straight +X).

Helper: `SpawnAgent(world, pos, speed)` adds Transform + NavAgent. `RunAgentSystem(world, sys, dt)` constructs SystemContext and calls Update.

**`tests/test_worldserial.cpp` additions:**

- **T24** NavAgentComponent round-trip with non-default MoveSpeed/Radius/ReachedEpsilon.
- **T25** NavTargetComponent round-trip with non-default Destination.

`NavAgentSystem.h` include path is already configured in `tests/CMakeLists.txt` from Spec 2 (`${CMAKE_SOURCE_DIR}/src/game/src`) — no CMake change needed.

---

## Risks

1. **Stateless FindPath cost.** O(agents × FindPath) per tick. At ≤10 agents on current scene scale: ~60ms/sec budget out of 16.67ms/frame × 60 — fine. Above ~20 agents: budget exhausted. Mitigation: ship v1, profile, upgrade to cached-path v2 (documented). Not a Spec 3 blocker.

2. **ShowNavPaths viz doubles FindPath cost** when toggled on. Same workload-doubling as Spec 2's ShowObstacles iteration. Debug-only.

3. **No FAILED_NO_PATH telemetry.** Silent skip on unreachable targets diverges from the "log degradation, not silent skip" memory rule. Trade: AI commonly targets transiently-unreachable spots (tracking player through line-of-sight loss). Logging every miss → noise. Accepted; if AI debugging needs it, add rate-limited per-entity warning (e.g., first miss per second).

4. **Agent + Collider double-resolution.** Agent's MoveIntent goes through KinematicMovementSystem which also resolves against ColliderComponent. Two "don't-walk-through-walls" layers (navmesh routing + collider blocking). Complementary in practice; no risk. Agent without a Collider works too (purely nav-routed, no collision response).

5. **path[1] vs path[0] selection.** Assumes path[0] is start-position-snapped-to-navmesh. If FindPath returns a path where path[0] is meaningfully away from current Transform (agent off-navmesh), agent walks toward path[1] which may be further-than-expected. Unlikely if agents stay on navmesh; if seen in practice, add path[0]-snap check.

---

## Out-of-scope (explicit deferral)

- **Cached path on NavAgent (v2 upgrade).** Documented as planned upgrade; ship stateless v1.
- **DetourCrowd multi-agent ORCA avoidance** — future when crowd density warrants. Library not even linked (Spec 1 deliberately omitted DetourCrowd from `target_link_libraries`).
- **Multi-agent-class navmeshes** (small dog vs human vs vehicle) — future, requires list-of-navmeshes + `AgentSizeClass` tag.
- **Off-mesh connections** (jumps, ladders, teleports) — future, requires `NavOffMeshConnection` component + Recast off-mesh API wiring.
- **Click-to-move on player** — game-side feature. Trivial after Spec 3: add NavAgent + NavTarget to player entity; NavAgentSystem registered after PlayerMovementSystem means nav-driven intent wins when target present. No Spec 3 work needed; just author-side composition.
- **Explicit "repath on block" detection** — stateless system already re-queries every tick, so block detection is automatic (next FindPath sees the changed world). v2 cached-path version needs explicit detection.
- **Async / multi-threaded pathing** — query stays on GameThread (Spec 1's dtNavMeshQuery thread-id assert enforces this).
- **Path smoothing beyond Detour string-pulling** — string-pulling is built into FindPath; no extra smoothing.

---

## File change summary

**New:** `src/game/src/NavAgentSystem.h`.

**Modified:** ECS.h (+2 components + 2 X-macro entries) · ComponentSerialization.h (2 round-trips) · ECSCommands.h (2 × Apply/Remove/Duplicate) · WorldManager.cpp (2 × save/load) · game.cpp (include + registration) · game.h (GAME_API_VERSION 17) · RenderStats.h (ShowNavPaths field) · EditorPreferences.h ("navpaths" round-trip) · RenderStatsPanel.cpp (checkbox) · DebugRenderPass.cpp (viz block) · EcsInspectorPanel.{h,cpp} (2 × menu+edit blocks) · test_navmesh.cpp (T14-T18 + helpers) · test_worldserial.cpp (T24+T25).

**Commit estimate:** 5-7 commits. Smaller than Spec 2 because no new CMake plumbing needed (game→Engine link landed in Spec 2).
