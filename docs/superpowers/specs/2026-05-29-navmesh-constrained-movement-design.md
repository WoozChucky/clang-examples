# Navmesh-Constrained Movement

**Date:** 2026-05-29
**Status:** Approved, pending implementation plan

## Problem / Goal

Today the player moves freely from WASD and is only blocked by axis-separated AABB
collision against **static** `ColliderComponent`s (`Collision.h` →
`KinematicMovementSystem`). A baked Recast/Detour navmesh already exists but only drives
**AI pathing** (`NavAgentSystem` follows a cached path). It does not constrain the
directly-controlled player at all.

**Goal:** let opt-in entities have their per-tick movement clamped to the walkable
navmesh — the player cannot walk off the mesh or into baked obstacles, and slides along
boundaries instead of sticking. Keep the existing AABB collision layered on top for
dynamic/non-baked blockers.

## Decisions (from brainstorming)

1. **Boundary behavior:** wall-slide via `dtNavMeshQuery::moveAlongSurface` (canonical
   Detour constraint for a directly-controlled character; smooth, no popping).
2. **Scope:** opt-in **marker component** (`NavConstrainedComponent`) — added to the player
   today, reusable for any future directly-controlled mover. Not hardcoded to
   `PlayerComponent`, not forced on all movers.
3. **Collision combo:** **layered** — navmesh `moveAlongSurface` clamps the move to the
   walkable area first, then the existing `ResolveKinematicMove` (AABB) runs on the result
   for dynamic/non-baked colliders.
4. **Off-mesh fallback:** **recover toward nearest poly** — if the entity's start position
   finds no poly, use `ClosestPoint` to pull it back toward the nearest walkable point;
   normal `moveAlongSurface` resumes once back on-mesh. Self-healing, never permanently
   stuck. `SM_WARN` once per occurrence (rate-limited).

## Non-Goals

- No change to `NavAgentSystem` — AI agents already path on-mesh; they are not given the
  marker.
- No change to free (non-constrained) entities — without the marker, behavior is
  byte-identical to today (AABB only).
- No gravity / vertical physics. `moveAlongSurface` returns the navmesh surface height, so
  a constrained entity tracks floor height; this is acceptable because the player currently
  has zero Y motion (`ComputePlanarMove` never touches Y).
- No dynamic-obstacle navmesh re-bake changes (obstacle system unchanged).

## Background (verified)

- `SystemContext.Nav` (`Systems.h`) is set every tick for **all** phases, so the
  Physics-phase `KinematicMovementSystem` can call `ctx.Nav` exactly as the Simulation-phase
  `NavAgentSystem` does.
- `KinematicMovementSystem` (`game.cpp`) is the **sole** owner of movement application:
  reads `MoveIntentComponent.DesiredDelta`, runs `ResolveKinematicMove` (when a
  `ColliderComponent` is present), applies to `TransformComponent`, clears the intent.
- `NavMesh` (`src/engine/src/navigation/NavMesh.h`) already exposes `FindPath` and
  `ClosestPoint(world, out)` (snap to nearest poly within ~2m XZ / 4m Y extents). It owns a
  `dtNavMeshQuery`. GameThread-only (asserted; `dtNavMeshQuery` is not thread-safe).
- `NavServices` (`src/common/include/NavServices.h`) is the Game.dll↔Engine bridge table of
  function pointers — **append-only** (reordering/removing shifts offsets and breaks
  `Game.dll` binary compat). Currently exposes `HasMesh`/`FindPath`/`NavVersion` + obstacle
  add/remove. It does **not** expose any per-frame surface-constraint query yet.
- Components are registered through the `ECS_FOR_EACH_REGISTERED_COMPONENT(X)` X-macro in
  `ECS.h`; each registered type also needs `ECSCommandProcessor` Apply/Remove handling
  (`ECSCommands.h`) and (de)serialization (`ComponentSerialization.h`). Inspector editors
  use a registry (`src/editor/src/panels/inspector/`).

## Design

### Data flow

```
PlayerMovementSystem (Simulation)  → writes MoveIntent.DesiredDelta        [UNCHANGED]
NavAgentSystem       (Simulation)  → writes MoveIntent.DesiredDelta (AI)   [UNCHANGED]

KinematicMovementSystem (Physics):
    desired = intent.DesiredDelta
    if has NavConstrainedComponent && ctx.Nav && ctx.Nav->HasMesh():
        end     = transform.Position + desired
        clamped = ctx.Nav->MoveAlongSurface(transform.Position, end)   // wall-slide / recovery
        desired = clamped - transform.Position
    if has ColliderComponent:
        desired = ResolveKinematicMove(world, e, transform, collider, desired).AppliedDelta
    transform.Position += desired
    clear intent.DesiredDelta
```

`NavConstrained` off → the first `if` is skipped → identical to today. Navmesh absent
(`HasMesh()==false`) → skipped → AABB only.

### 1. `NavMesh::ConstrainMove`

New method on `NavMesh` (`NavMesh.h/.cpp`):

```cpp
// Constrain a desired move to the navmesh surface (wall-slide). Returns the
// constrained world end position. If `start` is off-mesh (no poly within query
// extents), returns a position pulled toward the nearest poly (recovery) and logs
// a rate-limited SM_WARN; if even ClosestPoint fails, returns `start` unchanged
// (no movement). GameThread only.
glm::vec3 ConstrainMove(const glm::vec3& start, const glm::vec3& desiredEnd) const;
```

Implementation:
1. `m_Query->findNearestPoly(start, halfExtents, &filter, &startRef, nearestPt)`.
2. `startRef != 0` → `m_Query->moveAlongSurface(startRef, start, desiredEnd, &filter,
   resultPos, visited, &visitedCount, kMaxVisited)`; return `resultPos`.
