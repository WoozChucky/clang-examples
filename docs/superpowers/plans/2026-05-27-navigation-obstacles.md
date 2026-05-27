# Navigation Obstacles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire `dtTileCache` dynamic obstacle support so runtime entities (projectile debris, placed cover, falling boulders) carve walkable area on the navmesh without rebuilding it.

**Architecture:** New `NavObstacleComponent` (Cylinder or Box) authored or game-spawned. A Physics-phase `NavObstacleSyncSystem` diffs ECS state against an `EntityId → ObstacleHandle` map owned by `NavMeshSystem`, queues `dtTileCache::addObstacle/removeObstacle` on changes, tolerates position deltas, GCs disappeared entities. `NavMeshSystem::Tick(dt)` called once per GameThread tick drives `dtTileCache::update` to actually apply queued changes + re-bake affected tiles.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), NVRHI deferred renderer, Recast/Detour libs already vendored at `third_party/recastnavigation/` (Spec 1 linked DetourTileCache). nlohmann/json for world.json persistence.

**Spec reference:** `docs/superpowers/specs/2026-05-27-navigation-obstacles-design.md` (commit `0bf01f1`).

---

## Codebase orientation (read once before Task 1)

- **Spec 1 (`feat/navigation-core`) shipped** to main as merge `3801e00`. NavMesh / NavMeshBuilder / NavMeshSystem live in `src/engine/src/navigation/`. `dtTileCache` already built day-1 with `MaxObstacles` (from `NavMeshConfigComponent`) passed to `dtTileCacheParams::maxObstacles` — obstacle wiring is purely additive.
- **`DebugDraw.h` is header-only** (no .cpp file). `DebugAppendCylinder` goes inline in the header alongside existing helpers. The existing `DebugAppendCapsule` (lines 52-84) is the closest pattern — copy + simplify (no spherical caps, no top/bottom domes).
- **`DebugAppendBox` signature is `(mn, mx)` not `(center, halfExtents)`** — see `DebugDraw.h:19`. Box obstacle viz must compute `worldPos - obs.Size` / `worldPos + obs.Size` before calling.
- **`dtTileCache::addObstacle / addBoxObstacle / removeObstacle / update` signatures** are in `third_party/recastnavigation/DetourTileCache/Include/DetourTileCache.h:137-156`. All return `dtStatus`. `addObstacle` outputs the `dtObstacleRef` via the last arg.
- **Inspector pattern for adding NavObstacleComponent menu entries + edit UI** — mirror the NavMeshSource block landed in `8cc6e67` (now on main via the nav-core merge). `EcsInspectorPanel.h` per-component member state at line 61-63 (`editNavSource` / `lastEditedNavSourceEntity` / `navSourceModified`). Add/Remove menu entries at lines ~214 and ~314. Edit block after the NavMeshSource edit block.
- **`ECSCommandHooks` pattern** (Spec 1) keeps engine-private types out of `ECSCommands.h`. We do NOT need a new hook — `NavMeshSystem::Tick` is called directly from `GameThread.cpp`, not via the command ring.
- **`ISystem` registration order** in `game.cpp::GameRegisterSystems`: per-tick systems land in registration order within a phase. `NavObstacleSyncSystem` (Physics) goes AFTER `KinematicMovementSystem` (also Physics) — so by the time it runs, mover entities are at their post-resolution positions.
- **Test harness:** `tests/test_navmesh.cpp` already has `SpawnNavBox` helper + `DefaultCfg`. Add `DrainTileCache` helper + T09-T13.
- **GameThread tick loop:** `src/engine/src/threading/GameThread.cpp` around line 218 calls `ECSCommandProcessor::ProcessCommands` then `Game_GameUpdate` (which runs all ISystems). `NavMeshSystem::Tick(dt)` slots in after the game update, before snapshot publish.

---

## Task 0: Create feature branch

**Files:** none (git only)

- [ ] **Step 1: Verify clean main**

```bash
git status -sb
# Expected: "## main...origin/main" with no uncommitted changes (.claude/ untracked is fine).
git log --oneline -3
# Expected: 0bf01f1 (Spec 2 spec) + 3801e00 (nav-core merge) + earlier.
```

- [ ] **Step 2: Create branch**

```bash
git checkout -b feat/navigation-obstacles
git status -sb
# Expected: "## feat/navigation-obstacles"
```

- [ ] **Step 3: No commit yet** — administrative only.

---

## Task 1: ECS components — NavObstacleShape + NavObstacleComponent + GAME_API bump

**Files:**
- Modify: `src/common/include/ECS.h` (add enum + struct + X-macro entry)
- Modify: `src/game/include/game.h` (GAME_API_VERSION 15 → 16)

- [ ] **Step 1: Add `NavObstacleShape` enum + component in `ECS.h`**

Add IMMEDIATELY AFTER the existing `NavMeshConfigComponent` struct (which ends around line 283, before the X-macro comment block):

```cpp
// Recast TileCache obstacle shapes. Distinct from ColliderShape because Recast has
// no Sphere obstacle (Cylinder is the radial primitive) and obstacle/collision
// footprints may diverge (e.g., gameplay buffer wider than visual collision).
enum class NavObstacleShape : uint8_t {
    Cylinder = 0,   // dtTileCache::addObstacle(pos, radius, height)
    Box      = 1,   // dtTileCache::addBoxObstacle(bmin, bmax)
};

// Per-entity dynamic nav obstacle. Carves walkable area on the live navmesh via
// dtTileCache. Sync system queues addObstacle when the component appears,
// removeObstacle when it disappears, remove + re-add when Transform.Position
// moves > epsilon. Persisted in world.json so authors can place permanent
// obstacles in the editor.
struct NavObstacleComponent {
    NavObstacleShape Shape = NavObstacleShape::Cylinder;
    // Cylinder: x = radius, y = height (z unused).
    // Box:      half-extents.
    glm::vec3 Size{0.5f, 1.0f, 0.5f};
    // Local-space center offset from Transform.Position (mirrors ColliderComponent).
    glm::vec3 Offset{0.0f};
};
```

