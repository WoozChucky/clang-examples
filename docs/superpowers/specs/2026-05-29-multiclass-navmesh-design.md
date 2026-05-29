# Multi-Class Navmesh (per-agent-radius bakes)

**Date:** 2026-05-29
**Status:** Approved, pending implementation plan

## Problem / Goal

Navmesh-constrained movement (shipped on `feat/navmesh-constrained-movement`) constrains an
entity's **center point** to the walkable navmesh via Detour `moveAlongSurface`. Recast bakes
the navmesh **eroded inward by `NavMeshConfigComponent.AgentRadius`** (`rcErodeWalkableArea`),
so a point-on-mesh is automatically kept that far from walls/obstacles. But there is only **one**
navmesh with **one** bake radius (0.5 m default), shared by every entity. An entity whose body
footprint differs from that single bake radius pokes its collider into walls (center reaches the
eroded edge, body half-extent crosses it).

**Goal:** support **multiple navmeshes baked at different agent radii ("classes")** over the same
map geometry. Each entity navigates the mesh baked for *its* size class, so `moveAlongSurface`
keeps the body tangent to walls with **zero runtime radius math**. This is the standard Recast
pattern (one navmesh per agent-radius category) and is deterministic — well-suited to the target
ARPG's future server-authoritative multiplayer simulation.

Concretely: players (uniform size) and same-size monsters share one class; a larger boss uses a
second, more-eroded class.

## Decisions (from brainstorming)

1. **Class model:** an **explicit authored class list** (not auto-derived from entity radii).
2. **Entity → class:** a **shared `NavClassComponent { ClassId }`** carried by the player and by
   NavAgents (not a field duplicated across components; the `NavConstrainedComponent` marker stays
   fieldless).
3. **Storage:** class list is a **fixed-cap array inside `NavMeshConfigComponent`** (not a heap
   `std::vector`) so the component stays trivially-copyable for the ECS snapshot — matching the
   existing `NavAgentComponent::CachedPath[32]` style. `ClassId` is the **index** into that list
   (default 0); append-only list recommended. The top-level per-agent fields
   (`AgentRadius/AgentHeight/AgentMaxClimb`) are **removed** — `NavClassConfig` is the single source
   of truth — and `NavMesh::Build` takes the chosen `NavClassConfig` directly (no `ConfigForClass`
   shim). Invariant: `ClassCount >= 1` (a default config has one class).
6. **No backward compatibility required** (engine is early-development): old `world.json` nav configs
   need no migration code — a config with no `Classes` key simply loads as one default class.
   Breaking schema changes are acceptable and preferred over migration complexity.
4. **Obstacle fan-out** via **logical obstacle ids** that keep the `NavServices` obstacle ABI
   (`uint32_t` handles) unchanged.
5. **Queries:** **append** class-aware `…ForClass` variants to `NavServices`; old non-class fns
   delegate to class 0 (back-compat + existing tests).

## Non-Goals (this spec)

- **Multi-class disk bake** — runtime rebuild only this iteration. Extending
  `SaveToFile`/`LoadFromFile`/`TryLoadFromDisk` to N meshes (+ staleness) is the **explicit
  follow-up** once this works. (The existing single-mesh disk-bake path is left intact but is
  effectively superseded for multi-class scenes; see Migration & Disk-Bake Interaction.)
- **Stable class ids / reorder safety** — `ClassId` is a positional index; reordering or removing a
  class remaps entity ClassIds. Append-only editing is assumed and documented.
- **Per-class `NavVersion`** — a single global version (bumped on any rebuild) is used; conservative
  path-invalidation, simple, correct.
- No change to `NavMesh` itself (each `NavMesh` remains a single-class mesh).
- No change to the player's planar-movement / AABB-layering behavior from the base feature.

## Background (verified)

- `NavMeshSystem` (`src/engine/src/navigation/NavMeshSystem.h`) is a GameThread-only singleton
  holding ONE published `std::shared_ptr<const NavMesh> m_Current`, an `EntityId → ObstacleHandle`
  map, a global `m_NavVersion`, and a mesh-CPU-data cache. `Rebuild(world, cfg)` builds one mesh
  from the triangle soup + cfg; `Current()`, `AddCylinderObstacle/AddBoxObstacle/RemoveObstacle/
  Tick`, `Track/Find/UntrackObstacleForEntity` operate on that one mesh.
- `NavMesh::Build(soup, cfg)` consumes a `NavMeshConfigComponent` whose `AgentRadius/AgentHeight/
  AgentMaxClimb` drive Recast erosion/clearance/step. `NavMesh::{FindPath,ClosestPoint,
  ConstrainMove}` query the single `dtNavMeshQuery`.
