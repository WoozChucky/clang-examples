# Navigation Obstacles — Design Spec

**Date:** 2026-05-27
**Status:** Approved
**Subspec of:** Recast/Detour integration (Spec 2 of ~3)
**Depends on:** `2026-05-27-navigation-core-design.md` (Spec 1, merged to main as `feat/navigation-core`)

---

## Goal

Wire `dtTileCache` dynamic obstacle support: a `NavObstacleComponent` (Cylinder or Box shape), a per-tick sync system that diffs ECS state against the tilecache and adds/removes/relocates obstacles, and a `ShowObstacles` debug viz. Spec 1 shipped TileCache from day 1 with `MaxObstacles` plumbed but unused — Spec 2 is purely additive, no rewrite of the build pipeline.

After Spec 2 ships you can: tag any entity with `NavObstacleComponent`, see it carve walkable area in the viewport, watch agents (Spec 3, when it lands) route around it. Projectile-spawned obstacles and runtime-placed cover both work through the same component path.

---

## Scope (Spec 2 within the larger decomposition)

- **Spec 1 — `navigation-core` (shipped):** build pipeline, NavMesh resource, query API, editor panel, debug viz.
- **Spec 2 — `navigation-obstacles` (this spec):** dynamic obstacle wiring.
- **Spec 3 — `navigation-agents`:** NavAgent + NavTarget + waypoint-follow system writing MoveIntent.
- **Spec 4 — `navigation-bake` (conditional):** disk persistence of `dtTileCache`.

---

## Architecture overview

**Code layout:**

```
src/engine/src/navigation/
  ├── NavMesh.{h,cpp}            # extend: Tick(dt) + AddObstacle/RemoveObstacle forwarders
  └── NavMeshSystem.{h,cpp}      # extend: Tick(dt), obstacle API, EntityId→ObstacleHandle map
src/common/include/ECS.h
  + NavObstacleShape enum + NavObstacleComponent + X-macro entry
src/common/include/ComponentSerialization.h
  + NavObstacleComponent round-trip
src/common/include/ECSCommands.h
  + NavObstacleComponent Apply/Remove/Duplicate dispatch
src/engine/src/utilities/WorldManager.cpp
  + NavObstacleComponent save/load loops
src/engine/src/rendering/DebugDraw.{h,cpp}
  + DebugAppendCylinder helper (new primitive)
src/engine/src/rendering/passes/DebugRenderPass.cpp
  + ShowObstacles block (DebugAppendCylinder + DebugAppendBox)
src/engine/src/rendering/RenderStats.h
  + ShowObstacles toggle field
src/editor/src/EditorPreferences.h
  + "obstacles" persistence key
src/editor/src/rendering/imgui/RenderStatsPanel.cpp
  + Obstacles checkbox
src/editor/src/rendering/imgui/EcsInspectorPanel.{h,cpp}
  + Add/Remove menu entries + per-component edit UI
src/engine/src/threading/GameThread.cpp
  + NavMeshSystem::Instance().Tick(dt) call once per tick
src/game/src/NavObstacleSync.h          # NEW — header-only system (testable like Collision.h)
src/game/src/game.cpp
  + Register NavObstacleSyncSystem (Physics phase)
src/game/include/game.h
  + GAME_API_VERSION 15→16
tests/test_navmesh.cpp
  + T09-T13 obstacle behavior tests
```

**Threading:**
- All `NavMeshSystem` state owned by GameThread. Sync system runs on GameThread (Physics phase). `dtTileCache::update` is single-threaded — safe because all callers live on GameThread.
- Published `shared_ptr<const NavMesh>` unchanged from Spec 1. RenderThread reads for viz only.
- Sync system queues add/remove calls each tick; `NavMeshSystem::Tick` then drives `dtTileCache::update` once at end of tick. Two-phase split keeps the sync focused on ECS diff while `Tick` stays usable from other call sites (e.g. Spec 3 agents may want to call Tick directly).

**Map invalidation on Rebuild:** `NavMeshSystem::Rebuild` clears the `EntityId → ObstacleHandle` map before atomic-store. Sync system on the next tick sees every entity's obstacle as "missing" and re-queues addObstacle calls. Two-tick window between rebuild and obstacle re-application — visible as brief uncarved-navmesh viz blip; functionally a non-issue because Rebuild is a deliberate editor action.