3. `startRef == 0` (off-mesh) → `ClosestPoint(start, nearest)`:
   - success → return `nearest` (pull back onto mesh; subsequent ticks resume normal
     slide); `SM_WARN` rate-limited.
   - failure → return `start` (no movement this tick).

`halfExtents` reuse the `ClosestPoint` defaults (~2m XZ / 4m Y). `kMaxVisited` small fixed
buffer (e.g. 16) — visited polys are discarded (we only need `resultPos`).

### 2. `NavServices.MoveAlongSurface`

Append one field to the **end** of `NavServices` (append-only contract):

```cpp
// Constrain a move to the navmesh surface (wall-slide via dtNavMeshQuery::
// moveAlongSurface). Off-mesh start → pulls toward nearest poly (recovery).
// Returns the constrained end position (== start if fully off-mesh). GameThread only.
glm::vec3 (*MoveAlongSurface)(const glm::vec3& start, const glm::vec3& desiredEnd);
```

Wire it in `NavServicesImpl.cpp` mirroring `FindPath`: load the published
`shared_ptr<const NavMesh>`; null → return `desiredEnd` unchanged (no mesh = no constraint,
matches the `HasMesh()` guard caller-side); non-null → forward to `NavMesh::ConstrainMove`.

### 3. `NavConstrainedComponent`

Empty marker (no fields). Mirror an existing trivial marker (`SunMarker`).

- **`ECS.h`** — declare `struct NavConstrainedComponent {};` and add
  `X(NavConstrainedComponent)` to `ECS_FOR_EACH_REGISTERED_COMPONENT`.
- **`ECSCommands.h`** — add `AddComponent`/`ModifyComponent` dispatch in
  `ApplyComponentCommand` and the `RemoveComponent` branch in `RemoveComponentByType`.
- **`ComponentSerialization.h`** — (de)serialize (no fields → presence-only object `{}`,
  matching how marker components persist).
- **Inspector editor** — `NavConstrainedEditor` in
  `src/editor/src/panels/inspector/`, registered like the other component editors; renders a
  one-line "constrained to navmesh" note + remove affordance (no fields to edit).
- Add the marker to the player entity at world seed (`game.cpp`, where the player is
  created / `world.json`).

### 4. `KinematicMovementSystem`

Insert the navmesh-constrain step shown in **Data flow**, before the existing
`ResolveKinematicMove` call. Reads the marker via `ctx.world.HasComponent`,
guards on `ctx.Nav && ctx.Nav->HasMesh()`.

### 5. Error handling

- Navmesh absent → constraint skipped (`HasMesh()` guard); AABB only.
- `ctx.Nav == nullptr` (test harness / pre-init) → constraint skipped.
- Start off-mesh → recovery toward nearest poly (`ClosestPoint`), rate-limited `SM_WARN`.
- Even `ClosestPoint` fails → return `start` (zero applied delta this tick; not frozen
  permanently — player can be re-baked back onto the mesh / the marker can be removed).

## Testing

- **Integration (`tests/test_navmesh.cpp`, matches existing nav tests):** build a small mesh
  with a wall / boundary, then:
  - move toward open space → result reaches (≈) the desired end, on-mesh.
  - move straight at a boundary → result stays on-mesh, slides tangentially (does not cross).
  - move from an off-mesh start → result is pulled toward the nearest poly (distance to mesh
    decreases), recovery path exercised.
- **Manual:** add the marker to the player; walk into a baked wall/obstacle → player slides,
  never leaves the walkable area; remove the marker → free movement returns; with no navmesh
  baked → behaves as today; `runtime.exe` honors it (constraint is engine-side, marker is
  per-scene world data).

`moveAlongSurface` is Detour-internal (not pure-mathable), so no `SsaoMath`-style pure unit
test; integration tests over a real built mesh are the established nav pattern.

## Components & Boundaries

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `NavMesh::ConstrainMove` | Wrap findNearestPoly + moveAlongSurface + ClosestPoint recovery | Detour, glm |
| `NavServices.MoveAlongSurface` + impl | Expose constraint to Game.dll, null-mesh guard | NavMesh, NavMeshSystem |
| `NavConstrainedComponent` | Opt-in marker | ECS registration / commands / serialization / inspector |
| `KinematicMovementSystem` | Layered navmesh-then-AABB constraint | SystemContext.Nav, Collision.h |
| `test_navmesh.cpp` cases | On-mesh / slide / recovery verification | NavMesh |

## Files Touched

- `src/engine/src/navigation/NavMesh.{h,cpp}` — `ConstrainMove`.
- `src/common/include/NavServices.h` — append `MoveAlongSurface` field.
- `src/engine/src/navigation/NavServicesImpl.cpp` — wire `MoveAlongSurface`.
- `src/common/include/ECS.h` — `NavConstrainedComponent` struct + X-macro line.
- `src/common/include/ECSCommands.h` — Apply/Remove dispatch.
- `src/common/include/ComponentSerialization.h` — (de)serialize the marker.
- `src/editor/src/panels/inspector/NavConstrainedEditor.{h,cpp}` (new) + registration +
  `src/editor/CMakeLists.txt`.
- `src/game/src/game.cpp` — navmesh-constrain step in `KinematicMovementSystem`; add marker
  to the player entity.
- `tests/test_navmesh.cpp` — constraint cases.

## Build / Reload Note

Adding a registered ECS component changes struct layout → per `CLAUDE.md`: rebuild
`ecs.dll`, `editor`, and `game`, then **restart the editor** (the running `editor.exe` has
the old component set linked in). Bump `GAME_API_VERSION` only if `Game.h` layout changes
(it does not here — the marker lives in `ECS.h`).