- [ ] **Step 2: Register in X-macro**

Modify `src/common/include/ECS.h` — append a new line at the end of `ECS_FOR_EACH_REGISTERED_COMPONENT` (current last line is `X(NavMeshConfigComponent)`):

```cpp
#define ECS_FOR_EACH_REGISTERED_COMPONENT(X) \
    X(TransformComponent) \
    X(MeshComponent) \
    X(MaterialComponent) \
    X(TextComponent) \
    X(LightningComponent) \
    X(ParentComponent) \
    X(ChildComponent) \
    X(SunMarker) \
    X(InputStateComponent) \
    X(WorldCameraComponent) \
    X(UICameraComponent) \
    X(DayNightConfigComponent) \
    X(AtmosphereStateComponent) \
    X(FogComponent) \
    X(SkyComponent) \
    X(AppControlComponent) \
    X(ViewportComponent) \
    X(PlayerComponent) \
    X(CameraZoomComponent) \
    X(GameStateComponent) \
    X(ActionQueueComponent) \
    X(UIRectComponent) \
    X(StateScopeComponent) \
    X(MenuButtonComponent) \
    X(MenuStateComponent) \
    X(ColliderComponent) \
    X(MoveIntentComponent) \
    X(NavMeshSourceComponent) \
    X(NavMeshConfigComponent) \
    X(NavObstacleComponent)
```

- [ ] **Step 3: Bump GAME_API_VERSION**

Modify `src/game/include/game.h`:

```cpp
#define GAME_API_VERSION 16u  // was 15u
```

- [ ] **Step 4: Build to verify**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target ecs Engine editor game --config Debug
```

Expected: clean build. Explicit template instantiations for `NavObstacleComponent` emitted automatically via X-macro.

- [ ] **Step 5: Commit**

```bash
git add src/common/include/ECS.h src/game/include/game.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs): NavObstacleComponent + NavObstacleShape enum + GAME_API 16

NavObstacleShape (Cylinder/Box) distinct from ColliderShape — Recast
has no Sphere obstacle, and obstacle/collision footprints may diverge.
NavObstacleComponent carries Shape + Size + Offset (Cylinder: x=radius
y=height; Box: half-extents). Registered in the X-macro for explicit
template instantiation. GAME_API 15 -> 16 because ECS.h layout
changed; editor + game must be rebuilt + editor restarted."
```

---

## Task 2: JSON round-trip + test_worldserial T23

**Files:**
- Modify: `src/common/include/ComponentSerialization.h` (to_json/from_json)
- Modify: `src/engine/src/utilities/WorldManager.cpp` (save/load loops)
- Modify: `tests/test_worldserial.cpp` (T23)

- [ ] **Step 1: Write failing test first**

In `tests/test_worldserial.cpp`, inside the existing `Test_Navigation()` function (added in nav-core Task 2), append a new block at the end (before the closing `}`):

```cpp
    // T23: NavObstacleComponent per-entity round-trip
    {
        NavObstacleComponent obs{};
        obs.Shape  = NavObstacleShape::Box;
        obs.Size   = glm::vec3(2.5f, 1.0f, 0.8f);
        obs.Offset = glm::vec3(0.1f, 0.0f, -0.2f);
        json j = obs;
        NavObstacleComponent back = j.get<NavObstacleComponent>();
        EXPECT(back.Shape == NavObstacleShape::Box);
        EXPECT(near(back.Size.x, 2.5f));
        EXPECT(near(back.Size.y, 1.0f));
        EXPECT(near(back.Size.z, 0.8f));
        EXPECT(near(back.Offset.x,  0.1f));
        EXPECT(near(back.Offset.y,  0.0f));
        EXPECT(near(back.Offset.z, -0.2f));
    }
```

- [ ] **Step 2: Run test — expect failure**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_worldserial --config Debug
```

Expected: FAIL with `nlohmann::json` cannot serialize `NavObstacleComponent`. Good.

- [ ] **Step 3: Add `to_json` / `from_json` for `NavObstacleComponent`**

In `src/common/include/ComponentSerialization.h`, insert AFTER the existing `NavMeshSourceComponent` block (which was added in Spec 1 Task 2, around line 194):

```cpp
inline void to_json(nlohmann::json& j, const NavObstacleComponent& t) {
    j = nlohmann::json{
        // Shape as uint8_t for JSON stability (matches ColliderShape / NavMeshGeometrySource pattern).
        {"Shape",  static_cast<uint8_t>(t.Shape)},
        {"Size",   t.Size},
        {"Offset", t.Offset}
    };
}
inline void from_json(const nlohmann::json& j, NavObstacleComponent& t) {
    if (j.contains("Shape"))  t.Shape = static_cast<NavObstacleShape>(j.at("Shape").get<uint8_t>());
    if (j.contains("Size"))   j.at("Size").get_to(t.Size);
    if (j.contains("Offset")) j.at("Offset").get_to(t.Offset);
}
```

- [ ] **Step 4: WorldManager save side**

Modify `src/engine/src/utilities/WorldManager.cpp`. In the per-entity save loop (the existing block where each component gets serialized), after the `NavMeshSourceComponent` block (added in Spec 1):

```cpp
        if (world->HasComponent<NavObstacleComponent>(entity)) {
            jEntity["NavObstacleComponent"] = *(world->GetComponent<NavObstacleComponent>(entity));
        }
```

- [ ] **Step 5: WorldManager load side**

In the per-entity load loop (where each component is restored), after the `NavMeshSourceComponent` load:

```cpp
            if (jEntity.contains("NavObstacleComponent"))
                world->AddComponent(createdEntity, jEntity["NavObstacleComponent"].get<NavObstacleComponent>());
```

- [ ] **Step 6: Run test to verify pass**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_worldserial --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```

Expected: success line. T20-T23 all pass.

- [ ] **Step 7: Regression check on other tests**

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
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): JSON round-trip for NavObstacleComponent

NavObstacleComponent serialized per-entity in world.json. Shape
stored as uint8_t (matches ColliderShape / NavMeshGeometrySource
pattern). T23 in test_worldserial pins Box + Size + Offset
round-trip with non-default values."
```

