# Movement Intent / Collision-Resolution Decoupling — Design

**Date:** 2026-05-27
**Status:** Approved (design); pending spec review
**Scope:** ECS (one new runtime component) + game.cpp (one new system + one trimmed). No render, no editor, no serialization.

## Problem

`PlayerMovementSystem` currently does three things:

1. **Intent**: read input → compute desired delta.
2. **Resolution**: look up `ColliderComponent`, call `ResolveKinematicMove`.
3. **Apply**: write the resolved delta to `TransformComponent.Position`.

Concerns 2+3 are not player-specific — every future kinematic mover (AI, projectiles, kinematic platforms) will reimplement the same dance against `Collision.h`. The coupling forces "what does this entity want?" and "how does the world allow it?" into the same system.

## Goal

Decouple intent from resolution via the standard ECS pattern: movement systems publish a per-tick **`MoveIntentComponent`**; a single **`KinematicMovementSystem`** consumes it, resolves against colliders (when present), applies to Transform, and clears the intent. PlayerMovementSystem becomes single-concern ("read input → write intent"). Future movers reuse the resolver with zero new code paths.

## Mental model

- **Movement systems** (PlayerMovement today, AI/projectile tomorrow): produce a `vec3 DesiredDelta` per controlled entity per tick.
- **`KinematicMovementSystem`**: consume + resolve + apply. Single owner of `Collision.h` calls and `TransformComponent.Position` mutation from movement.
- **`ResolveKinematicMove`**: unchanged. Pure helper, already tested.

The boundary is "what you want" (entity-local input/AI logic) vs "what the world permits" (collider-aware resolution). Same pattern as the menu's "intent (ActionEvent) vs handler (AppFlowSystem)" — applied to movement.

## System phases (architectural seam for future physics)

Introduce a dedicated `Physics` phase between `Simulation` and `PostSimulation` so phases gain meaning: **decide → resolve → react**. This is the seam where a future physics engine (Jolt / Bullet) plugs in — `KinematicMovementSystem` lives there today, a future `PhysicsStepSystem` (rigid bodies) lives there tomorrow. No bundling of two concerns into one phase.

**`src/common/include/Systems.h`** — extend the enum:

```cpp
enum class SystemPhase : uint8_t {
    Input          = 0,   // (future) input-derived state
    Simulation     = 1,   // gameplay decisions / intent (PlayerMovement, AI, MenuInteraction, AppFlow, ...)
    Physics        = 2,   // spatial resolution against the world (KinematicMovementSystem + future RigidBodyStep)
    PostSimulation = 3,   // reactions to the resolved world (camera follow, animation triggers, audio cues)
    PreRender      = 4,   // last-chance ECS prep before snapshot
};
```

Numeric renumber (`PostSimulation` 2→3, `PreRender` 3→4) is safe: the scheduler sorts by (phase, registration index), so adjusting values just changes the sort, which is the intended effect.

**Phase migrations driven by this change:**

| System | Phase before | Phase after | Reason |
|---|---|---|---|
| `PlayerMovementSystem` | Simulation | **Simulation** (unchanged) | Decides intent. |
| `KinematicMovementSystem` (new) | — | **Physics** | Resolves intent against colliders. |
| `CameraZoomSystem` | Simulation | **PostSimulation** | Reacts to the post-resolved player position. |
| `IsometricFollowCameraSystem` | Simulation | **PostSimulation** | Reacts to the post-resolved player position. |

All other systems (TextRotation, DayNight, MenuInteraction, AppFlow, DebugSpawn) stay in Simulation — they're either gameplay decisions or ambient demo systems with no physics dependency. Bikeshedding their phase categorization is deferred until they have a concrete reason to move.

**Correctness win** (camera migration): today the cameras read the post-Player Transform because of intra-`Simulation` registration order. Moving them to `PostSimulation` makes the dependency explicit at the phase level — they no longer rely on "we happen to register after Player." With `KinematicMovementSystem` now between them and Player, this distinction matters: camera must run **after** the resolved Transform, not just after the intent-writing PlayerMovement.

## Architecture

### 1. New component

`src/common/include/ECS.h` — add to the component declarations (after `ColliderComponent`):

```cpp
// Per-entity per-tick movement intent: how much the entity WANTS to move this tick.
// Written by movement systems (PlayerMovementSystem, future AI/projectile systems);
// consumed by KinematicMovementSystem which resolves against colliders, applies to
// TransformComponent, and zeroes DesiredDelta (clear-after-consume — no per-tick
// add/remove churn). Runtime-only: not authored, not serialized, not in ECSCommands.
struct MoveIntentComponent {
    glm::vec3 DesiredDelta{0.0f};
};
```

Plus `X(MoveIntentComponent)` in the X-macro tail. **No** ECSCommands registration, **no** serialization, **no** inspector — runtime component, like `MenuStateComponent`/`ActionQueueComponent`.

### 2. New system

`src/game/src/game.cpp` — `KinematicMovementSystem` (Simulation phase, runs unconditionally — scope-by-data, not by state):