**GAME_API_VERSION:** 15 → 16 (ECS.h layout changed).

---

## ECS components

```cpp
// Recast TileCache obstacle shapes. Distinct from ColliderShape because:
//   - Recast has no Sphere obstacle (Cylinder is the radial primitive).
//   - Obstacle and collision domains may diverge (different footprints).
enum class NavObstacleShape : uint8_t {
    Cylinder = 0,   // dtTileCache::addObstacle(pos, radius, height)
    Box      = 1,   // dtTileCache::addBoxObstacle(bmin, bmax)
};

// Per-entity dynamic nav obstacle. When present, the sync system queues an
// addObstacle call to dtTileCache (carves walkable area). When removed (or
// the entity is destroyed), removeObstacle fires. When the entity's Transform
// moves > kPositionEpsilon, the obstacle is re-anchored (remove + re-add).
struct NavObstacleComponent {
    NavObstacleShape Shape = NavObstacleShape::Cylinder;
    // Cylinder: x = radius, y = height (z unused).
    // Box:      half-extents.
    glm::vec3 Size{0.5f, 1.0f, 0.5f};
    // Local-space center offset from Transform.Position (matches ColliderComponent).
    glm::vec3 Offset{0.0f};
};
```

X-macro entry + `ECSCommandProcessor::ApplyComponentCommand` + `RemoveComponentByType` + `DuplicateEntityComponents` dispatch + inspector UI + JSON round-trip. Same pattern as `NavMeshSourceComponent` shipped in Spec 1.

---

## `NavMeshSystem` extensions

```cpp
class ENGINE_API NavMeshSystem {
public:
    // ... existing Instance / Rebuild / Current ...

    // Drive dtTileCache update — applies queued add/removeObstacle calls,
    // re-bakes affected tiles. Single call per tick (GameThread).
    void Tick(float dt);

    // Queue an obstacle. Returns an opaque handle the caller stores (typically
    // via the EntityId→handle side table below). Returns 0 on failure.
    using ObstacleHandle = uint32_t;  // wraps dtObstacleRef
    ObstacleHandle AddCylinderObstacle(const glm::vec3& pos, float radius, float height);
    ObstacleHandle AddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax);
    void RemoveObstacle(ObstacleHandle h);

    // EntityId → ObstacleHandle mapping. Owned here so the map invalidates
    // correctly when Rebuild publishes a new NavMesh (old refs are invalid
    // against the new dtTileCache).
    void TrackObstacleForEntity(EntityId e, ObstacleHandle h);
    ObstacleHandle FindObstacleForEntity(EntityId e) const;
    void UntrackEntity(EntityId e);

    int ObstacleCount() const;  // for Navigation panel + tests

private:
    std::shared_ptr<const NavMesh> m_Current;
    std::unordered_map<EntityId, ObstacleHandle> m_EntityToObstacle;
};
```

**Why a side table (not on the component):** ECS components get snapshot-copied between threads. `dtObstacleRef` is only meaningful inside the GameThread-owned `dtTileCache`. Keeping the map engine-side avoids leaking a thread-local handle into a snapshot.

**`NavMesh::Tick(float dt, bool* upToDate)`** — new method on `NavMesh` (engine-internal). Calls `m_TileCache->update(dt, m_NavMesh, upToDate)`. `NavMeshSystem::Tick` snapshots its current `shared_ptr` and calls through. Single-call-per-tick policy: if `upToDate` is false, remaining work continues next tick.

**`NavMesh::AddObstacle / RemoveObstacle`** — thin forwarders to `m_TileCache->addObstacle / addBoxObstacle / removeObstacle`. Wrap `dtObstacleRef` (uint32_t) so the public `NavMeshSystem` API doesn't leak dtTileCache.

---

## `NavObstacleSyncSystem`

Lives in a new `src/game/src/NavObstacleSync.h` (header-only, so test_navmesh can include it directly — same precedent as `Collision.h`). Registered from `game.cpp::GameRegisterSystems` in Physics phase, after `KinematicMovementSystem`.