---

## Task 3: ECSCommands dispatch (Apply/Remove/Duplicate)

**Files:**
- Modify: `src/common/include/ECSCommands.h`

- [ ] **Step 1: Add `NavObstacleComponent` to `ApplyComponentCommand`**

In `src/common/include/ECSCommands.h`, find the `ApplyComponentCommand` function (around line 232). Append a new `else if` branch AFTER the `NavMeshConfigComponent` branch (added Spec 1 Task 3):

```cpp
        } else if (componentData.Type == std::type_index(typeid(NavObstacleComponent))) {
            if (auto* obs = componentData.Get<NavObstacleComponent>()) {
                world.AddComponent(entity, *obs);
            }
        }
```

- [ ] **Step 2: Add to `RemoveComponentByType`**

In the same file, find `RemoveComponentByType` (around line 293). Append after the `NavMeshConfigComponent` branch:

```cpp
        } else if (typeIndex == std::type_index(typeid(NavObstacleComponent))) {
            world.RemoveComponent<NavObstacleComponent>(entity);
        }
```

- [ ] **Step 3: Add to `DuplicateEntityComponents`**

In the same file, find `DuplicateEntityComponents` (around line 217). Append after the `NavMeshSourceComponent` line (added Spec 1 Task 3):

```cpp
        if (auto* c = world.GetComponent<NavObstacleComponent>(src))     world.AddComponent(dst, *c);
```

- [ ] **Step 4: Build to verify**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target ecs Engine editor game --config Debug
```

Expected: clean build. Header-only file, every TU re-includes.

- [ ] **Step 5: Commit**

```bash
git add src/common/include/ECSCommands.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs-cmds): NavObstacleComponent Apply/Remove/Duplicate dispatch

Standard plumbing so the inspector can add/edit/remove per-entity
and duplicated entities carry the obstacle. Mirrors the NavMeshSource
pattern shipped in Spec 1."
```

---

## Task 4: NavMesh + NavMeshSystem extensions — Tick + obstacle API + EntityId map

**Files:**
- Modify: `src/engine/src/navigation/NavMesh.h` (add Tick + AddCylinder/AddBox/RemoveObstacle declarations)
- Modify: `src/engine/src/navigation/NavMesh.cpp` (implement them)
- Modify: `src/engine/src/navigation/NavMeshSystem.h` (Tick + obstacle API + EntityId→ObstacleHandle map)
- Modify: `src/engine/src/navigation/NavMeshSystem.cpp` (implementations + map invalidation on Rebuild)

- [ ] **Step 1: Extend `NavMesh.h`**

Add the new public methods to `class NavMesh` in `src/engine/src/navigation/NavMesh.h`. Insert after the existing `GetStats()` declaration (around line 80):

```cpp
    // Drive dtTileCache::update — applies queued add/removeObstacle calls and
    // re-bakes affected tiles. Single-call-per-tick policy from spec; remaining
    // work continues next tick.
    void Tick(float dt);

    // Queue a cylinder obstacle (Y-axis aligned). Returns 0 on failure (log via SM_WARN).
    // Caller is responsible for tracking the returned ref to later remove the obstacle.
    uint32_t AddCylinderObstacle(const glm::vec3& pos, float radius, float height);

    // Queue an AABB obstacle. Returns 0 on failure.
    uint32_t AddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax);

    // Remove a previously-added obstacle by its ref. No-op if ref is 0.
    void RemoveObstacle(uint32_t ref);
```

- [ ] **Step 2: Implement on `NavMesh.cpp`**

In `src/engine/src/navigation/NavMesh.cpp`, append AFTER the existing `GetStats()` implementation:

```cpp
// ---------- Tick / Obstacle API ----------
void NavMesh::Tick(float dt)
{
    if (!m_TileCache || !m_NavMesh) return;
    bool upToDate = true;
    m_TileCache->update(dt, m_NavMesh, &upToDate);
    // Single-call policy per spec; if !upToDate, remaining work continues next tick.
}

uint32_t NavMesh::AddCylinderObstacle(const glm::vec3& pos, float radius, float height)
{
    if (!m_TileCache) return 0;
    const float p[3] = { pos.x, pos.y, pos.z };
    dtObstacleRef ref = 0;
    if (dtStatusFailed(m_TileCache->addObstacle(p, radius, height, &ref))) {
        return 0;
    }
    return static_cast<uint32_t>(ref);
}

uint32_t NavMesh::AddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax)
{
    if (!m_TileCache) return 0;
    const float mn[3] = { bmin.x, bmin.y, bmin.z };
    const float mx[3] = { bmax.x, bmax.y, bmax.z };
    dtObstacleRef ref = 0;
    if (dtStatusFailed(m_TileCache->addBoxObstacle(mn, mx, &ref))) {
        return 0;
    }
    return static_cast<uint32_t>(ref);
}

void NavMesh::RemoveObstacle(uint32_t ref)
{
    if (!m_TileCache || ref == 0) return;
    m_TileCache->removeObstacle(static_cast<dtObstacleRef>(ref));
}
```

- [ ] **Step 3: Extend `NavMeshSystem.h`**

Modify `src/engine/src/navigation/NavMeshSystem.h`. Replace the existing class with the extended version (preserve the existing Instance/Rebuild/Current, add the new methods + private member):

```cpp
#pragma once

#include <memory>
#include <unordered_map>

#include <glm/vec3.hpp>

#include "Engine.h"
#include "ECS.h"   // EntityId

class  ECS;
class  MeshSystem;
class  NavMesh;
struct NavMeshConfigComponent;

// Engine-side service holding the current navmesh. Build runs on GameThread; the
// resulting shared_ptr is atomic-published so any reader (RenderThread debug viz,
// game-side queries on the same GameThread) can grab a stable snapshot.
class ENGINE_API NavMeshSystem {
public:
    using ObstacleHandle = uint32_t;  // wraps dtObstacleRef

    static NavMeshSystem& Instance();

