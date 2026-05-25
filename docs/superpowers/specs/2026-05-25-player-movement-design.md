# Player Component + Movement System — Design

**Date:** 2026-05-25
**Status:** Approved (design)

## Problem

The game has no notion of a controllable player. WASD currently flies the free-look
camera (`FreeLookCameraSystem` → `WorldCameraComponent`). We want a `PlayerComponent`
that marks an entity as the player and a `PlayerMovementSystem` that moves it from
keyboard input.

## Goal (v1)

A new ECS `PlayerComponent` (with a tunable move speed) and a `PlayerMovementSystem`
that moves any entity carrying `PlayerComponent` + `TransformComponent` on the XZ ground
plane via WASD. The player is tagged onto an existing scene entity through the editor
inspector (no mesh-spawn plumbing needed — the engine has no built-in primitive mesh and
meshes are editor/scene-driven). Movement math is a pure, unit-tested helper.

## Non-Goals (deferred)

- **Camera-follow** and **camera-relative movement** — v1 leaves the camera as-is; you
  watch the player via the editor fly-cam (or runtime free-look).
- **Jump / gravity / 3D vertical movement** — v1 is planar (Y fixed).
- **Scene persistence of `PlayerComponent`** in `world.json` — the tag is applied per
  session via the inspector; `WorldManager` serialization is not extended in v1.
- **Default-scene player spawn** — no auto-spawned player entity.

## Architecture

### 1. `PlayerComponent` (`src/common/include/ECS.h`)

```cpp
struct PlayerComponent {
    float MoveSpeed = 5.0f; // world units / second
};
```

- Add `X(PlayerComponent)` to `ECS_FOR_EACH_REGISTERED_COMPONENT`.
- Register in `src/common/include/ECSCommands.h`:
  - `ApplyComponentCommand` — add/modify dispatch (`AddComponent(entity, *player)`).
  - `RemoveComponentByType` — `RemoveComponent<PlayerComponent>(entity)`.
  Forgetting either branch is a silent no-op.

### 2. Pure movement helper (game-side header)

A header so it's testable without the full system. Depends only on `InputStateComponent`
(declared in `ECS.h`, which pulls `Input.h` for the `KEY_*` codes) and GLM:

```cpp
// src/game/src/PlayerMovement.h
#pragma once
#include <glm/glm.hpp>
#include "ECS.h"   // InputStateComponent + KEY_* (via Input.h)

// World-axis planar move from WASD. Forward = -Z, back = +Z, left = -X, right = +X.
// Diagonal is normalized so it isn't faster than a cardinal move. Y is never touched.
inline glm::vec3 ComputePlanarMove(const InputStateComponent& in, float speed, float dt) {
    glm::vec3 dir(0.0f);
    if (in.KeysDown[KEY_W]) dir.z -= 1.0f;
    if (in.KeysDown[KEY_S]) dir.z += 1.0f;
    if (in.KeysDown[KEY_A]) dir.x -= 1.0f;
    if (in.KeysDown[KEY_D]) dir.x += 1.0f;
    if (dir.x != 0.0f || dir.z != 0.0f) dir = glm::normalize(dir);
    return dir * (speed * dt);
}
```

### 3. `PlayerMovementSystem` (`src/game/src/game.cpp`)

A new `ISystem` (Simulation phase), registered in `GameRegisterSystems`:

```cpp
class PlayerMovementSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (!in) return;
        const float dt = static_cast<float>(ctx.dt);
        ctx.world.Each<PlayerComponent, TransformComponent>([&](EntityId e) {
            float speed = 5.0f;
            if (const auto* p = ctx.world.GetComponent<PlayerComponent>(e)) speed = p->MoveSpeed;
            const glm::vec3 delta = ComputePlanarMove(*in, speed, dt);
            if (delta.x != 0.0f || delta.z != 0.0f)
                ctx.world.Modify<TransformComponent>(e, [&](TransformComponent& t){ t.Position += delta; });
        });
    }
    const char* Name() const override { return "PlayerMovementSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};
```

Registered alongside the others in `GameRegisterSystems` (`s->Register(std::make_unique<PlayerMovementSystem>());`). `game.cpp` includes `"PlayerMovement.h"`.

(Uses the same per-entity `Each` / `Modify` and singleton-input pattern as the existing
systems; iterates all `PlayerComponent` holders so multiple players "just work," though
v1 expects one.)

### 4. Editor inspector (`src/editor/src/rendering/imgui/EcsInspectorPanel.cpp`)

Follow the established per-component pattern (mirrors `LightningComponent`/`MeshComponent`):
- **Add-component menu:** when the selected entity lacks `PlayerComponent`, a "Player"
  `Selectable` that pushes `ECSCommand::AddComponent(entity, PlayerComponent{})`.
- **Remove menu:** when present, a "Remove Player" `Selectable` pushing
  `ECSCommand::RemoveComponent<PlayerComponent>(entity)`.
- **Editor:** when present, a `MoveSpeed` `SliderFloat` (e.g. `0.5 .. 50`, "%.1f") that
  pushes `ECSCommand::ModifyComponent(selectedEntity, editPlayer)` on change.

All edits flow through the ECS command ring (RenderThread → GameThread), like every
other inspector edit.

## Data flow

PlatformThread fills `InputStateComponent` → GameThread runs `PlayerMovementSystem`
(reads input singleton, mutates the player's `TransformComponent`) → snapshot →
RenderThread draws the player's mesh at its new position. Editor inspector edits
(add/remove/MoveSpeed) → command ring → GameThread applies them.

## Known overlap (accepted for v1)

`FreeLookCameraSystem` also reads WASD. In the **editor**, plain WASD moves only the
player (the editor fly-cam requires RMB held and overrides the game camera), so smoke
testing is clean. In `runtime`, WASD moves both the camera and the player until
camera-follow is added (deferred). No gating is added in v1.

## Testing

New `tests/test_playermove.cpp` target (plain `main()`, prints `All … passed.`),
exercising the pure `ComputePlanarMove`:
1. `KEY_W` only → `(0, 0, -speed*dt)`; `KEY_A` only → `(-speed*dt, 0, 0)`; `KEY_D` →
   `(+speed*dt,0,0)`; `KEY_S` → `(0,0,+speed*dt)`.
2. `KEY_W` + `KEY_D` (diagonal) → both x and z non-zero with `length == speed*dt`
   (normalized, not √2 faster).
3. `KEY_W` + `KEY_S` (opposing) → `(0,0,0)`; no keys → `(0,0,0)`.
4. Y component is always 0.
5. Speed/dt scaling: doubling `dt` (or `speed`) doubles the delta length.

The helper needs only `ECS.h` (for `InputStateComponent` + `KEY_*`) and GLM, so the test
links lightly (`CommonHeaders` + `glm::glm`) with no game/engine link — mirroring the
existing pure-logic tests.

## Build impact

`ECS.h` changes + a new registered component type → rebuild `ecs`, `editor`, and `game`;
**bump `GAME_API_VERSION` 6 → 7**; restart the editor (the running `editor.exe` has the
old layout linked in). Add `PlayerMovement.h` (header-only — no CMake source entry) and
the `test_playermove` target to `tests/CMakeLists.txt`.

## Risks / Notes

- **Silent ECSCommands gap:** both processor branches must be updated or inspector
  add/remove silently no-ops.
- **`PlayerMovement.h` location:** game-internal header at `src/game/src/`; the test adds
  that dir + `src/common/include` to its include path.
- **Multiple players:** the system moves every `PlayerComponent` holder identically; v1
  expects exactly one tagged entity. Not a defect — just noted.