```cpp
class NavObstacleSyncSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        auto& nav = NavMeshSystem::Instance();
        if (!nav.Current()) return;  // no navmesh built yet — nothing to carve

        m_VisitedThisTick.clear();

        ctx.world.Each<NavObstacleComponent, TransformComponent>(
            [&](EntityId e, const NavObstacleComponent& obs, const TransformComponent& tr) {
                m_VisitedThisTick.insert(e);
                const glm::vec3 worldPos = tr.Position + obs.Offset;

                auto& prev = m_EntityCache[e];   // default-constructs to zero state
                const auto existing = nav.FindObstacleForEntity(e);

                const bool needsAdd     = (existing == 0);
                const bool moved        = glm::distance(prev.Position, worldPos) > kPositionEpsilon;
                const bool shapeChanged = prev.Shape != obs.Shape || prev.Size != obs.Size;
                const bool needsRebind  = !needsAdd && (moved || shapeChanged);

                if (needsRebind) {
                    nav.RemoveObstacle(existing);
                    nav.UntrackEntity(e);
                }
                if (needsAdd || needsRebind) {
                    NavMeshSystem::ObstacleHandle h = 0;
                    if (obs.Shape == NavObstacleShape::Cylinder) {
                        h = nav.AddCylinderObstacle(worldPos, obs.Size.x, obs.Size.y);
                    } else {
                        h = nav.AddBoxObstacle(worldPos - obs.Size, worldPos + obs.Size);
                    }
                    if (h != 0) {
                        nav.TrackObstacleForEntity(e, h);
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
                if (auto h = nav.FindObstacleForEntity(it->first); h != 0) {
                    nav.RemoveObstacle(h);
                    nav.UntrackEntity(it->first);
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
        glm::vec3        Position{0};
        NavObstacleShape Shape{};
        glm::vec3        Size{0};
    };
    std::unordered_map<EntityId, CachedState> m_EntityCache;
    std::unordered_set<EntityId>              m_VisitedThisTick;
    static constexpr float kPositionEpsilon = 0.05f;  // 5 cm — below default cell size 0.3m
};
```

**Why position-delta cache lives in the system, not in NavMeshSystem:** the diff is per-tick computation, not persistent engine state. NavMeshSystem owns only the live ref mapping. Keeps responsibilities clean.

**`kPositionEpsilon = 0.05f`** prevents tile re-bake thrash from sub-voxel float jitter. 5cm < default cell size 0.3m so it's well below voxel resolution.

**`NavMeshSystem::Tick(dt)` call site:** `GameThread.cpp`, single invocation after `ECSCommandProcessor::ProcessCommands` and all `ISystem::Update`s have run, before snapshot publish. Keeps the system focused on ECS diff while leaving `Tick` usable from other call sites (Spec 3 agents may want to call it without going through obstacle sync).

---

## Debug viz + `DebugAppendCylinder`

**New helper in `src/engine/src/rendering/DebugDraw.{h,cpp}`:**

```cpp
// Wireframe cylinder: top ring + bottom ring + N vertical edges. Y-axis aligned.
// `center` = bottom-center; height extends +Y. 12 segments by default = 72 line verts.
ENGINE_API void DebugAppendCylinder(std::vector<DebugVertex>& out,
                                    const glm::vec3& center, float radius, float height,
                                    const glm::vec4& color, int segments = 12);
```

**`ShowObstacles` toggle** mirrors `ShowNavMesh` (shipped Spec 1):
- `RenderStats.h` DebugDrawSettings: `bool ShowObstacles = false;`
- `EditorPreferences.h` round-trip: `"obstacles"` key in debugDraw block.
- `RenderStatsPanel.cpp`: `changed |= ImGui::Checkbox("Obstacles", &dd.ShowObstacles);` after NavMesh checkbox.
- `DebugRenderPass.cpp`: extend early-out + new block after ShowNavMesh:

```cpp
if (s.ShowObstacles) {
    const glm::vec4 col(1.0f, 0.3f, 0.9f, 1.0f);  // magenta — distinct from rest of palette
    world->Each<NavObstacleComponent, TransformComponent>(
        [&](EntityId, const NavObstacleComponent& obs, const TransformComponent& tr) {
            const glm::vec3 worldPos = tr.Position + obs.Offset;
            if (obs.Shape == NavObstacleShape::Cylinder) {
                DebugAppendCylinder(m_Verts, worldPos, obs.Size.x, obs.Size.y, col);
            } else {
                DebugAppendBox(m_Verts, worldPos, obs.Size, col);
            }
        });
}
```

