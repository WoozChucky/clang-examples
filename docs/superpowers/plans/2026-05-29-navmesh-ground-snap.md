# Navmesh Ground-Snap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a `NavConstrained` entity's Y follow the navmesh surface (ground-snap), replacing the planar-Y stopgap, so the player walks up ramps/stairs and rests on the floor.

**Architecture:** In `KinematicMovementSystem`, the nav constraint already yields `clamped` (the on-mesh point, whose `.y` is the surface height at the new XZ). Apply X/Z through the navmesh constraint + AABB as today, but set Y **directly** to `clamped.y + GroundOffset(collider)` (collider base on surface), bypassing the AABB vertical axis. Pure offset helper in `Collision.h`.

**Tech Stack:** C++23, project ECS, glm, Recast/Detour (via existing `NavServices`).

**Build/preset (project memory):** `msvc-win64-vs2026-community` only. Game-lib only change → `cmake --build … --target game` hot-reloads; no editor restart, no `GAME_API_VERSION` bump.

**Commit identity (project memory):** `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit …`. Never `--no-verify`.

**Branch:** `feat/navmesh-ground-snap`.

**No automated test:** `GroundOffset` is a trivial pure formula and there is no game-side unit-test target; verification is the build + manual ramp test (consistent with the project's manual-verify stance for game-system behavior). Do NOT add a new test target.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/game/src/Collision.h` | `GroundOffset(transform, collider)` pure helper (origin → collider-base offset) |
| `src/game/src/game.cpp` | `KinematicMovementSystem`: XZ via constraint+AABB, direct Y ground-snap |

---

## Task 1: `GroundOffset` helper

**Files:**
- Modify: `src/game/src/Collision.h` (after `ComputeColliderCenter`, ~line 60)

- [ ] **Step 1: Add the helper**

In `Collision.h`, immediately after the `ComputeColliderCenter(...)` function, add:

```cpp
// Distance from the transform origin DOWN to the collider's base, so that
// Position.Y = surfaceY + GroundOffset(...) places the collider base on the
// navmesh surface. Used by navmesh ground-snap. (collider base
// = center.y - halfExtents.y = Position.y + Offset.y*scale.y - halfExtents.y,
// so the origin sits halfExtents.y - Offset.y*scale.y above the base.)
inline float GroundOffset(const TransformComponent& transform, const ColliderComponent& collider)
{
    return ComputeColliderHalfExtents(transform, collider).y
         - collider.Offset.y * transform.Scale.y;
}
```

- [ ] **Step 2: Build the game library to verify it compiles**

Run: `cmake --build --preset msvc-win64-vs2026-community --target game`
Expected: builds clean (no caller yet — Task 2 adds it; the inline fn just compiles).

- [ ] **Step 3: Commit**

```bash
git add src/game/src/Collision.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(game): GroundOffset helper (transform origin to collider base)"
```

---

## Task 2: Ground-snap in `KinematicMovementSystem`

**Files:**
- Modify: `src/game/src/game.cpp` (`KinematicMovementSystem::Update`, lines ~277-304)

- [ ] **Step 1: Replace the nav block + apply block**

Replace the current navmesh-constraint block AND the apply block (the code from the `// Navmesh constraint (opt-in …)` comment through the `Modify<TransformComponent>` that does `t.Position += applied;`) with the version below. The `desired`/`transform`/`intent`/early-out lines above and the clear-after-consume `Modify` below are UNCHANGED.

```cpp
            // Navmesh constraint (opt-in via NavConstrainedComponent): wall-slide
            // the X/Z move along the walkable surface, and ground-snap Y to the
            // surface height (follows ramps/stairs, rests on the floor). Skipped
            // when no marker / no nav table / no mesh built.
            glm::vec3 navDesired = desired;
            bool  groundSnap = false;
            float groundY    = 0.0f;
            if (ctx.world.HasComponent<NavConstrainedComponent>(e)
                && ctx.Nav && ctx.Nav->HasMesh()) {
                const glm::vec3 end     = transform->Position + desired;
                const uint8_t navClass  = ResolveNavClass(ctx.world, e, classCount);
                const glm::vec3 clamped  = ctx.Nav->MoveAlongSurfaceForClass(navClass, transform->Position, end);
                // X/Z from the navmesh wall-slide; Y set directly below (ground-snap),
                // NOT through the AABB resolver — pure floor placement, no vertical physics.
                navDesired = glm::vec3(clamped.x - transform->Position.x,
                                       0.0f,
                                       clamped.z - transform->Position.z);
                groundSnap = true;
                if (const auto* col = ctx.world.GetComponent<ColliderComponent>(e))
                    groundY = clamped.y + GroundOffset(*transform, *col);
                else
                    groundY = clamped.y;
            }

            glm::vec3 applied = navDesired;
            if (const auto* collider = ctx.world.GetComponent<ColliderComponent>(e)) {
                applied = ResolveKinematicMove(ctx.world, e, *transform, *collider, navDesired).AppliedDelta;
            }

            if (applied.x != 0.0f || applied.y != 0.0f || applied.z != 0.0f || groundSnap) {
                ctx.world.Modify<TransformComponent>(e, [&](TransformComponent& t){
                    t.Position += applied;                 // applied.y is 0 on the nav path; full delta for non-nav movers
                    if (groundSnap) t.Position.y = groundY; // direct ground placement (overrides nav path's 0-Y)
                });
            }
```

Notes for the implementer:
- `GroundOffset` is from `Collision.h` (already `#include`d in `game.cpp`); `ResolveNavClass`/`NavLiveClassCount` from `NavClass.h` (already included by the multi-class work); `classCount` is already computed once above the `Each` loop.
- Non-`NavConstrained` movers are unchanged: `groundSnap` stays false, `navDesired == desired` (incl. any Y), applied through the AABB resolver and the full `applied` (incl. Y) as before.

- [ ] **Step 2: Build the game library**

Run: `cmake --build --preset msvc-win64-vs2026-community --target game`
Expected: builds clean.

- [ ] **Step 3: Commit**

```bash
git add src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(game): navmesh-driven player Y (ground-snap, replaces planar-Y stopgap)"
```

---

## Task 3: Manual verification

No code. The game lib hot-reloads (no editor restart needed) — rebuild `game` and let the running editor pick it up, or restart if it's not running.

- [ ] **Step 1: Build:** `cmake --build --preset msvc-win64-vs2026-community --target game`.
- [ ] **Step 2: Scene with elevation** — ensure the level has a ramp / raised walkable platform baked into the navmesh (NavMeshSource geometry the agent radius can climb; slope within the class's `AgentMaxSlope`, steps within `AgentMaxClimb`). Rebuild NavMesh if needed.
- [ ] **Step 3: Play mode** — drive the player up the ramp/onto the platform → it climbs and the collider base sits on the surface (no sinking/floating); flat-ground movement looks unchanged; a slope steeper than the class limit is blocked at the walkable edge; the follow camera tracks the height.
- [ ] **Step 4 (if a code fix was needed): commit it.**

```bash
git add -A
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "fix(game): <describe ground-snap fix>"
```

---

## Self-Review (completed during authoring)

- **Spec coverage:** §1 `GroundOffset` (Task 1); §2 ground-snap in `KinematicMovementSystem` (Task 2); §3 error handling (no-mesh/not-constrained skip, off-mesh recovery snaps to nearest poly, too-steep blocked by the navmesh) — covered by the skip/`groundSnap` gating and the unchanged constraint behavior; testing = manual (Task 3). All mapped.
- **Apply-guard:** condition is `applied.x||y||z || groundSnap`, so a `NavConstrained` entity that only ground-snaps (no XZ delta this tick — e.g. moveAlongSurface clamped XZ to ~0 at a wall while Y changes) still applies the snap, and a non-nav pure-Y mover still applies (the `||y` term). No regression vs the original `applied != 0` guard.
- **Type consistency:** `GroundOffset(const TransformComponent&, const ColliderComponent&) → float` defined in Task 1, called identically in Task 2; `clamped`, `navClass`, `classCount`, `ResolveNavClass`, `MoveAlongSurfaceForClass` match the current code.
- **Placeholders:** none — full code + exact commands per step.