- `NavMeshConfigComponent` (`ECS.h`): `CellSize, CellHeight, AgentRadius, AgentHeight,
  AgentMaxClimb, AgentMaxSlope, TileSize, MaxObstacles`. Singleton, persisted in world.json
  `Environment` (`WorldManager` + `ComponentSerialization.h` / `BuildEnvironmentJson`).
- `NavServices` (`src/common/include/NavServices.h`): append-only function-pointer table.
  Currently `HasMesh, FindPath, NavVersion, AddCylinderObstacle, AddBoxObstacle, RemoveObstacle,
  Track/Find/UntrackObstacleForEntity, MoveAlongSurface`.
- Components register via the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro + `ECSCommands` Apply/
  Remove/Copy + `ComponentSerialization` + `WorldManager` save/load + an inspector editor (registry
  in `EcsInspectorPanel`).
- `NavAgentSystem` (Simulation) and `KinematicMovementSystem` (Physics, the player constraint seam)
  both receive `SystemContext.Nav` every tick.

## Design

### 1. Class model & config (`ECS.h`, serialization)

```cpp
inline constexpr int kMaxNavClasses = 8;

struct NavClassConfig {
    float AgentRadius   = 0.5f;
    float AgentHeight   = 1.8f;
    float AgentMaxClimb = 0.4f;
};

struct NavMeshConfigComponent {
    // ---- shared/global bake params ----
    float CellSize      = 0.3f;
    float CellHeight    = 0.2f;
    float AgentMaxSlope = 45.0f;
    float TileSize      = 32.0f;
    int   MaxObstacles  = 128;

    // ---- per-radius classes (fixed-cap, trivially-copyable) ----
    NavClassConfig Classes[kMaxNavClasses]{};
    uint8_t        ClassCount = 0;   // 0 → treated as one default class (legacy / migration)
};
```

The per-agent fields (`AgentRadius/AgentHeight/AgentMaxClimb`) are **removed** from the top level;
they live only in `NavClassConfig`. `NavMesh::Build` changes signature to
`Build(const NavMeshTriangleSoup&, const NavMeshConfigComponent& cfg, const NavClassConfig& cls)` —
it reads `AgentRadius/AgentHeight/AgentMaxClimb` from `cls` and the shared params from `cfg`. The
default-constructed `NavMeshConfigComponent` has `ClassCount = 1` with `Classes[0]` defaulted
(`{0.5, 1.8, 0.4}`); `ClassCount >= 1` is an invariant. No `ConfigForClass` shim.

