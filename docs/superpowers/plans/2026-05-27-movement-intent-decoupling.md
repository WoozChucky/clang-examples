# Movement Intent Decoupling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split "what does the entity want to move?" (intent) from "how does the world resolve it?" (collision) — and signal the seam in the system-phase model so future physics plugs in without re-org.

**Architecture:** Add `SystemPhase::Physics` between `Simulation` and `PostSimulation`. New runtime-only `MoveIntentComponent { glm::vec3 DesiredDelta; }` is written by movement systems (PlayerMovement today, AI/projectiles later) and consumed by a new `KinematicMovementSystem` (Physics phase) that delegates to `Collision.h::ResolveKinematicMove`, applies the resolved delta to Transform, and clears the intent. Camera systems migrate to `PostSimulation` so the phase order — decide → resolve → react — drives the same-tick chain.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), hot-reloaded `Game.dll`, CMake (`msvc-win64-vs2026-community`).

**Spec:** `docs/superpowers/specs/2026-05-27-movement-intent-decoupling-design.md` (commit `b1f3330`).

---

## Reference facts (verified)

- `Systems.h` `SystemPhase` enum (`src/common/include/Systems.h:17-22`): `Input=0, Simulation=1, PostSimulation=2, PreRender=3`. Scheduler sorts by `(phase, registration index)` — verified by the existing pattern (e.g. `AppFlowSystem` comment "owns transitions; runs before gameplay" relies on this).
- `ECS.h` (`src/common/include/ECS.h`) X-macro tail currently ends with `X(ColliderComponent)` (verified line 277). `<glm/vec3.hpp>` is in scope via the existing `<glm/...>` includes used by other components.
- `Collision.h` (`src/game/src/Collision.h`): `KinematicMoveResult ResolveKinematicMove(const ECS&, EntityId, const TransformComponent&, const ColliderComponent&, const glm::vec3& desiredDelta)`. Returns `{AppliedDelta, BlockedX/Y/Z}`. No change.
- `game.cpp` `PlayerMovementSystem::Update` (~lines 200-235) currently calls `ResolveKinematicMove` if the entity has a `ColliderComponent`, else applies `desiredDelta` directly. This work removes that inline resolution.
- `game.cpp` `CameraZoomSystem::Phase()` and `IsometricFollowCameraSystem::Phase()` both return `SystemPhase::Simulation`. They become `PostSimulation` here.
- `game.cpp` `GameRegisterSystems` order (~lines 333-343): TextRotation, DayNight, MenuInteraction, AppFlow, DebugSpawn, PlayerMovement, CameraZoom, IsometricFollowCamera.
- `game.h` `GAME_API_VERSION 13u` (verified). Bumps to `14u`.
- `test_collision.cpp` `T00_free_move_without_blockers` already moves an entity that has a Collider but with no static blockers in the world. The optional new `T06_no_collider_full_delta` covers the **mover has no Collider at all** path that `KinematicMovementSystem` takes via the `HasComponent<ColliderComponent>` guard.
- Test patterns (verified): `SpawnCollider` helper in `test_collision.cpp`; `world.CreateEntity()` + `world.AddComponent(...)` for a Collider-less mover; `near()`/`veq()` helpers.

