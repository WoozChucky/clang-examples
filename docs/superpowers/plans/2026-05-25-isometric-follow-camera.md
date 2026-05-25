# Isometric Follow Camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the free-look game camera with a fixed-angle isometric (PoE2-style) camera that follows the player, with no camera controls.

**Architecture:** A pure `ComputeFollowCamera` helper (game header, unit-tested) builds a `CameraView` from the player position + hardcoded angle/distance/FOV constants. A new `IsometricFollowCameraSystem` (game `ISystem`, Simulation, run after `PlayerMovementSystem`) writes `WorldCameraComponent` from it. `FreeLookCameraSystem` and the now-unused `FreeLookControlComponent` are removed.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), game `ISystem`/`SystemScheduler` (hot-reloaded `Game.dll`), GLM (`lookAtRH`/`perspectiveRH_ZO`). Pure helper unit-tested via a new `test_followcam`.

**Spec:** `docs/superpowers/specs/2026-05-25-isometric-follow-camera-design.md`

---

## Build & test reference (every task)

Build preset: **`msvc-win64-vs2026-community`** (enterprise preset is NOT installed — never use it). Binaries in `out/build/msvc-win64-vs2026-community/bin/Debug/`.

```powershell
cmake --preset msvc-win64-vs2026-community                                   # reconfigure (after CMakeLists changes)
cmake --build --preset msvc-win64-vs2026-community --target <target>          # build
./out/build/msvc-win64-vs2026-community/bin/Debug/<test>.exe                   # run a test
```

**Commit identity (MANDATORY):** `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "<msg>"`. Never the work email. Never `git add .claude/`.

**Restart caveat:** Task 2 removes `FreeLookControlComponent` from `ECS.h` and bumps `GAME_API_VERSION`. After the rebuild the user must **restart `editor.exe`** (the running one has the old layout linked in). A subagent cannot restart it — build everything, report that a restart + GUI smoke is required.

## File map

- `src/game/src/CameraFollow.h` — **new.** Pure `FollowCameraParams` + `ComputeFollowCamera` + `kPoE2Follow`. (Task 1)
- `tests/test_followcam.cpp` + `tests/CMakeLists.txt` — **new** test target. (Task 1)
- `src/game/src/game.cpp` — delete `FreeLookCameraSystem`, add `IsometricFollowCameraSystem`, swap registration, drop the free-look seed, include the helper. (Task 2)
- `src/common/include/ECS.h` — remove `FreeLookControlComponent` struct + X-macro entry. (Task 2)
- `src/game/include/game.h` — `GAME_API_VERSION` 7 → 8. (Task 2)

---

## Task 1: Pure `ComputeFollowCamera` helper + test (TDD)

**Files:**
- Create: `src/game/src/CameraFollow.h`
- Create: `tests/test_followcam.cpp`
- Modify: `tests/CMakeLists.txt`

Pure (GLM + `CameraView.h`), unit-testable with no ecs/engine link (mirrors `test_playermove`). Additive — nothing else changes yet, so the editor/game still build.

- [ ] **Step 1: Write the failing test `tests/test_followcam.cpp`**

```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "CameraFollow.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

static void T00_eye_above_and_distance()
{
    const glm::vec3 targetPos(0.0f);
    const CameraView v = ComputeFollowCamera(targetPos, kPoE2Follow, 16.0f / 9.0f);
    const glm::vec3 lookTarget = targetPos + glm::vec3(0.0f, kPoE2Follow.TargetHeight, 0.0f);
    EXPECT(v.Position.y > lookTarget.y);                                        // eye above target
    EXPECT(near(glm::length(v.Position - lookTarget), kPoE2Follow.Distance));   // exact distance
}

static void T01_lookat_centers_target()
{
    const glm::vec3 targetPos(3.0f, 0.0f, -2.0f);
    const CameraView v = ComputeFollowCamera(targetPos, kPoE2Follow, 1.6f);
    const glm::vec3 lookTarget = targetPos + glm::vec3(0.0f, kPoE2Follow.TargetHeight, 0.0f);
    const glm::vec4 vs = v.View * glm::vec4(lookTarget, 1.0f); // target in view space
    EXPECT(near(vs.x, 0.0f) && near(vs.y, 0.0f));             // centered
    EXPECT(near(vs.z, -kPoE2Follow.Distance));               // in front, at Distance (RH: -Z forward)
}

static void T02_yaw_orientation()
{
    FollowCameraParams p0 = kPoE2Follow; p0.YawDeg = 0.0f;
    const CameraView v0 = ComputeFollowCamera(glm::vec3(0.0f), p0, 1.6f);
    EXPECT(v0.Position.z > 0.0f);          // yaw 0 -> eye on +Z side
    EXPECT(near(v0.Position.x, 0.0f));

    FollowCameraParams p90 = kPoE2Follow; p90.YawDeg = 90.0f;
    const CameraView v90 = ComputeFollowCamera(glm::vec3(0.0f), p90, 1.6f);
    EXPECT(v90.Position.x > 0.0f);         // yaw 90 -> eye on +X side
    EXPECT(near(v90.Position.z, 0.0f));
}

static void T03_aspect_changes_projection()
{
    const CameraView a = ComputeFollowCamera(glm::vec3(0.0f), kPoE2Follow, 1.0f);
    const CameraView b = ComputeFollowCamera(glm::vec3(0.0f), kPoE2Follow, 2.0f);
    EXPECT(!near(a.Projection[0][0], b.Projection[0][0])); // horizontal scale tracks aspect
}

int main()
{
    T00_eye_above_and_distance();
    T01_lookat_centers_target();
    T02_yaw_orientation();
    T03_aspect_changes_projection();
    if (g_Failures == 0) { std::printf("All follow camera tests passed.\n"); return 0; }
    std::printf("%d follow camera test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the `test_followcam` target to `tests/CMakeLists.txt`**

Append at the end:

```cmake
add_executable(test_followcam
    test_followcam.cpp
)

