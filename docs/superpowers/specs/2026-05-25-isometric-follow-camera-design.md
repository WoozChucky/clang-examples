# Isometric Follow Camera (PoE2-style) — Design

**Date:** 2026-05-25
**Status:** Approved (design)

## Problem

The game camera is a free-look fly camera (`FreeLookCameraSystem` → `WorldCameraComponent`).
With a player entity now in place (`PlayerComponent` + `PlayerMovementSystem`), the game
should use a fixed-angle isometric/top-down camera (Path of Exile 2 style) that simply
follows the player, with no camera controls.

## Goal

A `IsometricFollowCameraSystem` that, each tick, places `WorldCameraComponent` at a fixed
offset relative to the player and looks at the player — a steep, fixed-angle perspective
view. No keyboard/mouse camera controls. It **replaces** the free-look game camera; the
free-look system and its control component are removed.

## Non-Goals (deferred)

- **Smoothing / follow lag** — v1 snaps the camera to the player each tick (instant follow).
- **Runtime tuning panel / config component** — angle/distance/FOV are hardcoded named
  constants, dialed via game-DLL hot-reload (rebuild `game` → reloads in ~1s).
- **Orthographic projection** — v1 uses perspective (PoE2 uses perspective with a tight FOV).
- **Edge-pan, zoom, screen-shake, rotation.**

## Architecture

### 1. Pure helper — `src/game/src/CameraFollow.h`

No ECS coupling (depends only on GLM + `CameraView.h`), so it is unit-testable:

```cpp
#pragma once
#include <cmath>                        // std::cos, std::sin
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp> // lookAtRH, perspectiveRH_ZO
#include "CameraView.h"                 // CameraView (View/Projection/Position)

struct FollowCameraParams {
    float Distance;     // eye distance from the look-at target
    float ElevationDeg; // angle above the horizon (90 = straight down)
    float YawDeg;       // rotation around the target's Y axis
    float FovDeg;       // vertical field of view
    float TargetHeight; // look-at point raised above the player's origin
    float Near;
    float Far;
};

// PoE2-ish starting values. Dial via hot-reload.
inline constexpr FollowCameraParams kPoE2Follow{
    /*Distance*/ 22.0f, /*ElevationDeg*/ 55.0f, /*YawDeg*/ 45.0f,
    /*FovDeg*/ 40.0f, /*TargetHeight*/ 1.0f, /*Near*/ 0.1f, /*Far*/ 1000.0f
};

// Build a fixed-angle follow camera looking at `targetPos`.
inline CameraView ComputeFollowCamera(const glm::vec3& targetPos,
                                      const FollowCameraParams& p,
                                      float aspect) {
    const glm::vec3 target = targetPos + glm::vec3(0.0f, p.TargetHeight, 0.0f);
    const float E = glm::radians(p.ElevationDeg);
    const float A = glm::radians(p.YawDeg);
    // Unit direction from the target up to the eye (|offsetDir| == 1).
    const glm::vec3 offsetDir(std::cos(E) * std::sin(A), std::sin(E), std::cos(E) * std::cos(A));
    const glm::vec3 eye = target + p.Distance * offsetDir;

    CameraView v;
    v.View       = glm::lookAtRH(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
    v.Projection = glm::perspectiveRH_ZO(glm::radians(p.FovDeg), aspect, p.Near, p.Far);
    v.Position   = eye;
    return v;
}
```

(`offsetDir` is a unit vector — its length is `sqrt(cos²E·(sin²A+cos²A) + sin²E) = 1` — so
`|eye − target| == Distance` exactly. `ElevationDeg < 90` keeps a non-degenerate up vector
for `lookAtRH`.)

### 2. `IsometricFollowCameraSystem` — `src/game/src/game.cpp`

A new `ISystem` (Simulation phase), registered in `GameRegisterSystems` in place of
`FreeLookCameraSystem`:

