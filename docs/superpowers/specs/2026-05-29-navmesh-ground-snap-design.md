# Navmesh-Driven Player Y (Ground Snap)

**Date:** 2026-05-29
**Status:** Approved, pending implementation plan

## Problem / Goal

Navmesh-constrained movement currently preserves the entity's **input Y** (planar-Y stopgap): the
nav constraint applies only the constrained X/Z and keeps `desired.y` (always 0 for the WASD player).
That stopgap existed because the navmesh was added after the movement code, when the player Y was
just 0 and there was no vertical concept. For the target ARPG, the player should **stick to the
navmesh floor** — which is also what makes walking up ramps/stairs work.

**Goal:** make the navmesh drive a `NavConstrained` entity's Y — ground-snap it to the navmesh
surface each move — replacing the planar-Y stopgap. `MoveAlongSurface` already returns the
constrained point **on the destination poly at its surface height**, so `clamped.y` is the ground
height at the new XZ (it follows connected ramp/step polys within the class's slope/climb limits).
The stopgap was literally discarding that Y.

## Decisions (from brainstorming)

1. **Ground offset = derived from the collider.** Snap so the collider's *base* rests on the
   surface; no magic number, survives resizing.
2. **Direct Y snap, bypassing the AABB vertical axis.** X/Z resolve through the navmesh constraint +
   AABB (as today); Y is set directly to the surface (no collider can "block" the floor snap, and it
   avoids per-tick AABB-Y delta fighting).

## Non-Goals

- No vertical physics (gravity, jumping, falling) — Y is pure ground placement.
- No Y smoothing/lerp (per-tick step = slope × move distance, already small).
- No change to nav **agents** — they aren't `NavConstrained` and already follow their on-mesh path
  waypoints' Y.
- No re-snap of a stationary entity (the existing zero-delta early-out means Y only updates while
  moving; first move after spawn corrects an authored-Y mismatch).

## Background (verified)

- `KinematicMovementSystem` (`src/game/src/game.cpp`, Physics phase): for `NavConstrained` entities it
  computes `clamped = ctx.Nav->MoveAlongSurfaceForClass(navClass, transform->Position, end)` then —
  today — `navDesired = {clamped.x - pos.x, desired.y, clamped.z - pos.z}` (planar-Y), feeds
  `navDesired` to `ResolveKinematicMove` (axis-separated AABB), applies the delta, clears intent.
- `clamped.y` from Detour `moveAlongSurface` is the surface height at the destination poly.
- `Collision.h` has `ComputeColliderHalfExtents(transform, collider)` (scaled half-extents) and
  `ComputeColliderCenter` (`position + collider.Offset * scale`). Collider base
  = center.y − halfExtents.y = `pos.y + Offset.y*scale.y − halfExtents.y`.
- `IsometricFollowCameraSystem` aims at the player's full `Position` (incl. Y) → it tracks the player
  up ramps automatically; no camera change needed.

## Design

### 1. `GroundOffset` helper (`Collision.h`, pure)

```cpp
// Distance from the transform origin DOWN to the collider's base. Setting
// Position.Y = surfaceY + GroundOffset(...) puts the collider base on the surface.
// No collider → 0 (origin sits on the surface).
inline float GroundOffset(const TransformComponent& transform, const ColliderComponent& collider) {
    return ComputeColliderHalfExtents(transform, collider).y
         - collider.Offset.y * transform.Scale.y;
}
```

### 2. `KinematicMovementSystem` ground-snap (`game.cpp`)

Replace the planar-Y line + add a direct Y snap after the X/Z apply:

```cpp
glm::vec3 navDesired = desired;
bool  groundSnap = false;
float groundY    = 0.0f;
if (ctx.world.HasComponent<NavConstrainedComponent>(e) && ctx.Nav && ctx.Nav->HasMesh()) {
    const uint8_t navClass = ResolveNavClass(ctx.world, e, classCount);
    const glm::vec3 end     = transform->Position + desired;
    const glm::vec3 clamped = ctx.Nav->MoveAlongSurfaceForClass(navClass, transform->Position, end);
    navDesired = glm::vec3(clamped.x - transform->Position.x, 0.0f,
                           clamped.z - transform->Position.z);   // XZ only; Y via snap
    groundSnap = true;
    if (const auto* col = ctx.world.GetComponent<ColliderComponent>(e))
        groundY = clamped.y + GroundOffset(*transform, *col);
    else
        groundY = clamped.y;
}

glm::vec3 applied = navDesired;
if (const auto* collider = ctx.world.GetComponent<ColliderComponent>(e))
    applied = ResolveKinematicMove(ctx.world, e, *transform, *collider, navDesired).AppliedDelta;

const bool moved = (applied.x != 0.0f || applied.y != 0.0f || applied.z != 0.0f);
if (moved || groundSnap) {
    ctx.world.Modify<TransformComponent>(e, [&](TransformComponent& t){
        t.Position += applied;                     // applied.y is 0 on the nav path; full delta for non-nav movers
        if (groundSnap) t.Position.y = groundY;    // direct ground placement (overrides the nav path's 0-Y)
    });
}
// clear intent (unchanged)
```

Non-`NavConstrained` movers keep their existing behavior: `groundSnap=false`, `navDesired == desired`
(incl. any Y), applied through AABB as before.

### 3. Error handling

- No navmesh / not `NavConstrained` → `groundSnap=false`, Y untouched (today's behavior).
- Off-mesh recovery → `clamped.y` is the nearest-poly surface → snaps the entity back onto the floor.
- Too-steep / too-high geometry (beyond the class's `AgentMaxSlope`/`AgentMaxClimb`) isn't in the
  navmesh, so `moveAlongSurface` won't route there → blocked at the walkable edge (correct).

## Testing

- **Unit:** `GroundOffset` is pure → factored into `Collision.h`. (No game-side unit-test target
  exists and the formula is inspection-trivial; rely on manual verification rather than adding a new
  target.)
- **Manual:** walk the player up a ramp / onto a raised walkable platform in Play mode → it climbs
  and the collider base sits on the surface; flat ground behaves as before; a slope steeper than the
  class limit is blocked at the edge; the follow camera tracks the height.

## Components & Boundaries

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `GroundOffset` (`Collision.h`) | Pure origin→collider-base offset | TransformComponent, ColliderComponent |
| `KinematicMovementSystem` (`game.cpp`) | XZ via constraint+AABB; direct Y ground-snap for NavConstrained | SystemContext.Nav, Collision.h, ResolveNavClass |

## Files Touched

- `src/game/src/Collision.h` — `GroundOffset` helper.
- `src/game/src/game.cpp` — `KinematicMovementSystem` ground-snap (replaces the planar-Y line).

## Build / Reload Note

`.cpp`/header changes only in the game library — `cmake --build … --target game` hot-reloads; no
`GAME_API_VERSION` bump, no ECS struct change, no editor restart needed.