```cpp
class KinematicMovementSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        ctx.world.Each<TransformComponent, MoveIntentComponent>([&](EntityId e) {
            const auto* intent = ctx.world.GetComponent<MoveIntentComponent>(e);
            if (!intent) return;
            const glm::vec3 desired = intent->DesiredDelta;
            if (desired.x == 0.0f && desired.y == 0.0f && desired.z == 0.0f) return;

            const auto* transform = ctx.world.GetComponent<TransformComponent>(e);
            if (!transform) return;

            glm::vec3 applied = desired;
            if (const auto* collider = ctx.world.GetComponent<ColliderComponent>(e)) {
                applied = ResolveKinematicMove(ctx.world, e, *transform, *collider, desired).AppliedDelta;
            }

            if (applied.x != 0.0f || applied.y != 0.0f || applied.z != 0.0f) {
                ctx.world.Modify<TransformComponent>(e, [&](TransformComponent& t){ t.Position += applied; });
            }
            // Clear-after-consume: zero the intent so a stale value doesn't re-fire next tick
            // if the producer didn't run (e.g. gating turned a movement system off).
            ctx.world.Modify<MoveIntentComponent>(e, [](MoveIntentComponent& m){ m.DesiredDelta = glm::vec3(0.0f); });
        });
    }
    const char* Name() const override { return "KinematicMovementSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Physics; }
};
```

**Why ungated:** any entity with MoveIntent should resolve. In MainMenu, `PlayerMovementSystem` is gated off → never writes intent → KinematicMovementSystem iterates an empty set and no-ops. Free, and lets future non-player movers (e.g. cutscene-driven actors) work in any state without re-gating.

**Why Physics phase:** the resolver is the "world's response to intent" — it lives one layer below gameplay decisions. When a real physics engine arrives, `PhysicsStepSystem` (rigid-body integration) joins the same phase, with `KinematicMovementSystem` skipping entities that have a `RigidBodyComponent` (physics owns their Transform). The phase becomes the single integration point for all spatial resolution.

### 3. PlayerMovementSystem after the refactor

`src/game/src/game.cpp` — `PlayerMovementSystem::Update` becomes:

```cpp
void Update(SystemContext& ctx) override {
    // Gameplay runs only in-level.
    const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
    if (!gs || gs->Current != GameStateId::InLevel) return;

    const auto* in = ctx.world.GetSingleton<InputStateComponent>();
    if (!in) return;
    const float dt = static_cast<float>(ctx.dt);

    ctx.world.Each<PlayerComponent, TransformComponent>([&](EntityId e) {
        float speed = 5.0f;
        if (const auto* p = ctx.world.GetComponent<PlayerComponent>(e)) speed = p->MoveSpeed;
        const glm::vec3 desiredDelta = ComputePlanarMove(*in, speed, dt, glm::radians(kPoE2Follow.YawDeg));
        if (desiredDelta.x == 0.0f && desiredDelta.y == 0.0f && desiredDelta.z == 0.0f) return;

        // Lazy-seed the intent on first write; subsequent ticks Modify in place.
        if (!ctx.world.HasComponent<MoveIntentComponent>(e)) {
            ctx.world.AddComponent(e, MoveIntentComponent{});
        }
        ctx.world.Modify<MoveIntentComponent>(e, [&](MoveIntentComponent& m){ m.DesiredDelta = desiredDelta; });
    });
}
```

**No more** `Transform` mutation, **no more** `ColliderComponent` lookup, **no more** `ResolveKinematicMove` call. One concern: "what does the player want?" The `#include "Collision.h"` in `game.cpp` stays (KinematicMovementSystem still uses it).

### 4. Seed strategy: lazy add by the producer

Any system that wants to drive a kinematic entity does `if (!HasComponent<MoveIntent>) AddComponent(MoveIntent{}); Modify(...)`. One-time check then per-tick Modify. Self-contained — no boot-time seeding coupling, no editor authoring required, works identically for player + future AI/projectiles.

### 5. Registration + phase order (load-bearing)

The scheduler sorts by **(phase, registration index)**. Phase ordering does the heavy lifting now; registration order is the tie-breaker within a phase.

```cpp
// Simulation phase (gameplay decisions / intent):
s->Register(TextRotation);          // Simulation
s->Register(DayNight);              // Simulation
s->Register(MenuInteraction);       // Simulation
s->Register(AppFlow);               // Simulation
s->Register(DebugSpawn);            // Simulation
s->Register(PlayerMovement);        // Simulation  ← writes MoveIntent

// Physics phase (spatial resolution): new system.
s->Register(KinematicMovement);     // Physics     ← reads MoveIntent, resolves, applies Transform

// PostSimulation phase (reactions): cameras migrated here from Simulation.
s->Register(CameraZoom);            // PostSimulation
s->Register(IsometricFollowCamera); // PostSimulation ← reads the post-resolution Transform
```

Same-tick chain (driven by phase, not registration order):
**input → intent (Simulation) → resolve+apply (Physics) → camera follows resolved position (PostSimulation)** → snapshot.