    // Build navmesh from current ECS state. GameThread only.
    // Clears the EntityId->ObstacleHandle map (old refs invalid against new dtTileCache).
    void Rebuild(const ECS& world, const NavMeshConfigComponent& cfg, const MeshSystem* meshSystem);

    // Get current navmesh. Any thread. May be null before first build.
    std::shared_ptr<const NavMesh> Current() const;

    // ---- Spec 2 additions ----

    // Drive dtTileCache::update on the current NavMesh. GameThread only. No-op if Current() is null.
    void Tick(float dt);

    // Forwarders to NavMesh add/remove. Return 0 on failure (no current navmesh, or dtTileCache failure).
    ObstacleHandle AddCylinderObstacle(const glm::vec3& pos, float radius, float height);
    ObstacleHandle AddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax);
    void           RemoveObstacle(ObstacleHandle h);

    // EntityId -> ObstacleHandle mapping. Side table so dtObstacleRef stays engine-side
    // (snapshot-thread mismatches if we stored it in a component).
    void           TrackObstacleForEntity(EntityId e, ObstacleHandle h);
    ObstacleHandle FindObstacleForEntity(EntityId e) const;  // returns 0 if not tracked
    void           UntrackEntity(EntityId e);

    int            ObstacleCount() const;  // for Navigation panel + tests

private:
    NavMeshSystem() = default;
    std::shared_ptr<const NavMesh>               m_Current;
    std::unordered_map<EntityId, ObstacleHandle> m_EntityToObstacle;
};
```

- [ ] **Step 4: Implement `NavMeshSystem.cpp` extensions**

Modify `src/engine/src/navigation/NavMeshSystem.cpp`:

(a) In `Rebuild`, clear the map BEFORE publishing the new shared_ptr. Find the existing `std::atomic_store(&m_Current, shared);` call. Insert one line above it (and one above the existing empty-soup atomic_store too — both code paths clear):

```cpp
void NavMeshSystem::Rebuild(const ECS& world,
                            const NavMeshConfigComponent& cfg,
                            const MeshSystem* meshSystem)
{
    const NavMeshTriangleSoup soup = NavMeshBuilder::CollectTriangles(world, meshSystem);
    if (soup.Empty || soup.Tris.empty()) {
        SM_WARN("NavMeshSystem::Rebuild: no NavMeshSource entities; publishing empty navmesh");
        m_EntityToObstacle.clear();   // <-- ADD
        std::atomic_store(&m_Current, std::shared_ptr<const NavMesh>{});
        return;
    }
    auto fresh = NavMesh::Build(soup, cfg);
    if (!fresh) {
        SM_WARN("NavMeshSystem::Rebuild: NavMesh::Build returned null; keeping previous navmesh");
        return;
    }
    m_EntityToObstacle.clear();       // <-- ADD
    std::shared_ptr<const NavMesh> shared(std::move(fresh));
    std::atomic_store(&m_Current, shared);
}
```

(b) Append the new methods at the end of `NavMeshSystem.cpp`:

```cpp
void NavMeshSystem::Tick(float dt) {
    auto cur = std::atomic_load(&m_Current);
    if (!cur) return;
    // const_cast: NavMesh::Tick is a mutating operation on the dtTileCache.
    // shared_ptr<const NavMesh> is the snapshot type for cross-thread reads;
    // the GameThread owner needs the mutable view to drive dtTileCache::update.
    const_cast<NavMesh*>(cur.get())->Tick(dt);
}

NavMeshSystem::ObstacleHandle NavMeshSystem::AddCylinderObstacle(
    const glm::vec3& pos, float radius, float height)
{
    auto cur = std::atomic_load(&m_Current);
    if (!cur) return 0;
    return const_cast<NavMesh*>(cur.get())->AddCylinderObstacle(pos, radius, height);
}

NavMeshSystem::ObstacleHandle NavMeshSystem::AddBoxObstacle(
    const glm::vec3& bmin, const glm::vec3& bmax)
{
    auto cur = std::atomic_load(&m_Current);
    if (!cur) return 0;
    return const_cast<NavMesh*>(cur.get())->AddBoxObstacle(bmin, bmax);
}

void NavMeshSystem::RemoveObstacle(ObstacleHandle h) {
    auto cur = std::atomic_load(&m_Current);
    if (!cur || h == 0) return;
    const_cast<NavMesh*>(cur.get())->RemoveObstacle(h);
}

void NavMeshSystem::TrackObstacleForEntity(EntityId e, ObstacleHandle h) {
    if (h == 0) return;
    m_EntityToObstacle[e] = h;
}

NavMeshSystem::ObstacleHandle NavMeshSystem::FindObstacleForEntity(EntityId e) const {
    auto it = m_EntityToObstacle.find(e);
    return (it != m_EntityToObstacle.end()) ? it->second : 0;
}

void NavMeshSystem::UntrackEntity(EntityId e) {
    m_EntityToObstacle.erase(e);
}

int NavMeshSystem::ObstacleCount() const {
    return static_cast<int>(m_EntityToObstacle.size());
}
```

- [ ] **Step 5: Build**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine --config Debug
```

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/navigation/NavMesh.h src/engine/src/navigation/NavMesh.cpp src/engine/src/navigation/NavMeshSystem.h src/engine/src/navigation/NavMeshSystem.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): NavMesh + NavMeshSystem obstacle API + Tick

NavMesh::Tick(dt) drives dtTileCache::update — single-call-per-tick
policy from spec; remaining queued work continues next tick.
NavMesh::AddCylinderObstacle / AddBoxObstacle / RemoveObstacle wrap
dtObstacleRef as uint32_t so the public surface stays free of
DetourTileCache.

NavMeshSystem grows the same forwarders + an EntityId->ObstacleHandle
side table (engine-side: refs are GameThread-local, would be unsafe
inside an ECS component snapshot). Rebuild clears the map before
atomic-store since the new dtTileCache has fresh address space —
old refs are invalid. Sync system (Task 5) re-adds obstacles on
the next tick."
```

---

## Task 5: NavObstacleSync.h header + test_navmesh T09-T13

**Files:**
- Create: `src/game/src/NavObstacleSync.h` (header-only ISystem)
- Modify: `tests/test_navmesh.cpp` (add DrainTileCache helper + T09-T13)

- [ ] **Step 1: Create `NavObstacleSync.h`**

```cpp
#pragma once