**Color rationale:** Magenta `(1.0, 0.3, 0.9)` is the open hue slot — palette so far is cyan (trigger collider), yellow (static collider + selection AABB), orange (dynamic collider), lime green (navmesh polys), grey/white (grid).

---

## Editor inspector UI

Mirror the NavMeshSource pattern shipped in Spec 1 follow-up (`8cc6e67`). In `EcsInspectorPanel.{h,cpp}`:

**Header** — add member state next to NavMeshSource block:

```cpp
NavObstacleComponent editNavObstacle{};
EntityId             lastEditedNavObstacleEntity = INVALID_ENTITY;
bool                 navObstacleModified = false;
```

**Add Component menu** entry, after NavMeshSource:

```cpp
if (!ctx.WorldSnapshot->HasComponent<NavObstacleComponent>(entity)) {
    if (ImGui::MenuItem("Add NavMesh Obstacle Component")) {
        ECSCommand cmd = ECSCommand::AddComponent(entity, NavObstacleComponent{});
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("ECS command queue full! Add component command dropped.");
        }
    }
}
```

**Remove Component menu** entry, after NavMeshSource remove:

```cpp
if (ctx.WorldSnapshot->HasComponent<NavObstacleComponent>(entity)) {
    if (ImGui::MenuItem("Remove NavMesh Obstacle Component")) {
        ECSCommand cmd = ECSCommand::RemoveComponent<NavObstacleComponent>(entity);
        if (!ctx.App->ECSCommandRing.Push(cmd)) { SM_WARN("..."); }
    }
}
```

**Edit UI** — after the NavMeshSource edit block:

```cpp
if (ctx.WorldSnapshot->HasComponent<NavObstacleComponent>(selectedEntity)) {
    if (ImGui::CollapsingHeader("NavMesh Obstacle Component", ImGuiTreeNodeFlags_DefaultOpen)) {
        // working-copy refresh, snapshot-on-entity-change, modified flag — mirror ColliderComponent
        static const char* kShapeNames[] = { "Cylinder", "Box" };
        int idx = (editNavObstacle.Shape == NavObstacleShape::Box) ? 1 : 0;
        if (ImGui::Combo("Shape", &idx, kShapeNames, 2)) {
            editNavObstacle.Shape = (idx == 1) ? NavObstacleShape::Box : NavObstacleShape::Cylinder;
            navObstacleModified = true;
        }
        const char* sizeLabel = (editNavObstacle.Shape == NavObstacleShape::Cylinder)
            ? "Size (X=radius, Y=height)"
            : "Size (half-extents)";
        if (ImGui::InputFloat3(sizeLabel, &editNavObstacle.Size.x)) navObstacleModified = true;
        if (ImGui::InputFloat3("Offset", &editNavObstacle.Offset.x)) navObstacleModified = true;
        if (navObstacleModified) {
            ECSCommand cmd = ECSCommand::ModifyComponent(selectedEntity, editNavObstacle);
            if (!ctx.App->ECSCommandRing.Push(cmd)) { SM_WARN("..."); }
            navObstacleModified = false;
        }
    }
}
```

---

## Testing

**`tests/test_navmesh.cpp` new tests:**

```cpp
// Helper: drives dtTileCache::update until upToDate so test assertions see the
// post-carve navmesh. Bounded loop to avoid hangs on bugs.
static void DrainTileCache(NavMeshSystem& nav, int maxTicks = 16) {
    for (int i = 0; i < maxTicks; ++i) nav.Tick(0.016f);
}

static void T09_cylinder_obstacle_blocks_path();         // Add cylinder → path curves
static void T10_box_obstacle_blocks_path();              // Add box → path curves
static void T11_remove_obstacle_restores_path();         // Remove component → path straight again
static void T12_obstacle_position_change_triggers_rebind(); // Move obstacle → path adapts
static void T13_rebuild_clears_obstacle_map();           // After Rebuild, sync re-adds obstacle
```

All tests follow the pattern: spawn a floor + obstacle scenario → run `NavObstacleSyncSystem::Update` manually → call `DrainTileCache` → assert `FindPath` waypoint count.