Camera continues to see the corrected player position the same frame; the dependency is now expressed at the phase level rather than relying on intra-phase registration order.

### 6. Clear-after-consume rationale

The resolver zeroes `DesiredDelta` after writing the Transform. Reason: if a producer system stops running next tick (gating flip, paused state, system removed), the entity should **not** keep drifting on stale intent. Zeroing is cheap (one Modify per moved entity per tick) and self-defending. Producers re-write fresh intent each tick anyway, so the zero is overwritten before the resolver runs again.

## Why not Approach B (Intent + Result) now

A `MoveResultComponent { AppliedDelta, BlockedX/Y/Z }` is trivial to add later if a consumer arrives (animation: "bump on block"; AI: "re-pathfind"). Adding it now without a consumer = speculative ECS surface area. YAGNI. The resolver becomes:

```cpp
ResolveKinematicMove(...) → AppliedDelta;
Modify<MoveResultComponent>(e, [&](MoveResultComponent& r){ r.AppliedDelta = applied; r.BlockedX = (applied.x != desired.x); ... });
```

— one extra write, additive. Defer.

## Why not Approach C (Velocity) now

Velocity belongs to entities with momentum (projectiles, AI with cruise speed, dropped pickups, future Jolt-style rigid bodies). The player is **direct WASD** — no momentum — so velocity collapses to `velocity = direction*speed` every tick = intent re-derived through one more component. When projectiles arrive, a tiny `VelocityIntegratorSystem` writes `MoveIntent.DesiredDelta = velocity * dt` and shares the **same** resolver. The intent layer is the right place to converge; velocity sits one layer above it. Add when needed.

## Testing

- `Collision.h` helpers untouched — `test_collision` stays green (no rebuild of those tests required, but they re-run as a non-regression check).
- New behavior is system-integration: best validated by manual smoke (player still moves + slides on walls). One pure-test add worth doing: a `T06_no_collider_full_delta` case in `test_collision` covering the "MoveIntent on an entity without a ColliderComponent applies the full desired delta unchanged" path the new resolver takes. Currently implicit in `T00`; an explicit no-collider case future-proofs the resolver's pass-through branch.

## Manual smoke

- Player moves with WASD (unchanged behavior).
- Player slides on a static collider (unchanged behavior).
- A second entity (e.g. a debug entity created via F12 + manually adding a MoveIntent via the inspector — *not* part of this work, just a thought experiment) would move using the same path.
- In MainMenu, no movement (PlayerMovement gated off → no MoveIntent written → KinematicMovementSystem no-ops).
- ESC → MainMenu → back to InLevel: movement resumes (the lazy-seeded MoveIntent survives but is zeroed by the resolver, so no stuck drift).

## Touch list

- `src/common/include/Systems.h` — add `SystemPhase::Physics = 2`; renumber `PostSimulation` → 3, `PreRender` → 4. Update the phase-purpose comments.
- `src/common/include/ECS.h` — `MoveIntentComponent` struct + X-macro entry. **ECS.h layout change → editor rebuild + restart, `GAME_API_VERSION` bump 13u → 14u.**
- `src/game/src/game.cpp` — new `KinematicMovementSystem` (Physics phase); trim `PlayerMovementSystem::Update`; change `CameraZoomSystem::Phase()` + `IsometricFollowCameraSystem::Phase()` from `Simulation` to `PostSimulation`; register `KinematicMovement` between PlayerMovement and the cameras (registration order doesn't matter across phases, but keep it readable).
- `src/game/include/game.h` — `GAME_API_VERSION` 13u → 14u.
- `tests/test_collision.cpp` — optional `T06_no_collider_full_delta` (1 case, ~15 LOC).

No changes to `Collision.h`, `ECSCommands.h`, `ComponentSerialization.h`, `WorldManager.cpp`, `EcsInspectorPanel.{h,cpp}`, or any render-side file. No editor UI surface. No persistence.

## Non-goals (YAGNI)

- `MoveResultComponent` — defer until a consumer needs it.
- `VelocityComponent` / integrator — defer until projectiles or AI with momentum arrive.
- Authored MoveIntent (editor inspector) — runtime-only by design.
- Persisted MoveIntent — runtime-only by design.
- Dynamic-vs-dynamic resolution — same v1 limitation as `Collision.h` (`!IsStatic → return false`). Out of scope here; lives in the next collision iteration.

## Acceptance

- Player behavior is bit-identical to current: WASD moves, walls block, sliding works, gating off in MainMenu.
- `PlayerMovementSystem` no longer references `TransformComponent.Position` mutation, `ColliderComponent`, or `ResolveKinematicMove`.
- `Collision.h` is included by `game.cpp` only via `KinematicMovementSystem`'s call to `ResolveKinematicMove`.
- All existing tests (`test_collision`, `test_ecs`, `test_menu`, `test_worldserial`) pass.
- A new mover system (e.g. a future `AIMovementSystem`) needs only to write `MoveIntent.DesiredDelta` — no collision-resolution code duplicated.