#include <unordered_map>
#include <unordered_set>

#include <glm/glm.hpp>

#include "ECS.h"
#include "Systems.h"
#include "lib.h"   // SM_WARN

#include "navigation/NavMeshSystem.h"

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
                    } else {  // Box
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
        glm::vec3        Position{0.0f};
        NavObstacleShape Shape{NavObstacleShape::Cylinder};
        glm::vec3        Size{0.0f};
    };
    std::unordered_map<EntityId, CachedState> m_EntityCache;
    std::unordered_set<EntityId>              m_VisitedThisTick;
    static constexpr float kPositionEpsilon = 0.05f;  // 5 cm — below default cell size 0.3m
};
```

- [ ] **Step 2: Write failing tests in `test_navmesh.cpp`**

Append the helper + 5 new tests to `tests/test_navmesh.cpp`. Add the include + helper near the top (after existing includes):

```cpp
#include "NavObstacleSync.h"
```

Add the include path for `src/game/src` to `tests/CMakeLists.txt` — see Step 4.

Below the existing test functions (after T07), add:

```cpp
// Helper: drive dtTileCache::update until upToDate so tests see post-carve state.
// Bounded loop to avoid hangs on bugs.
static void DrainTileCache(NavMeshSystem& nav, int maxTicks = 16) {
    for (int i = 0; i < maxTicks; ++i) nav.Tick(0.016f);
}

// Shared helper for obstacle tests: floor + initial Rebuild that gives a usable navmesh.
static void SpawnFloorAndBuild(ECS& w) {
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
}

static EntityId SpawnCylinderObstacle(ECS& w, const glm::vec3& pos, float radius, float height) {
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ pos, glm::vec3(0.0f), glm::vec3(1.0f) });
    NavObstacleComponent obs{};
    obs.Shape = NavObstacleShape::Cylinder;
    obs.Size  = glm::vec3(radius, height, 0.0f);
    w.AddComponent(e, obs);
    return e;
}

static EntityId SpawnBoxObstacle(ECS& w, const glm::vec3& pos, const glm::vec3& halfExtents) {
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ pos, glm::vec3(0.0f), glm::vec3(1.0f) });
    NavObstacleComponent obs{};
    obs.Shape = NavObstacleShape::Box;
    obs.Size  = halfExtents;
    w.AddComponent(e, obs);
    return e;
}

static void RunSync(ECS& w, NavObstacleSyncSystem& sync) {
    SystemContext ctx{ w, 0.016 };
    sync.Update(ctx);
}

static void T09_cylinder_obstacle_blocks_path() {
    ECS w;
    SpawnFloorAndBuild(w);
    auto nm0 = NavMeshSystem::Instance().Current();
    EXPECT(nm0 != nullptr);
    auto pathBefore = nm0->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathBefore.size() == 2);  // straight line, no obstacle

    SpawnCylinderObstacle(w, glm::vec3(0, 0, 0), 1.5f, 2.0f);
    NavObstacleSyncSystem sync;
    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());

    auto pathAfter = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathAfter.size() > 2);  // forced to route around the cylinder
}

static void T10_box_obstacle_blocks_path() {
    ECS w;
    SpawnFloorAndBuild(w);
    SpawnBoxObstacle(w, glm::vec3(0, 1.0f, 0), glm::vec3(0.5f, 1.0f, 2.0f));  // wall-ish box
    NavObstacleSyncSystem sync;
    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());

    auto path = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(path.size() > 2);  // routes around the box
}

static void T11_remove_obstacle_restores_path() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnCylinderObstacle(w, glm::vec3(0, 0, 0), 1.5f, 2.0f);
    NavObstacleSyncSystem sync;

    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());
    auto pathBlocked = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathBlocked.size() > 2);

    w.RemoveComponent<NavObstacleComponent>(e);
    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());
    auto pathRestored = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathRestored.size() == 2);  // straight again
}

static void T12_obstacle_position_change_triggers_rebind() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnCylinderObstacle(w, glm::vec3(0, 0, 4.5f), 1.5f, 2.0f);  // off-path
    NavObstacleSyncSystem sync;

    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());
    auto pathOffPath = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathOffPath.size() == 2);  // obstacle is far from the line

    // Move obstacle onto the path.
    w.Modify<TransformComponent>(e, [](TransformComponent& t){ t.Position = glm::vec3(0, 0, 0); });
    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());
    auto pathOnPath = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathOnPath.size() > 2);
}

static void T13_rebuild_clears_obstacle_map() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnCylinderObstacle(w, glm::vec3(0, 0, 0), 1.5f, 2.0f);
    NavObstacleSyncSystem sync;

    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());
    EXPECT(NavMeshSystem::Instance().ObstacleCount() == 1);

    // Trigger Rebuild — map clears + new navmesh, obstacle handle no longer tracked.
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    EXPECT(NavMeshSystem::Instance().ObstacleCount() == 0);

    // Next sync re-adds the obstacle (entity still has the component).
    RunSync(w, sync);
    EXPECT(NavMeshSystem::Instance().ObstacleCount() == 1);
}
```

Update `main()`:

```cpp
    T07_current_shared_across_threads();
    T08_flat_floor_only_box_builds_walkable_navmesh();
    T09_cylinder_obstacle_blocks_path();
    T10_box_obstacle_blocks_path();
    T11_remove_obstacle_restores_path();
    T12_obstacle_position_change_triggers_rebind();
    T13_rebuild_clears_obstacle_map();
```

- [ ] **Step 3: Run tests — expect compile failure**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh --config Debug
```

Expected: FAIL with `cannot find NavObstacleSync.h` (include path missing) — fixed next step.

- [ ] **Step 4: Add `src/game/src` to test_navmesh include path**