```cpp
class IsometricFollowCameraSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        // First player with a transform; no-op (camera holds) if none exists.
        bool found = false;
        glm::vec3 target(0.0f);
        ctx.world.Each<PlayerComponent, TransformComponent>([&](EntityId e) {
            if (found) return;
            if (const auto* t = ctx.world.GetComponent<TransformComponent>(e)) {
                target = t->Position;
                found = true;
            }
        });
        if (!found) return;

        const auto* vp = ctx.world.GetSingleton<ViewportComponent>();
        const float aspect = (vp && vp->Height) ? float(vp->Width) / float(vp->Height) : 16.0f / 9.0f;

        const CameraView cam = ComputeFollowCamera(target, kPoE2Follow, aspect);
        ctx.world.ModifySingleton<WorldCameraComponent>([&](WorldCameraComponent& w) {
            w.View       = cam.View;
            w.Projection = cam.Projection;
            w.Position   = cam.Position;
        });
    }
    const char* Name() const override { return "IsometricFollowCameraSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};
```

(`EntityId`-only `Each` + `ModifySingleton<WorldCameraComponent>` is safe — `WorldCameraComponent`
is not one of the queried types, and no live query refs are held.)

### 3. Remove the free-look camera

- Delete the `FreeLookCameraSystem` class from `game.cpp` and its `s->Register(...)` line in
  `GameRegisterSystems`; register `IsometricFollowCameraSystem` instead.
- Delete the `SetSingleton(FreeLookControlComponent{})` seed in the init block (+ its comment).
- Remove `struct FreeLookControlComponent` from `ECS.h` and its `X(FreeLookControlComponent)`
  entry from `ECS_FOR_EACH_REGISTERED_COMPONENT`. (It is not referenced anywhere else — not in
  the editor, not in `ECSCommands` — confirmed by grep.)
- `#include "CameraFollow.h"` in `game.cpp`.

`WorldCameraComponent` and its seed stay (still the camera output the renderer consumes).

## Data flow

`PlayerMovementSystem` moves the player's `TransformComponent` → `IsometricFollowCameraSystem`
(later in the Simulation phase) reads the player position + `ViewportComponent`, writes
`WorldCameraComponent` → snapshot → RenderThread uses `WorldCameraComponent` as the view when
the editor camera override is inactive (and always in `runtime`).

Register `IsometricFollowCameraSystem` **after** `PlayerMovementSystem` so the camera follows
the player's post-move position within the same tick (no 1-frame lag).

## Testing

New `tests/test_followcam.cpp` (plain `main()`, prints `All … passed.`) on the pure helper:
1. **Eye placement:** for a target at origin with `kPoE2Follow`, `eye.y > target.y` (above) and
   `length(eye − (target + (0,TargetHeight,0))) == Distance` (within epsilon).
2. **Look-at centers the target:** `View * vec4(targetWorld, 1)` ≈ `(0, 0, −Distance)` (target on
   the view forward axis, in front of the camera).
3. **Yaw orientation:** with `YawDeg = 0`, the eye sits on the +Z / +Y side of the target
   (`eye.z > target.z`, `eye.x ≈ target.x`); a 90° yaw moves it to the +X side.
4. **Projection responds to aspect:** two different aspect ratios produce different `Projection`
   matrices (e.g. `[0][0]` differs).

The helper needs only GLM + `CameraView.h`, so the test links `glm::glm` with the
`src/common/include` + `src/game/src` include dirs (no ecs/engine link) — mirroring `test_playermove`.

## Build impact

Removing `FreeLookControlComponent` changes `ECS.h` (struct + X-macro), so: rebuild `ecs`,
`editor`, and `game`; **bump `GAME_API_VERSION` 7 → 8**; restart the editor (the running one
has the old layout linked in). Add `CameraFollow.h` (header-only) and the `test_followcam`
target to `tests/CMakeLists.txt`.

## Risks / Notes

- **No player → no-op:** if no entity has `PlayerComponent` + `TransformComponent`, the camera
  holds its last/seeded value. Tag a player (inspector) to see the follow camera.
- **Editor smoke needs game view:** in the editor the EditorCamera override usually wins; switch
  to game view (editor camera inactive) to observe the follow camera. In `runtime` it is always
  the view.
- **Dialing the feel:** the constants are the only tuning surface; change `kPoE2Follow` + rebuild
  `game` (hot-reload) to iterate — except this branch's `ECS.h` removal needs the one-time editor
  restart first; subsequent constant tweaks are pure game-DLL hot-reloads.
- **System order:** `IsometricFollowCameraSystem` must run after `PlayerMovementSystem` (both
  Simulation) — ensured by registration order.