target_link_libraries(test_followcam PRIVATE
    glm::glm
)

target_include_directories(test_followcam PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/game/src
)

target_compile_definitions(test_followcam PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_followcam PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Configure + build to verify FAIL (header missing)**

```powershell
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_followcam
```
Expected: FAIL — `Cannot open include file: 'CameraFollow.h'`.

- [ ] **Step 4: Create `src/game/src/CameraFollow.h`**

```cpp
#pragma once

#include <cmath> // std::cos, std::sin

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp> // glm::lookAtRH, glm::perspectiveRH_ZO

#include "CameraView.h" // CameraView (View/Projection/Position)

// Fixed-angle isometric follow-camera parameters (no runtime controls).
struct FollowCameraParams {
    float Distance;     // eye distance from the look-at target
    float ElevationDeg; // angle above the horizon (90 = straight down)
    float YawDeg;       // rotation around the target's Y axis
    float FovDeg;       // vertical field of view
    float TargetHeight; // look-at point raised above the player's origin
    float Near;
    float Far;
};

// PoE2-ish starting values. Dial via game-DLL hot-reload.
inline constexpr FollowCameraParams kPoE2Follow{
    /*Distance*/ 22.0f, /*ElevationDeg*/ 55.0f, /*YawDeg*/ 45.0f,
    /*FovDeg*/ 40.0f, /*TargetHeight*/ 1.0f, /*Near*/ 0.1f, /*Far*/ 1000.0f
};

// Build a fixed-angle follow camera looking at `targetPos` (+ TargetHeight on Y).
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

- [ ] **Step 5: Build + run the test → PASS**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_followcam
./out/build/msvc-win64-vs2026-community/bin/Debug/test_followcam.exe
```
Expected: `All follow camera tests passed.`

- [ ] **Step 6: Commit**

```powershell
git add src/game/src/CameraFollow.h tests/test_followcam.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(game): ComputeFollowCamera helper (isometric follow) + tests"
```

---

## Task 2: Swap free-look → IsometricFollowCameraSystem; remove FreeLookControlComponent

**Files:**
- Modify: `src/game/src/game.cpp` (delete `FreeLookCameraSystem` class lines 91-144; swap registration; drop the free-look seed line 236 + fix comment; add include)
- Modify: `src/common/include/ECS.h` (remove `FreeLookControlComponent` struct lines 126-134; remove `X(FreeLookControlComponent)` line 198)
- Modify: `src/game/include/game.h` (`GAME_API_VERSION` 7 → 8)

Atomic cutover: the follow system replaces the free-look system as the sole `WorldCameraComponent` writer, and the now-unused component is removed in the same commit.

- [ ] **Step 1: Add the helper include to `game.cpp`**

After `#include "PlayerMovement.h"` (near the top), add:

```cpp
#include "CameraFollow.h"
```

- [ ] **Step 2: Replace the `FreeLookCameraSystem` class with `IsometricFollowCameraSystem`**

In `src/game/src/game.cpp`, delete the entire `FreeLookCameraSystem` block — the comment + class spanning from `// Free-look fly camera. ...` (line 91) through its closing `};` (line 144) — and replace it with:

```cpp
// Fixed-angle isometric camera (PoE2-style) that follows the player. No controls:
// it places WorldCameraComponent at a constant offset from the first PlayerComponent
// entity and looks at it. No-op (camera holds) when no player exists.
class IsometricFollowCameraSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        bool found = false;
        glm::vec3 target(0.0f);
        // EntityId-only callback (no live component refs) so ModifySingleton below is safe.
        ctx.world.Each<PlayerComponent, TransformComponent>([&](EntityId e) {
            if (found) return;
            if (const auto* t = ctx.world.GetComponent<TransformComponent>(e)) {
                target = t->Position;
                found = true;
            }
        });
        if (!found) return; // no player -> leave the camera where it is

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

- [ ] **Step 3: Swap the registration in `GameRegisterSystems`**

In `GameRegisterSystems` (currently registers FreeLook, Text, DayNight, DebugSpawn, PlayerMovement, Quit):
- Delete the line `    s->Register(std::make_unique<FreeLookCameraSystem>());`
- Add, immediately AFTER `    s->Register(std::make_unique<PlayerMovementSystem>());`:
  ```cpp
      s->Register(std::make_unique<IsometricFollowCameraSystem>());
  ```

Result (order matters — the camera runs after the player moves, so it follows the post-move position this tick):
```cpp
void GameRegisterSystems(SystemScheduler* s) {
    if (!s) return;
    s->Register(std::make_unique<TextRotationSystem>());
    s->Register(std::make_unique<DayNightSystem>());
    s->Register(std::make_unique<DebugSpawnSystem>());
    s->Register(std::make_unique<PlayerMovementSystem>());
    s->Register(std::make_unique<IsometricFollowCameraSystem>());
    s->Register(std::make_unique<QuitRequestSystem>(KEY_ESCAPE));
}
```

- [ ] **Step 4: Remove the free-look seed in the init block**

In the `GameStateId::Uninitialized` seed block, delete the line:
```cpp
	        g_GameState->World.SetSingleton(FreeLookControlComponent{});
```
and update the preceding comment so it no longer references `FreeLookControlComponent` — change the comment block (currently mentioning "camera control ... FreeLookControlComponent defaults include Position {0,5,10}") to:
```cpp
	        // Seed game-owned singletons (resolved camera + app control + day/night state).
```
Keep the other seeds (`WorldCameraComponent`, `AppControlComponent`, `AtmosphereStateComponent`, and the atmosphere singletons) unchanged.

- [ ] **Step 5: Remove `FreeLookControlComponent` from `ECS.h`**

In `src/common/include/ECS.h`:
- Delete the `struct FreeLookControlComponent { ... };` definition (lines ~126-134).
- Delete the `    X(FreeLookControlComponent) \` line from `ECS_FOR_EACH_REGISTERED_COMPONENT` (the lines above/below keep their trailing backslashes, so the macro stays intact).

(It is not referenced anywhere else — not in the editor, not in `ECSCommands.h`. After Step 2 deleted `FreeLookCameraSystem` and Step 4 deleted the seed, no references remain.)

- [ ] **Step 6: Bump `GAME_API_VERSION` in `game.h`**

In `src/game/include/game.h`, change `#define GAME_API_VERSION 7u` to:
```cpp
#define GAME_API_VERSION 8u
```

- [ ] **Step 7: Full build + grep + regression tests**

```powershell
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target editor
cmake --build --preset msvc-win64-vs2026-community --target game
cmake --build --preset msvc-win64-vs2026-community --target test_followcam
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_followcam.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `editor` + `game` build clean; both tests print their `All … passed.` lines. Then grep `src/` for `FreeLookControlComponent` and `FreeLookCameraSystem` — there must be ZERO matches (if any remain, remove them).

- [ ] **Step 8: Commit**

```powershell
git add src/game/src/game.cpp src/common/include/ECS.h src/game/include/game.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(game): isometric follow camera replaces free-look; remove FreeLookControlComponent"
```

> After this, the user restarts `editor.exe` (ECS.h layout + `GAME_API_VERSION` 8) and GUI-smokes.

---

## Done criteria

- `test_followcam` + `test_ecs` print their `All … passed.` lines; `ecs`/`editor`/`game` build clean.
- Zero `FreeLookControlComponent` / `FreeLookCameraSystem` references remain in `src/`.
- User GUI smoke (after editor restart): tag a player (inspector), switch to game view (editor camera off) → the camera sits at a fixed steep isometric angle and tracks the player as WASD moves it; in `runtime` it is the view. Tweaking `kPoE2Follow` + rebuilding `game` hot-reloads the new angle.