Modify `tests/CMakeLists.txt`. Find the existing `test_navmesh` target block. Update `target_include_directories(test_navmesh PRIVATE ...)` to include the game/src dir:

```cmake
target_include_directories(test_navmesh PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src
    ${CMAKE_SOURCE_DIR}/src/game/src
)
```

- [ ] **Step 5: Run tests to verify pass**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: `All navmesh tests passed.` All 13 tests green (T01-T08 + T09-T13).

- [ ] **Step 6: Regression check on other tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_collision test_worldserial test_menu --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_collision.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/game/src/NavObstacleSync.h tests/test_navmesh.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): NavObstacleSyncSystem + obstacle-behavior tests

Header-only ISystem in Physics phase that diffs NavObstacleComponent
state against the EntityId->ObstacleHandle map owned by
NavMeshSystem. Handles: appear (Add+Track), disappear (Remove+
Untrack+erase cached state), position moved > 5cm (rebind), Shape/
Size changed (rebind). SM_WARN when AddObstacle fails (cap reached).
GC pass at end removes entries for vanished entities.

T09 cylinder blocks path. T10 box blocks path. T11 remove restores
straight path. T12 moving obstacle triggers rebind. T13 Rebuild
clears the obstacle map and next sync re-adds.

DrainTileCache helper loops Tick up to 16 ticks so tests see post-
carve state (dtTileCache::update is async — single call may not
finish all queued obstacle changes)."
```

---

## Task 6: GameThread per-tick `NavMeshSystem::Tick(dt)` call

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp`

- [ ] **Step 1: Add Tick call after game update**

In `src/engine/src/threading/GameThread.cpp`, find the per-tick game-update sequence (around line 218 where `ECSCommandProcessor::ProcessCommands` is called). The Game_GameUpdate function runs all ISystems via the GameLibrary. The Tick call goes AFTER that, before the snapshot publish.

Search for `Game_GameUpdate` or similar — locate the line where game logic runs each tick. Immediately after it, add:

```cpp
        // Drive dtTileCache::update so this tick's NavObstacleSyncSystem add/remove
        // calls actually apply (re-bake affected tiles, update obstacle state). Single
        // call per tick per Spec 2 (drain-loop policy deferred to perf measurement).
        NavMeshSystem::Instance().Tick(static_cast<float>(dt));
```

(`dt` should be available in the tick loop scope — verify with the surrounding code. If it's named differently, e.g., `m_FixedDt` or `tickDt`, use that.)

Ensure the include is present at the top:

```cpp
#include "navigation/NavMeshSystem.h"
```

(Spec 1 already added this include for the hooks lambda — confirm it's still there.)

- [ ] **Step 2: Build**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine --config Debug
```

Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add src/engine/src/threading/GameThread.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): GameThread drives NavMeshSystem::Tick once per tick

Single call after Game_GameUpdate (all ISystems including
NavObstacleSyncSystem have queued add/remove calls) and before
snapshot publish. dtTileCache::update applies queued obstacle
changes + re-bakes affected tiles. Single-call-per-tick policy
per Spec 2; switch to drain-loop later only if measured insufficient
under realistic load."
```

---

## Task 7: DebugAppendCylinder helper + ShowObstacles toggle + viz

**Files:**
- Modify: `src/engine/src/rendering/DebugDraw.h` (add inline DebugAppendCylinder)
- Modify: `src/engine/src/rendering/RenderStats.h` (ShowObstacles field)
- Modify: `src/editor/src/EditorPreferences.h` (round-trip "obstacles" key)
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` (checkbox)
- Modify: `src/engine/src/rendering/passes/DebugRenderPass.cpp` (ShowObstacles block)

- [ ] **Step 1: Add `DebugAppendCylinder` to `DebugDraw.h`**

Insert AFTER `DebugAppendCapsule` (around line 84), BEFORE `DebugAppendArrow`:

```cpp
// Wireframe cylinder (Y-axis aligned). `bottomCenter` = base center, height extends +Y.
// segments per ring. Two horizontal rings (XZ plane) + 4 vertical seams at +X/-X/+Z/-Z.
// (segments * 2 + 4) * 2 verts total. Matches the cylinder body of DebugAppendCapsule.
inline void DebugAppendCylinder(std::vector<DebugVertex>& out, const glm::vec3& bottomCenter,
                                float radius, float height,
                                const glm::vec4& color, int segments = 12) {
    if (radius < 1e-4f) radius = 1e-4f;
    if (height < 0.0f)  height = 0.0f;
    if (segments < 4)   segments = 4;
    const float kTwoPi = 6.28318530718f;
    const glm::vec3 topCenter = bottomCenter + glm::vec3(0.0f, height, 0.0f);

    // 2 horizontal circles (XZ) at base + top.
    for (int i = 0; i < segments; ++i) {
        const float t0 = kTwoPi * (float(i)     / float(segments));
        const float t1 = kTwoPi * (float(i + 1) / float(segments));
        const glm::vec3 a(std::cos(t0) * radius, 0.0f, std::sin(t0) * radius);
        const glm::vec3 b(std::cos(t1) * radius, 0.0f, std::sin(t1) * radius);
        DebugAppendLine(out, bottomCenter + a, bottomCenter + b, color);
        DebugAppendLine(out, topCenter    + a, topCenter    + b, color);
    }

    // 4 vertical seams.
    const glm::vec3 seams[4] = {
        { radius, 0.0f, 0.0f }, { -radius, 0.0f, 0.0f },
        { 0.0f, 0.0f,  radius }, { 0.0f, 0.0f, -radius },
    };
    for (const glm::vec3& s : seams) {
        DebugAppendLine(out, bottomCenter + s, topCenter + s, color);
    }
}
```

- [ ] **Step 2: Add `ShowObstacles` field to `DebugDrawSettings`**

In `src/engine/src/rendering/RenderStats.h`, extend the struct (around line 17-25):

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
};
```

- [ ] **Step 3: Round-trip in EditorPreferences**

In `src/editor/src/EditorPreferences.h`, extend `PrefsToJson` debugDraw block (around line 30-37):

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
        }},