**Serialization (no migration code):** `to_json` writes the shared params + a `Classes` array
(length `ClassCount`). `from_json` reads them; a config with **no `Classes` key** (old file) loads as
**one default class** (`ClassCount = 1`, `Classes[0]` defaulted). Old flat `AgentRadius` keys are
ignored. Breaking is acceptable (early dev). Additionally, the existing build-output `world.json` is
hand-updated to the new `Classes` schema as part of implementation (it's runtime data, not committed).

### 2. Entity → class (`ECS.h` + plumbing + editor)

```cpp
struct NavClassComponent { uint8_t ClassId = 0; };
```

Registered exactly like other components: X-macro, `ECSCommands` Apply/Remove/Copy,
`ComponentSerialization` (`to_json`/`from_json` of the single `ClassId`), `WorldManager` save/load,
and a `NavClassEditor` inspector (dropdown over `0..ClassCount-1`). Carried by the player (next to
`NavConstrainedComponent`) and by NavAgent entities. Absent → class 0. `ClassId >= ClassCount`
→ class 0 + one-shot `SM_WARN`.

A pure helper `uint8_t ResolveNavClass(const ECS& world, EntityId e, uint8_t classCount)` (returns
the entity's clamped ClassId, default 0) is factored so it is unit-testable.

### 3. `NavMeshSystem` — N meshes

- `m_Current` → `std::array<std::shared_ptr<const NavMesh>, kMaxNavClasses> m_Classes;` published
  per slot through the existing `PublishNavMesh`-style routing (extended to take a class index;
  still bumps the single global `m_NavVersion`).
- `Rebuild(world, cfg)`: build the triangle soup **once**; for `i in [0, NavLiveClassCount(cfg))`
  build `NavMesh::Build(soup, cfg, cfg.Classes[i])` and publish slot `i`. Empty soup → clear all
  slots. Reapplying obstacles after a rebuild fans out (see §4).
- `Current(uint8_t classId) const` → slot accessor (clamped; out-of-range → slot 0 / nullptr).
- A new private `m_ClassCount` snapshot records how many slots are live (for obstacle fan-out + the
  bake loop).

### 4. Obstacles fan out to all classes

An obstacle is a world object; each class mesh erodes around it by its own radius automatically, so
the SAME obstacle params are added to EVERY live class mesh.

- `AddCylinderObstacle/AddBoxObstacle` → for each live class, add to that class's `NavMesh`; store
  the per-class refs under a freshly-minted **logical obstacle id**; return the logical id.
- Internal map `std::unordered_map<uint32_t /*logicalId*/, std::array<uint32_t, kMaxNavClasses>>`
  (per-class `NavMesh`-level obstacle refs; 0 = unused slot).
- `RemoveObstacle(logicalId)` + `Tick(dt)` fan out across all live slots.
- `EntityToObstacle` stores the logical id (unchanged externally). `NavServices` obstacle ABI is
  unchanged — handles are logical ids.

### 5. Query API — append class-aware variants (`NavServices.h` + `NavServicesImpl.cpp`)

Appended at the END of `struct NavServices` (append-only contract):
```cpp
bool      (*HasMeshForClass)(uint8_t classId);
void      (*FindPathForClass)(uint8_t classId, const glm::vec3& start, const glm::vec3& end,
                              float maxSearchRadius, std::vector<glm::vec3>* outPath);
glm::vec3 (*MoveAlongSurfaceForClass)(uint8_t classId, const glm::vec3& start,
                                      const glm::vec3& desiredEnd);
```
Forwarders mirror the existing ones but call `NavMeshSystem::Instance().Current(classId)`. The
existing `HasMesh/FindPath/MoveAlongSurface` are kept and delegate to **class 0** (back-compat +
existing T24/T31/T32 tests).

Consumers:
- `NavAgentSystem`: resolves the agent's class (`NavClassComponent`) → `ctx.Nav->FindPathForClass(classId, …)`.
- `KinematicMovementSystem`: resolves the entity's class → `ctx.Nav->MoveAlongSurfaceForClass(classId, …)`; planar/AABB layering from the base feature is unchanged.

### 6. Editor

- **Navigation panel** (`NavigationPanel.cpp`): edit the global params, plus a class-list editor —
  add class (up to `kMaxNavClasses`), remove last class, edit each class's radius/height/climb.
  Edits flow through the existing nav-config command/persist path. Display each as
  `"Class N — r=0.50 h=1.80 climb=0.40"`.
- **Inspector** (`NavClassEditor`): a combo selecting `ClassId` over `0..ClassCount-1`, mirroring the
  marker-editor pattern; pushes an `AddComponent`/`ModifyComponent` command.

### 7. Error handling

- `ClassCount==0` → one default class built (no empty-nav regression).
- `ClassId` out of range on an entity → class 0 + one-shot `SM_WARN` (no crash, no silent wrong-mesh).
- Out-of-range `classId` into any `Current(classId)` / `…ForClass` → slot 0 / `HasMesh` false /
  `desiredEnd` returned (mirrors the no-mesh guards).
- `ClassCount > kMaxNavClasses` clamped at author/load time + `SM_WARN`.
- Building N meshes multiplies bake cost ~N×; acceptable for the prototype (2–3 classes). Logged via
  the existing rebuild logging.

### 8. Migration & Disk-Bake Interaction

- Legacy world.json (flat agent params, no class list) → Class 0 synthesized on load (see §1).
- The existing single-mesh disk bake (`SaveToFile`/`LoadFromFile`/`TryLoadFromDisk`,
  `SaveCurrentToDisk`, auto-bake on `SetWorldPath`) is **not** extended this spec. To avoid loading
  a stale single-class bake as if it were the multi-class world, multi-class scenes **always
  Rebuild at runtime**: `TryLoadFromDisk` is bypassed/short-circuited when `ClassCount > 1` (it may
  still serve the single-class/class-0 case). The multi-class disk bake is the named follow-up.

## Testing

- **Unit (`tests/test_navmesh.cpp`):**
  - Multi-class build: floor + wall soup, config with Class 0 (r small) and Class 1 (r large) →
    `Current(0)` and `Current(1)` both non-null; a point near the wall is on the small mesh but
    OFF the large mesh (`ClosestPoint` distance / `FindPath` reachability divergence proving
    different erosion).
  - `NavLiveClassCount` helper: `ClassCount` clamped to `>= 1`.
  - `ResolveNavClass` helper: entity with `NavClassComponent{2}` → 2; absent → 0; `ClassId>=count`
    → 0.
  - Obstacle fan-out: add a cylinder obstacle → path blocked on BOTH class meshes; remove → restored
    on both.
  - `NavServices` `…ForClass`: `HasMeshForClass`/`FindPathForClass`/`MoveAlongSurfaceForClass` per
    class; out-of-range classId → class-0 / empty / passthrough; legacy `HasMesh` etc. still map to
    class 0.
  - Migration: a `NavMeshConfigComponent` json with legacy flat fields and no class array →
    `from_json` yields `ClassCount==1` with Class 0 = legacy values.
- **Manual:** author two classes (player r, boss r) in the Navigation panel; set a small entity to
  Class 0 and a large entity to Class 1; bake; in Play mode the large entity keeps farther from
  walls/obstacles than the small one and neither body pokes through; legacy single-class worlds load
  unchanged; `runtime.exe` honors per-class constraint.

## Components & Boundaries

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `NavClassConfig` + `NavMeshConfigComponent.Classes[]` | Author N radius classes (fixed-cap) | — |
| `NavMesh::Build(soup, cfg, cls)` | Build one mesh from a chosen class's agent params | Detour |
| `NavClassComponent` + plumbing + `NavClassEditor` | Per-entity class selection | ECS registration, ImGui |
| `NavLiveClassCount` / `ResolveNavClass` helpers | Class-count clamp + entity→class resolution (unit-tested) | ECS, glm |
| `NavMeshSystem` (N slots + obstacle fan-out) | Build/hold/publish per-class meshes; logical-id obstacles | NavMesh, Detour |
| `NavServices` `…ForClass` + forwarders | Class-aware query bridge to Game.dll | NavMeshSystem |
| `NavAgentSystem` / `KinematicMovementSystem` | Route by entity class | SystemContext.Nav |
| `NavigationPanel` class-list editor | Author classes | ImGui, nav-config command path |

## Files Touched

- `src/common/include/ECS.h` — `kMaxNavClasses`, `NavClassConfig`, reshape `NavMeshConfigComponent` (drop flat agent fields, add `Classes[]`/`ClassCount`), `NavClassComponent` + X-macro.
- `src/engine/src/navigation/NavMesh.{h,cpp}` — `Build` takes a `NavClassConfig` (reads agent params from it).
- `src/common/include/NavClass.h` (new) — `NavLiveClassCount`, `ResolveNavClass`.
- `src/common/include/ECSCommands.h` — `NavClassComponent` Apply/Remove/Copy.
- `src/common/include/ComponentSerialization.h` — `NavMeshConfigComponent` class-array (de)serialize + legacy migration; `NavClassComponent`.
- `src/engine/src/utilities/WorldManager.cpp` — `NavClassComponent` save/load (Environment for the config already handled).
- `src/engine/src/navigation/NavMeshSystem.{h,cpp}` — `m_Classes[]`, per-class build/publish/`Current(classId)`, obstacle fan-out via logical ids, `ConfigForClass`, multi-class `TryLoadFromDisk` short-circuit.
- `src/common/include/NavServices.h` — append `…ForClass` fn pointers.
- `src/engine/src/navigation/NavServicesImpl.cpp` — `…ForClass` forwarders + wiring; old fns → class 0.
- `src/game/src/NavAgentSystem.h` — resolve class → `FindPathForClass`.
- `src/game/src/game.cpp` — `KinematicMovementSystem` resolve class → `MoveAlongSurfaceForClass`.
- `src/editor/src/panels/NavigationPanel.cpp` — class-list editor.
- `src/editor/src/panels/inspector/NavClassEditor.{h,cpp}` (new) + registration + `src/editor/CMakeLists.txt`.
- `tests/test_navmesh.cpp` / `tests/test_worldserial.cpp` — multi-class / fan-out / forwarder / helper / serialization cases.
- Build-output `world.json` — hand-updated to the `Classes` schema (runtime data, not committed).

## Build / Reload Note

Reshaping `NavMeshConfigComponent` + adding `NavClassComponent` changes the ECS struct set → per
`CLAUDE.md`: rebuild `ecs.dll`, `editor`, `game`, then **restart the editor**. `GAME_API_VERSION`
unchanged (no `Game.h` layout change).

## Follow-up (separate spec, after this works)

**Multi-class disk bake:** extend the bake file format / `NavMeshSystem` disk path to persist and
reload all N class meshes (N files or one multi-section file) with per-mesh staleness, and lift the
`ClassCount > 1` `TryLoadFromDisk` short-circuit.