`NavObstacleSyncSystem` lives in `NavObstacleSync.h` (header-only) so test_navmesh includes it directly — same precedent as `Collision.h`.

---

## Risks

1. **dtTileCache::update bounded work.** Recast caps obstacle processing per `update()` call (~64 internal). A burst of 100+ obstacles would lag several ticks. Ship single-call-per-tick; switch to drain-loop policy if measured insufficient.

2. **Per-tile rebuild cost.** Default tile size 32 voxels × cell 0.3m = 9.6m per tile. A single obstacle typically invalidates 1-4 tiles. Per-tile rebuild is sub-millisecond. Profile if rebuild storms appear.

3. **Map invalidation on Rebuild causes 1-tick obstacle flicker.** Rebuild clears the EntityId→handle map; sync re-adds obstacles next tick; `dtTileCache::update` applies them the tick after. Two-tick window where rebuilt navmesh is uncarved. Visible in viz; functionally fine since Rebuild is a deliberate editor action.

4. **NavObstacle on entities without Transform.** Sync system's `Each<NavObstacle, Transform>` silently skips them. Add `SM_WARN("NavObstacle entity X has no Transform; skipped")` via a separate `Each<NavObstacle>` check — matches NavMeshBuilder's SM_WARN style.

5. **MaxObstacles overflow.** `dtTileCache::addObstacle` returns failure when the cap is hit. `NavMeshSystem::AddCylinder/Box` returns 0; sync logs `SM_WARN` on failed add. Tunable via `NavMeshConfigComponent::MaxObstacles` in the Navigation panel.

---

## Out-of-scope (explicit deferral)

- **OrientedBox obstacles** — ships only Cylinder + axis-aligned Box. `addOrientedBoxObstacle` + OrientedBox shape value land when rotated cover / fallen trees appear.
- **Agent runtime (NavAgent, NavTarget, path follow)** — Spec 3.
- **Disk bake** — Spec 4 (conditional).
- **Obstacle priority / area-cost overrides** — Recast obstacles always set the affected area to `RC_NULL_AREA`. Per-obstacle area-overrides (e.g., "this obstacle marks the area as 'mud' = walkable but slow") = future feature.
- **Async / multi-threaded TileCache update** — single-threaded per Detour docs. Keep on GameThread.
- **Special tooling for runtime-spawned obstacle persistence** — current design persists `NavObstacleComponent` like any other component; user must explicitly save mid-run to capture runtime spawns. No special "save scene state" tooling.
- **MeshSystem-from-GameThread (Spec 1 carryover)** — independent concern, doesn't block this spec. Folds into Spec 3.

---

## File change summary

**New:**
- `src/game/src/NavObstacleSync.h`

**Modified:**
- `src/common/include/ECS.h` (+ enum + component + X-macro entry)
- `src/common/include/ComponentSerialization.h` (per-entity round-trip)
- `src/common/include/ECSCommands.h` (Apply/Remove/Duplicate dispatch)
- `src/engine/src/utilities/WorldManager.cpp` (save/load loop)
- `src/engine/src/navigation/NavMesh.{h,cpp}` (+ Tick + AddObstacle/RemoveObstacle forwarders)
- `src/engine/src/navigation/NavMeshSystem.{h,cpp}` (+ Tick + obstacle API + EntityId→handle map)
- `src/engine/src/threading/GameThread.cpp` (call NavMeshSystem::Tick once per tick)
- `src/engine/src/rendering/DebugDraw.{h,cpp}` (+ DebugAppendCylinder)
- `src/engine/src/rendering/passes/DebugRenderPass.cpp` (+ ShowObstacles block)
- `src/engine/src/rendering/RenderStats.h` (+ ShowObstacles bool)
- `src/editor/src/EditorPreferences.h` (+ "obstacles" round-trip)
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` (+ checkbox)
- `src/editor/src/rendering/imgui/EcsInspectorPanel.{h,cpp}` (+ menu entries + edit UI)
- `src/game/src/game.cpp` (register NavObstacleSyncSystem)
- `src/game/include/game.h` (GAME_API_VERSION 15→16)
- `tests/test_navmesh.cpp` (+ T09-T13)

**Commit estimate:** 5-7 commits. Smaller than Spec 1 because the foundation is in place.