```

Extend `PrefsFromJson` (around line 64-71) — add this line after the `navmesh` line:

```cpp
        if (d.contains("obstacles")     && d["obstacles"].is_boolean())     debug.ShowObstacles     = d["obstacles"].get<bool>();
```

- [ ] **Step 4: Add Render Stats checkbox**

In `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`, append after the existing `NavMesh` checkbox:

```cpp
    changed |= ImGui::Checkbox("Obstacles",      &dd.ShowObstacles);
```

- [ ] **Step 5: Add ShowObstacles block to `DebugRenderPass.cpp`**

In `src/engine/src/rendering/passes/DebugRenderPass.cpp`:

(a) Update the early-out to include the new toggle. Find the existing line:
```cpp
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid && !s.ShowColliders && !s.ShowNavMesh)
        return;
```
Replace with:
```cpp
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid && !s.ShowColliders && !s.ShowNavMesh && !s.ShowObstacles)
        return;
```

(b) After the existing `if (s.ShowNavMesh) { ... }` block (added in Spec 1 Task 7), append:

```cpp
    if (s.ShowObstacles) {
        // Magenta — distinct from cyan (trigger), yellow (static), orange (dynamic),
        // lime green (navmesh), white/grey (grid). Reads as "carved" without conflict.
        const glm::vec4 col(1.0f, 0.3f, 0.9f, 1.0f);
        world->Each<NavObstacleComponent, TransformComponent>(
            [&](EntityId, const NavObstacleComponent& obs, const TransformComponent& tr) {
                const glm::vec3 worldPos = tr.Position + obs.Offset;
                if (obs.Shape == NavObstacleShape::Cylinder) {
                    // Cylinder anchored at base; obs.Size.x = radius, obs.Size.y = height.
                    DebugAppendCylinder(m_Verts, worldPos, obs.Size.x, obs.Size.y, col);
                } else {  // Box: DebugAppendBox takes (mn, mx) — convert from (center, halfExtents).
                    DebugAppendBox(m_Verts, worldPos - obs.Size, worldPos + obs.Size, col);
                }
            });
    }
```

- [ ] **Step 6: Build + run regression tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine editor test_editorprefs test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorprefs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: editor + Engine build clean. Both tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/rendering/DebugDraw.h src/engine/src/rendering/RenderStats.h src/editor/src/EditorPreferences.h src/editor/src/rendering/imgui/RenderStatsPanel.cpp src/engine/src/rendering/passes/DebugRenderPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): ShowObstacles debug viz + DebugAppendCylinder helper

DebugAppendCylinder inline in DebugDraw.h (header-only convention).
Y-axis aligned, base+top XZ rings + 4 vertical seams — matches the
cylinder body of DebugAppendCapsule, just without the dome caps.

ShowObstacles joins ShowNavMesh in DebugDrawSettings, persisted via
editor_preferences.json under the existing debugDraw block
(additive — old prefs files load with ShowObstacles=false). Render
Stats panel grows an Obstacles checkbox. DebugRenderPass iterates
NavObstacleComponent entities, draws cylinder (DebugAppendCylinder)
or box (DebugAppendBox with mn/mx convention) in magenta. Distinct
hue from every other debug color in the palette."
```

---

## Task 8: NavObstacle inspector UI

**Files:**
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.h` (member state)
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp` (Add/Remove menu + edit block)

- [ ] **Step 1: Add member state to `EcsInspectorPanel.h`**

In the `private:` section, immediately after the existing `NavMeshSource*` block (added in nav-core inspector commit `8cc6e67`, member lines `editNavSource` / `lastEditedNavSourceEntity` / `navSourceModified`), append:

```cpp
    NavObstacleComponent editNavObstacle{};
    EntityId             lastEditedNavObstacleEntity = INVALID_ENTITY;
    bool                 navObstacleModified = false;
```

- [ ] **Step 2: Add Component menu entry**

In `EcsInspectorPanel.cpp`, find the existing "Add NavMesh Source Component" block (search for `Add NavMesh Source Component`). Immediately after it (before the `ImGui::Separator();`), insert:

```cpp
                if (!ctx.WorldSnapshot->HasComponent<NavObstacleComponent>(entity)) {
                    if (ImGui::MenuItem("Add NavMesh Obstacle Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, NavObstacleComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }
```

- [ ] **Step 3: Remove Component menu entry**

Find the existing "Remove NavMesh Source Component" block. Immediately after it (before `ImGui::EndPopup();`), insert:

```cpp
                if (ctx.WorldSnapshot->HasComponent<NavObstacleComponent>(entity)) {
                    if (ImGui::MenuItem("Remove NavMesh Obstacle Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<NavObstacleComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }
```

- [ ] **Step 4: Per-component edit UI**

Find the existing "Edit NavMesh Source Component" block. Immediately after its closing braces (before the `} else if (selectedEntity != INVALID_ENTITY) {`), insert:

```cpp
            // Edit NavMesh Obstacle Component
            if (ctx.WorldSnapshot->HasComponent<NavObstacleComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("NavMesh Obstacle Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* obs = ctx.WorldSnapshot->GetComponent<NavObstacleComponent>(selectedEntity);
                    if (obs) {
                        if (lastEditedNavObstacleEntity != selectedEntity) {
                            editNavObstacle = *obs;
                            lastEditedNavObstacleEntity = selectedEntity;
                            navObstacleModified = false;
                        }
                        if (!navObstacleModified) {
                            editNavObstacle = *obs;
                        }
                        static const char* kShapeNames[] = { "Cylinder", "Box" };
                        static const NavObstacleShape kShapes[] = {
                            NavObstacleShape::Cylinder, NavObstacleShape::Box,
                        };
                        int idx = 0;
                        for (int i = 0; i < 2; ++i) if (editNavObstacle.Shape == kShapes[i]) idx = i;
                        if (ImGui::Combo("Shape", &idx, kShapeNames, 2)) {
                            editNavObstacle.Shape = kShapes[idx];
                            navObstacleModified = true;
                        }
                        const char* sizeLabel = (editNavObstacle.Shape == NavObstacleShape::Cylinder)
                            ? "Size (X=radius, Y=height)"
                            : "Size (half-extents)";
                        if (ImGui::InputFloat3(sizeLabel, &editNavObstacle.Size.x)) navObstacleModified = true;
                        if (ImGui::InputFloat3("Offset", &editNavObstacle.Offset.x)) navObstacleModified = true;
                        ImGui::Spacing();
                        if (navObstacleModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editNavObstacle);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            navObstacleModified = false;
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
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): NavMesh Obstacle component in EcsInspectorPanel

Add Component / Remove Component menu entries + per-component edit
UI (Shape combo Cylinder/Box + Size + Offset). Inspector mirrors the
NavMeshSource pattern (working-copy member + lastEdited tracker +
modified flag) and pushes ECSCommand::ModifyComponent on changes.

Size label switches based on Shape: 'X=radius, Y=height' for
Cylinder, 'half-extents' for Box."
```