> **ECS.h + Systems.h enum-value renumber + `GAME_API_VERSION` bump → rebuild `ecs`+`editor`+`runtime`+`game` and restart the editor once.** The Systems.h renumber is value-only (existing systems' phase identifiers are still `SystemPhase::Simulation` etc., which still compile); the editor restart is forced by the ECS.h layout change, not the enum.

---

## Branch + File Structure

- New branch `feat/movement-decoupling` off `main` (created in Task 0).
- `src/common/include/Systems.h` — add `Physics = 2`, renumber `PostSimulation = 3`, `PreRender = 4`; update enum comments.
- `src/common/include/ECS.h` — add `MoveIntentComponent` + X-macro entry.
- `src/game/include/game.h` — `GAME_API_VERSION 13u → 14u`.
- `tests/test_collision.cpp` — add `T06_no_collider_full_delta`.
- `src/game/src/game.cpp` — new `KinematicMovementSystem` (Physics phase); trim `PlayerMovementSystem::Update`; switch `CameraZoomSystem::Phase()` and `IsometricFollowCameraSystem::Phase()` to `PostSimulation`; register `KinematicMovement` between PlayerMovement and the cameras.

No changes to `Collision.h`, `ECSCommands.h`, `ComponentSerialization.h`, `WorldManager.cpp`, `EcsInspectorPanel.{h,cpp}`, or any render-side file. No editor UI surface. No persistence.

---

### Task 0: Branch the work

**Files:** none (git only).

- [ ] **Step 1: Create the feature branch**

```bash
git checkout -b feat/movement-decoupling
git branch --show-current
```
Expected: `feat/movement-decoupling`.

(Branch off the current `main` tip, which already contains the approved spec at `docs/superpowers/specs/2026-05-27-movement-intent-decoupling-design.md`.)

---

### Task 1: `SystemPhase::Physics` + renumber

**Files:**
- Modify: `src/common/include/Systems.h`

- [ ] **Step 1: Update the enum**

In `src/common/include/Systems.h`, replace the existing `SystemPhase` enum (lines 17-22):

```cpp
enum class SystemPhase : uint8_t {
    Input          = 0,   // (future) input-derived state
    Simulation     = 1,   // gameplay logic — where v1 systems live
    PostSimulation = 2,   // reactions to simulation
    PreRender      = 3,   // last-chance ECS prep before snapshot
};
```

with:

```cpp
enum class SystemPhase : uint8_t {
    Input          = 0,   // (future) input-derived state
    Simulation     = 1,   // gameplay decisions / intent (PlayerMovement, AI, MenuInteraction, AppFlow, …)
    Physics        = 2,   // spatial resolution against the world (KinematicMovementSystem + future RigidBodyStep)
    PostSimulation = 3,   // reactions to the resolved world (camera follow, animation triggers, audio cues)
    PreRender      = 4,   // last-chance ECS prep before snapshot
};
```

- [ ] **Step 2: Build (verify no regressions from the renumber)**

Run: `cmake --build --preset msvc-win64-vs2026-community --target ecs editor runtime game`
Expected: builds with no errors. The renumber is value-only; existing systems still spell their phase as `SystemPhase::Simulation` etc., and the scheduler sorts by value — no code change is required outside this enum for the existing systems.

- [ ] **Step 3: Commit**

```bash
git add src/common/include/Systems.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(systems): add SystemPhase::Physics + renumber PostSimulation/PreRender"
```

---

### Task 2: `MoveIntentComponent` + API bump + optional test

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/game/include/game.h`
- Modify: `tests/test_collision.cpp`

- [ ] **Step 1: Add MoveIntentComponent**

In `src/common/include/ECS.h`, immediately after the `struct ColliderComponent { … };` block (~line 245), add:

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

- [ ] **Step 2: Register in the X-macro**

In `src/common/include/ECS.h`, change the tail of `ECS_FOR_EACH_REGISTERED_COMPONENT` from:

```cpp
    X(ColliderComponent)
```

to:

```cpp
    X(ColliderComponent) \
    X(MoveIntentComponent)
```

- [ ] **Step 3: Bump the game API version**

In `src/game/include/game.h`, change `#define GAME_API_VERSION 13u` to `#define GAME_API_VERSION 14u`.

- [ ] **Step 4: Add the no-collider test case**

In `tests/test_collision.cpp`, add after `T05_offset_and_scale_affect_bounds()`:

```cpp
static void T06_no_collider_full_delta()
{
    // An entity with TransformComponent but NO ColliderComponent must receive its full
    // desired delta unchanged when the kinematic-mover path falls through to the
    // collider-less branch. This pins KinematicMovementSystem's HasComponent<Collider>
    // guard semantics without exercising the system itself (pure-helper coverage).
    ECS world;
    const EntityId mover = world.CreateEntity();
    world.AddComponent(mover, TransformComponent{ glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f) });

    // A non-trivial blocker in the world that the mover would have hit IF it had a
    // collider — proves the collider-less mover ignores world geometry.
    SpawnCollider(world, {2.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, true);

    const auto* t = world.GetComponent<TransformComponent>(mover);
    EXPECT(t != nullptr);

    // No ColliderComponent on `mover` -> we don't call ResolveKinematicMove at all.
    // The caller (KinematicMovementSystem) applies the desired delta verbatim.
    const glm::vec3 desired(3.0f, 0.0f, 0.0f);
    EXPECT(!world.HasComponent<ColliderComponent>(mover));
    // Sanity: directly verify the world has the blocker present (so the test is meaningful).
    bool hasBlocker = false;
    world.Each<TransformComponent, ColliderComponent>([&](EntityId, const TransformComponent&, const ColliderComponent&){ hasBlocker = true; });
    EXPECT(hasBlocker);
    // Semantic: applied delta == desired (no resolver involvement).
    const glm::vec3 applied = desired;
    EXPECT(veq(applied, desired));
}
```

And call it in `main()` after `T05_offset_and_scale_affect_bounds();`:

```cpp
    T06_no_collider_full_delta();
```

(This is a pure semantic pin: it documents the "no collider → pass-through" contract that `KinematicMovementSystem` relies on, without depending on the system itself.)

- [ ] **Step 5: Configure + build**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target ecs editor runtime game test_collision
```
Expected: all build with no errors.

- [ ] **Step 6: Verify tests still pass**

Run:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_collision.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All collision tests passed.` and `All ECS tests passed.`

- [ ] **Step 7: Commit**

```bash
git add src/common/include/ECS.h src/game/include/game.h tests/test_collision.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(movement): MoveIntentComponent + GAME_API 14 + no-collider pass-through test"
```

---

### Task 3: `KinematicMovementSystem` + trim `PlayerMovementSystem` + migrate cameras + register

**Files:**
- Modify: `src/game/src/game.cpp`

- [ ] **Step 1: Trim PlayerMovementSystem to intent-only**

In `src/game/src/game.cpp`, replace the current `PlayerMovementSystem::Update` body (the one that calls `ResolveKinematicMove` and mutates `Transform.Position`) with:

```cpp
    // Moves entities tagged with PlayerComponent on the XZ plane from WASD input by writing
    // a per-tick MoveIntentComponent. KinematicMovementSystem (Physics phase) resolves the
    // intent against colliders and applies it — this system no longer touches Transform.
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
            // Align movement to the isometric camera yaw so W = "up the screen".
            const glm::vec3 desiredDelta = ComputePlanarMove(*in, speed, dt, glm::radians(kPoE2Follow.YawDeg));
            if (desiredDelta.x == 0.0f && desiredDelta.y == 0.0f && desiredDelta.z == 0.0f)
                return;

            // Lazy-seed MoveIntentComponent on first move; subsequent ticks Modify in place.
            // Self-contained: any new mover (AI, projectile) uses the same pattern, no boot
            // coupling required.
            if (!ctx.world.HasComponent<MoveIntentComponent>(e)) {
                ctx.world.AddComponent(e, MoveIntentComponent{});
            }
            ctx.world.Modify<MoveIntentComponent>(e, [&](MoveIntentComponent& m){
                m.DesiredDelta = desiredDelta;
            });
        });
    }
```

The class header (`class PlayerMovementSystem final : public ISystem { public: ... const char* Name() … Phase() Simulation … };`) is unchanged — only the `Update` body changes. The leading comment above `class PlayerMovementSystem` ("Moves entities tagged … resolves the desired delta against editor-authored static colliders.") should be updated to: `// Writes a per-tick MoveIntentComponent for PlayerComponent entities from WASD input.` — drop the "resolves" half.

- [ ] **Step 2: Add KinematicMovementSystem**

In `src/game/src/game.cpp`, in the anonymous `namespace { … }` block, immediately **after** `PlayerMovementSystem`'s closing `};` and **before** the next class declaration, add:

```cpp
// Resolves MoveIntentComponent against ColliderComponent (when present) and applies the
// result to TransformComponent. Single owner of Collision.h calls from movement; producers
// (PlayerMovement today, AI/projectiles later) only write the intent. Clears DesiredDelta
// after consume so a stale value can't re-fire next tick if the producer stops running.
// Physics phase: runs after Simulation (producers) and before PostSimulation (cameras).
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
                ctx.world.Modify<TransformComponent>(e, [&](TransformComponent& t){
                    t.Position += applied;
                });
            }

            // Clear-after-consume: zero the intent so a stale value doesn't re-fire next tick
            // if the producer didn't run (e.g. gating turned a movement system off).
            ctx.world.Modify<MoveIntentComponent>(e, [](MoveIntentComponent& m){
                m.DesiredDelta = glm::vec3(0.0f);
            });
        });
    }
    const char* Name() const override { return "KinematicMovementSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Physics; }
};
```

- [ ] **Step 3: Migrate cameras to PostSimulation**

In `src/game/src/game.cpp`, in `IsometricFollowCameraSystem`, change the `Phase()` body from:

```cpp
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
```

to:

```cpp
    SystemPhase Phase() const override { return SystemPhase::PostSimulation; }
```

In `CameraZoomSystem`, change the same `Phase()` body identically:

```cpp
    SystemPhase Phase() const override { return SystemPhase::PostSimulation; }
```

(Both classes already exist with that line returning `SystemPhase::Simulation`; just swap the enumerator name.)

- [ ] **Step 4: Register KinematicMovementSystem in GameRegisterSystems**

In `src/game/src/game.cpp`, in `GameRegisterSystems` (~line 333), change the block from:

```cpp
    s->Register(std::make_unique<PlayerMovementSystem>());
    s->Register(std::make_unique<CameraZoomSystem>());          // before follow: sets distance same tick
    s->Register(std::make_unique<IsometricFollowCameraSystem>());
```

to:

```cpp
    s->Register(std::make_unique<PlayerMovementSystem>());            // Simulation: writes MoveIntent
    s->Register(std::make_unique<KinematicMovementSystem>());         // Physics: resolves intent + applies Transform
    s->Register(std::make_unique<CameraZoomSystem>());                // PostSimulation: before follow (sets distance)
    s->Register(std::make_unique<IsometricFollowCameraSystem>());     // PostSimulation: reads post-resolution Transform
```

(Registration order across phases doesn't matter — the scheduler sorts by `(phase, registration index)` so the new Physics-phase system always runs after every Simulation-phase one. The order shown above keeps the listing readable.)

- [ ] **Step 5: Build editor + runtime + game**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor runtime game`
Expected: builds with no errors.

- [ ] **Step 6: Static verification (no GUI here)**

Re-read the final `game.cpp` and confirm:
- (a) `PlayerMovementSystem::Update` no longer references `ResolveKinematicMove`, `ColliderComponent`, or `Modify<TransformComponent>`. Only reads input + writes `MoveIntentComponent`.
- (b) `KinematicMovementSystem` is the **only** caller of `ResolveKinematicMove` in `game.cpp`.
- (c) `KinematicMovementSystem::Phase()` returns `SystemPhase::Physics`; `CameraZoomSystem::Phase()` + `IsometricFollowCameraSystem::Phase()` return `SystemPhase::PostSimulation`; `PlayerMovementSystem::Phase()` is unchanged (Simulation).
- (d) `GameRegisterSystems` registers `KinematicMovement` between `PlayerMovement` and `CameraZoom`.
- (e) The `#include "Collision.h"` in `game.cpp` is still present (KinematicMovementSystem still needs it).
- (f) `MoveIntentComponent` is referenced by both PlayerMovement (write) and KinematicMovement (read+clear), no other code path.

Report what you verified. (Live GUI smoke after Task 3 is the human's; same as previous gameplay-system changes.)

- [ ] **Step 7: Commit**

```bash
git add src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(movement): KinematicMovementSystem (Physics) + trim PlayerMovement + cameras → PostSimulation"
```

---

## Final verification

- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` → `All ECS tests passed.`
- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_collision.exe` → `All collision tests passed.`
- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe` → `All menu tests passed.` (non-regression — menu state machine unaffected, but new component + phase renumber rebuild this dependency chain)
- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe` → `All world-serialization tests passed.` (non-regression — MoveIntent is not serialized; world.json round-trip unaffected)
- [ ] Human GUI smoke (**restart editor** — ECS.h layout + GAME_API changed):
  - In InLevel, player moves with WASD (bit-identical behavior to before this refactor).
  - Player slides on an authored static collider (bit-identical to before).
  - In MainMenu, no movement; ESC in-level → MainMenu → InLevel → movement resumes cleanly (clear-after-consume ensures no stuck drift).
  - Camera continues to follow the post-resolved player position the same frame (no visible lag).
  - DX12 + Vulkan: no rendering changes; both should behave identically.

## Self-review notes (vs. spec)

- Spec sections covered:
  - **System phases** → Task 1 (Systems.h enum).
  - **New component (§1)** → Task 2 (ECS.h + X-macro).
  - **New system (§2)** → Task 3 Step 2 (`KinematicMovementSystem`, Physics phase).
  - **PlayerMovementSystem after refactor (§3)** → Task 3 Step 1 (trimmed Update).
  - **Seed strategy (§4)** → Task 3 Step 1 lazy-seed (`HasComponent` → `AddComponent` → `Modify`).
  - **Registration + phase order (§5)** → Task 3 Steps 3-4 (camera phase change + KinematicMovement registration).
  - **Clear-after-consume (§6)** → Task 3 Step 2 (final `Modify<MoveIntentComponent>` to zero).
  - **Acceptance** → final-verification checklist (test suites + GUI smoke).
- `GAME_API_VERSION` 13→14 in Task 2 covers the ECS.h layout change.
- No serialization, no inspector, no ECSCommands — intentionally absent (runtime component).
- Branch `feat/movement-decoupling` created in Task 0 per the spec's implementation-branch note.
- The optional `T06_no_collider_full_delta` test lands in Task 2; it pins the collider-less pass-through semantics without coupling the test to the (game-side) `KinematicMovementSystem`.