---

## Task 9: Register NavObstacleSyncSystem + final review

**Files:**
- Modify: `src/game/src/game.cpp` (include + system registration)

- [ ] **Step 1: Include the sync system header**

In `src/game/src/game.cpp`, near the top of the file with other system includes:

```cpp
#include "NavObstacleSync.h"
```

- [ ] **Step 2: Register the system in `GameRegisterSystems`**

Find the existing `GameRegisterSystems` function in `game.cpp`. Locate the line that registers `KinematicMovementSystem` (Physics phase). Add the obstacle sync registration immediately after — same phase, registers in order so KinematicMovement runs first (mover positions resolved), then NavObstacleSync picks up post-resolution Transform positions:

```cpp
    s_Systems.AddSystem<KinematicMovementSystem>();
    s_Systems.AddSystem<NavObstacleSyncSystem>();   // Physics phase, after movement
```

(The exact registration call may use a different API — read the existing system registration calls and match their pattern. Above is the canonical form from existing code.)

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
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): register NavObstacleSyncSystem (Physics phase)

Registered after KinematicMovementSystem so mover entities are at
their post-resolution positions when the sync system reads
Transform.Position. Both live in Physics phase; registration order
is preserved within a phase by the scheduler."
```

- [ ] **Step 5: Dispatch final whole-feature reviewer**

Per `superpowers:subagent-driven-development`, dispatch the final reviewer subagent. Provide:
- Spec: `docs/superpowers/specs/2026-05-27-navigation-obstacles-design.md` (commit `0bf01f1`)
- Full branch diff: `git diff main..feat/navigation-obstacles`
- All 9 commits on the branch (one per task).
- Per-task reviews summary (all APPROVED).

Reviewer verdict (READY TO MERGE / READY WITH NOTES / NEEDS CHANGES) drives merge-readiness. User does the GUI smoke before merge.

---

## Self-review notes

**Spec coverage check:**

- ✅ NavObstacleShape enum + NavObstacleComponent + X-macro entry — Task 1
- ✅ GAME_API_VERSION 15→16 — Task 1
- ✅ JSON round-trip (per-entity, Shape as uint8_t) — Task 2
- ✅ WorldManager save/load — Task 2
- ✅ test_worldserial T23 — Task 2
- ✅ ECSCommands Apply/Remove/Duplicate — Task 3
- ✅ NavMesh Tick + AddCylinderObstacle/AddBoxObstacle/RemoveObstacle — Task 4
- ✅ NavMeshSystem Tick + obstacle API + EntityId→ObstacleHandle map — Task 4
- ✅ Map invalidation on Rebuild (clear before atomic-store, both paths) — Task 4
- ✅ NavObstacleSyncSystem (Physics phase, position-delta tracking, GC) — Task 5
- ✅ test_navmesh T09-T13 + DrainTileCache helper — Task 5
- ✅ GameThread NavMeshSystem::Tick(dt) per-tick call — Task 6
- ✅ DebugAppendCylinder helper (inline in DebugDraw.h, header-only convention) — Task 7
- ✅ ShowObstacles toggle + EditorPreferences round-trip + checkbox — Task 7
- ✅ DebugRenderPass ShowObstacles block (magenta, mn/mx box convention) — Task 7
- ✅ Inspector Add/Remove menu + edit UI — Task 8
- ✅ NavObstacleSyncSystem registration (after KinematicMovementSystem) — Task 9
- ✅ Final whole-feature review — Task 9 Step 5

No gaps.

**Type-consistency check:**

- `NavObstacleShape::Cylinder = 0`, `Box = 1` — consistent T1 (enum), T2 (JSON cast), T5 (test helpers + sync system), T7 (viz dispatch), T8 (inspector combo).
- `NavObstacleComponent` field set (Shape, Size, Offset) — consistent everywhere.
- `NavMeshSystem::ObstacleHandle` typedef'd to `uint32_t` — used by T4 (API), T5 (sync system), T5 (test ObstacleCount call). Tests use `int` for ObstacleCount per the API.
- `NavMesh::Tick(dt)` vs `NavMeshSystem::Tick(dt)` — both same signature, system forwards to NavMesh.
- `kPositionEpsilon = 0.05f` — only used in T5 sync system; no other references.
- `MaxObstacles` config field already plumbed Spec 1; no change needed in this plan.

**Placeholder scan:**

Two locations call out for plan-time verification — these are honest "verify-then-edit" steps, not unfilled blanks:
- Task 6 Step 1: confirm the variable name for `dt` in GameThread.cpp's tick scope.
- Task 9 Step 2: confirm the system-registration API in `GameRegisterSystems` (the canonical form shown is `s_Systems.AddSystem<T>()` but the actual call may differ — read existing system registrations and match).

The implementer must read those exact lines before pasting; both are 1-line edits with clear surrounding context.

**Commit count:** 9 commits (Tasks 1-9, Task 0 administrative). Matches the spec's 5-7 estimate ± headroom — slightly higher because the test coverage is meaningful (T09-T13 + T23) and gets its own commit per TDD discipline.
