# State-Gated Menu — Phase 1 (State Machine Foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `GameStateId` the single source of truth in the ECS so gameplay systems run only in `InLevel`, with an `AppFlowSystem` owning transitions and a per-tick `ActionQueue` — no UI/menu yet.

**Architecture:** `GameStateComponent` singleton (authoritative, in the ECS snapshot) is mirrored onto the host `GameState.StateId` each tick; the `GameUpdate` switch collapses to one-time boot seeding. The three gameplay systems early-out unless `Current == InLevel`. `AppFlowSystem` drains an `ActionQueueComponent` (no producers until Phase 4) and, temporarily, toggles state on TAB so gating is testable now.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), hot-reloaded `Game.dll`, CMake (preset `msvc-win64-vs2026-community`).

**Spec:** `docs/superpowers/specs/2026-05-25-state-gated-menu-design.md` (this is Phase 1 of §15).

---

## Reference facts (verified against the codebase)

- Singleton API (`ECS.h`): `world.SetSingleton<T>(T)`, `world.GetSingleton<T>() -> const T*`, `world.ModifySingleton<T>([](T&){...})`.
- `using EntityId = uint64_t;` (`ECS.h:44`). `ECS.h` already includes `<cstdint>`, `<vector>`, `"input.h"`.
- X-macro `ECS_FOR_EACH_REGISTERED_COMPONENT` ends with `X(PlayerComponent) \` + `X(CameraZoomComponent)` (`ECS.h:177-195`).
- `GameStateId` is currently defined in `src/game/include/game.h:22-28`; `GAME_API_VERSION` is `8u` at `game.h:20`. `game.cpp` includes `"Game.h"` (resolves to that file).
- Key codes (`input.h`): `KEY_TAB = 258`, `KEY_ESCAPE = 256` (both already used in `game.cpp`).
- The three gameplay systems live in `game.cpp`'s anonymous namespace: `PlayerMovementSystem` (`Update` opens `const auto* in = ...; if (!in) return; const float dt = ...`), `CameraZoomSystem` (`const auto* in = ...; if (!in || in->Wheel == 0) return;`), `IsometricFollowCameraSystem` (`bool found = false; glm::vec3 target(0.0f);`).
- `GameRegisterSystems` order: TextRotation, DayNight, DebugSpawn, PlayerMovement, CameraZoom, IsometricFollowCamera, Quit.
- Test precedent with only a common include (no libs): `test_logformat` in `tests/CMakeLists.txt`.

> **ECS.h changes here require: rebuild `ecs` + `editor` + `runtime` + `game`, and restart the editor once** (per CLAUDE.md). `GAME_API_VERSION` bumps to `9u`.

---

## File Structure

- Create `src/common/include/GameStateId.h` — the moved enum (standalone, no deps).
- Create `src/common/include/StateScope.h` — pure `ScopeAllows` helper (Phase 3 uses it; landed now with its test).
- Modify `src/common/include/ECS.h` — include `GameStateId.h`; add `GameStateComponent`, `ActionEvent`, `ActionQueueComponent`; 2 X-macro entries.
- Modify `src/game/include/game.h` — remove local `GameStateId`; bump version.
- Modify `src/game/src/game.cpp` — gate 3 systems; add `AppFlowSystem`; boot seeding + top-of-tick mirror/clear + switch collapse; register `AppFlowSystem`.
- Create `tests/test_menu.cpp` + modify `tests/CMakeLists.txt`.

---

### Task 1: Common state types (GameStateId move + new singletons)

**Files:**
- Create: `src/common/include/GameStateId.h`
- Modify: `src/common/include/ECS.h`
- Modify: `src/game/include/game.h`

- [ ] **Step 1: Create the moved enum header**

Create `src/common/include/GameStateId.h`:

```cpp
#pragma once
#include <cstdint>

// Game lifecycle state. Lives in common (not game.h) so ECS components and engine code
// can reference it; game.h gets it transitively via ECS.h.
enum class GameStateId : uint32_t {
    Uninitialized = 0,
    MainMenu      = 1,
    InLevel       = 2,
    InEditor      = 3,
    Paused        = 4,
};
```

- [ ] **Step 2: Include it in ECS.h**

In `src/common/include/ECS.h`, with the other project includes (next to `#include "input.h"`, line ~23), add:

```cpp
#include "GameStateId.h"
```

- [ ] **Step 3: Add the new component structs**

In `src/common/include/ECS.h`, immediately **before** the `#define ECS_FOR_EACH_REGISTERED_COMPONENT(X) \` line (line ~177), add:

```cpp
// Current game lifecycle state (singleton). Authoritative source of truth: AppFlowSystem
// writes Current; systems + the renderer read it. Not persisted in world.json (runtime).
struct GameStateComponent {
    GameStateId Current = GameStateId::MainMenu;
};

// One queued UI/game action. Producers (menu interaction, Phase 4) push; owning systems
// consume by ActionId. Param is a generic payload; Source is the emitting entity (0 = none).
struct ActionEvent {
    uint32_t ActionId = 0;
    EntityId Source   = 0;
    uint64_t Param    = 0;
};

// Per-tick action queue (singleton). Cleared at the top of each GameUpdate tick and drained
// by consumer systems the same tick. Not persisted.
struct ActionQueueComponent {
    std::vector<ActionEvent> Events;
};
```

- [ ] **Step 4: Register the two singletons in the X-macro**

In `src/common/include/ECS.h`, change the tail of `ECS_FOR_EACH_REGISTERED_COMPONENT` from:

```cpp
    X(PlayerComponent) \
    X(CameraZoomComponent)
```

to:

```cpp
    X(PlayerComponent) \
    X(CameraZoomComponent) \
    X(GameStateComponent) \
    X(ActionQueueComponent)
```

(`ActionEvent` is a plain struct held by `ActionQueueComponent`, **not** a component — do not add it to the X-macro.)

- [ ] **Step 5: Remove the local enum from game.h + bump the version**

In `src/game/include/game.h`, replace:

```cpp
#define GAME_API_VERSION 8u

enum class GameStateId : uint32_t {
    Uninitialized = 0,
    MainMenu = 1,
    InLevel = 2,
    InEditor = 3,
    Paused = 4,
};
```

with:

```cpp
#define GAME_API_VERSION 9u

// GameStateId moved to src/common/include/GameStateId.h (included via ECS.h) so ECS
// components + engine code can reference it.
```

(`game.h` already `#include "ECS.h"` before the `GameState` struct, so `GameStateId` stays in scope.)

- [ ] **Step 6: Configure + build ecs/editor/runtime/game**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target ecs editor runtime game
```
Expected: all build with no errors.

- [ ] **Step 7: Verify the ECS test still passes (ECS.h changed)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

- [ ] **Step 8: Commit**

```bash
git add src/common/include/GameStateId.h src/common/include/ECS.h src/game/include/game.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(state): move GameStateId to common; add GameStateComponent + ActionQueue singletons"
```

---

### Task 2: ScopeAllows pure helper + test (TDD)

**Files:**
- Create: `src/common/include/StateScope.h`
- Create: `tests/test_menu.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_menu.cpp`:

```cpp
#include <cstdio>
#include "StateScope.h" // ScopeAllows + GameStateId

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

// mask == 0 => always-on (unscoped entity).
static void T00_unscoped_is_always_on() {
    EXPECT(ScopeAllows(0u, GameStateId::MainMenu));
    EXPECT(ScopeAllows(0u, GameStateId::InLevel));
}

// A single-state mask matches only that state.
static void T01_single_state() {
    const uint32_t menu = 1u << static_cast<uint32_t>(GameStateId::MainMenu);
    EXPECT(ScopeAllows(menu, GameStateId::MainMenu));
    EXPECT(!ScopeAllows(menu, GameStateId::InLevel));
}

// A multi-state mask matches any of its states.
static void T02_multi_state() {
    const uint32_t mask = (1u << static_cast<uint32_t>(GameStateId::MainMenu))
                        | (1u << static_cast<uint32_t>(GameStateId::Paused));
    EXPECT(ScopeAllows(mask, GameStateId::MainMenu));
    EXPECT(ScopeAllows(mask, GameStateId::Paused));
    EXPECT(!ScopeAllows(mask, GameStateId::InLevel));
}

int main() {
    T00_unscoped_is_always_on();
    T01_single_state();
    T02_multi_state();
    if (g_Failures == 0) { std::printf("All menu tests passed.\n"); return 0; }
    std::printf("%d menu test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Register the test target**

Append to `tests/CMakeLists.txt`:

```cmake
add_executable(test_menu
    test_menu.cpp
)

target_include_directories(test_menu PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

set_target_properties(test_menu PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Configure + build, verify it FAILS**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_menu
```
Expected: compile error — `StateScope.h` not found / `ScopeAllows` undefined.

- [ ] **Step 4: Create the helper**

Create `src/common/include/StateScope.h`:

```cpp
#pragma once
#include <cstdint>
#include "GameStateId.h"

// True if an entity scoped by `mask` is active in state `cur`. mask == 0 means "always-on"
// (no StateScopeComponent / unscoped). Bit i set => active in GameStateId value i.
inline bool ScopeAllows(uint32_t mask, GameStateId cur) {
    return mask == 0u || (mask & (1u << static_cast<uint32_t>(cur))) != 0u;
}
```

- [ ] **Step 5: Build + run, verify it PASSES**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_menu
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
```
Expected: `All menu tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/common/include/StateScope.h tests/test_menu.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(state): add pure ScopeAllows helper + test_menu"
```

---

### Task 3: Gate gameplay + AppFlowSystem + boot seeding (game.cpp)

**Files:**
- Modify: `src/game/src/game.cpp`

- [ ] **Step 1: Gate the three gameplay systems on InLevel**

In `src/game/src/game.cpp`, insert this snippet as the **first statements** inside `Update(SystemContext& ctx)` (immediately after the opening `{`) of **each** of `PlayerMovementSystem`, `CameraZoomSystem`, and `IsometricFollowCameraSystem`:

```cpp
        // Gameplay runs only in-level.
        const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
        if (!gs || gs->Current != GameStateId::InLevel) return;
```

(Three separate insertions — one per system. Leave the rest of each `Update` body unchanged.)

- [ ] **Step 2: Add AppFlowSystem**

In `src/game/src/game.cpp`, inside the anonymous `namespace {`, immediately **after** `PlayerMovementSystem`'s closing `};` (and before `} // namespace`), add:

```cpp
// Owns game-state transitions. Drains the per-tick ActionQueue (producers arrive in Phase 4)
// and routes navigation actions. TEMP (Phase 1): toggles MainMenu<->InLevel on TAB so the
// gameplay gating is testable before the menu exists — remove the TAB block in Phase 4.
class AppFlowSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (const auto* q = ctx.world.GetSingleton<ActionQueueComponent>()) {
            for (const ActionEvent& e : q->Events) {
                // Phase 4: route by CategoryOf(e.ActionId) and apply Nav transitions here.
                (void)e;
            }
        }

        // TEMP (Phase 1 testability) — remove in Phase 4.
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (in && in->Pressed[KEY_TAB]) {
            ctx.world.ModifySingleton<GameStateComponent>([](GameStateComponent& s) {
                s.Current = (s.Current == GameStateId::InLevel)
                              ? GameStateId::MainMenu : GameStateId::InLevel;
            });
        }
    }
    const char* Name() const override { return "AppFlowSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};
```

- [ ] **Step 3: Register AppFlowSystem before the gameplay systems**

In `GameRegisterSystems`, change:

```cpp
    s->Register(std::make_unique<TextRotationSystem>());
    s->Register(std::make_unique<DayNightSystem>());
    s->Register(std::make_unique<DebugSpawnSystem>());
    s->Register(std::make_unique<PlayerMovementSystem>());
```

to:

```cpp
    s->Register(std::make_unique<TextRotationSystem>());
    s->Register(std::make_unique<DayNightSystem>());
    s->Register(std::make_unique<AppFlowSystem>());   // owns transitions; runs before gameplay
    s->Register(std::make_unique<DebugSpawnSystem>());
    s->Register(std::make_unique<PlayerMovementSystem>());
```

- [ ] **Step 4: Add the top-of-tick mirror + queue clear in GameUpdate**

In `src/game/src/game.cpp`, in `GameUpdate`, immediately after the `if (!g_GameState) return;` line and before the comment/`switch`, add:

```cpp
    // Mirror authoritative ECS state onto the host field (legacy/editor reads). Absent on the
    // very first tick (seeded in the boot block below).
    if (const auto* gs = g_GameState->World.GetSingleton<GameStateComponent>())
        g_GameState->StateId = gs->Current;

    // Clear the per-tick action queue; producers push fresh events this tick.
    if (g_GameState->World.GetSingleton<ActionQueueComponent>())
        g_GameState->World.ModifySingleton<ActionQueueComponent>(
            [](ActionQueueComponent& q){ q.Events.clear(); });
```

- [ ] **Step 5: Collapse the switch to a one-time boot block**

In `src/game/src/game.cpp`, replace the **entire** `switch (g_GameState->StateId) { ... }` statement (the `Uninitialized` boot case plus the empty `MainMenu`/`InLevel`/`InEditor`/`Paused`/`default` cases) with:

```cpp
    // One-time boot: seed singletons + the starting scene, then enter MainMenu. After this,
    // GameStateComponent.Current is authoritative (AppFlowSystem drives transitions).
    if (g_GameState->StateId == GameStateId::Uninitialized) {
        SM_TRACE("[GAMEDLL] Initializing game...");

        // Game-owned singletons.
        g_GameState->World.SetSingleton(WorldCameraComponent{});
        g_GameState->World.SetSingleton(AppControlComponent{});
        g_GameState->World.SetSingleton(AtmosphereStateComponent{});
        g_GameState->World.SetSingleton(GameStateComponent{ GameStateId::MainMenu });
        g_GameState->World.SetSingleton(ActionQueueComponent{});

        // Persisted scene atmosphere may already be set by a startup world.json load; seed
        // defaults only when absent so loaded values aren't clobbered.
        if (!g_GameState->World.GetSingleton<DayNightConfigComponent>())
            g_GameState->World.SetSingleton(DayNightConfigComponent{});
        if (!g_GameState->World.GetSingleton<FogComponent>())
            g_GameState->World.SetSingleton(FogComponent{});
        if (!g_GameState->World.GetSingleton<SkyComponent>())
            g_GameState->World.SetSingleton(SkyComponent{});

        // World loaded from world.json is authoritative — skip default spawns to avoid
        // duplicates. Defaults are a fallback scene when no world is present.
        if (!g_GameState->WorldLoaded) {
            SM_TRACE("[GAMEDLL] No world loaded — spawning default scene");

            const auto textEntity = g_GameState->World.CreateEntity();
            g_GameState->World.AddComponent(textEntity, TransformComponent{.Position = glm::vec3{740.f, 250.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}});
            g_GameState->World.AddComponent(textEntity, TextComponent{.Text = "Hello, Game!", .Color = glm::vec4{1.0f, 1.0f, 1.0f, 1.0f}, .FontSize = 48});

            const auto sun = g_GameState->World.CreateEntity();
            g_GameState->World.AddComponent(sun, TransformComponent{.Position = glm::vec3{0.f, 0.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}});
            g_GameState->World.AddComponent(sun, LightningComponent{
                .Type = LightningType::Directional,
                .Direction = glm::vec4(glm::normalize(glm::vec3(-1.0f, -1.0f, -1.0f)), 0.0f),
                .Color = glm::vec4(1.0f, 0.95f, 0.9f, 1.0f)
            });
            g_GameState->World.AddComponent(sun, SunMarker{});

            const auto pointLight = g_GameState->World.CreateEntity();
            g_GameState->World.AddComponent(pointLight, TransformComponent{.Position = glm::vec3{0.f, 4.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}});
            g_GameState->World.AddComponent(pointLight, LightningComponent{
                .Type = LightningType::Point,
                .Color = glm::vec4(1.0f, 0.8f, 0.6f, 1.0f),
                .Intensity = 1.0f,
                .Range = 3.0f
            });
        }

        g_GameState->StateId = GameStateId::MainMenu;
    }
```

(This preserves the original boot body verbatim, adds the two new singleton seeds, and drops the now-empty per-state cases — the state machine lives in `AppFlowSystem` + the gating, not the switch.)

- [ ] **Step 6: Build editor + runtime + game**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor runtime game`
Expected: builds with no errors.

- [ ] **Step 7: Manual smoke (human — requires editor restart from Task 1's ECS.h change)**

Restart the editor (ECS.h layout changed in Task 1; `GAME_API_VERSION` is now 9). Then verify:
- On boot the game is in **MainMenu**: the player does **not** move with WASD, the camera does **not** follow/zoom (gameplay systems gated off). DayNight/text still animate.
- Press **TAB** → **InLevel**: WASD moves the player, the camera follows, the wheel zooms.
- Press **TAB** again → **MainMenu**: gameplay freezes again.
- Console shows no errors; no per-tick spam.

(The implementer cannot run the GUI — build to confirm compilation and report; the human runs this smoke.)

- [ ] **Step 8: Commit**

```bash
git add src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(state): gate gameplay to InLevel + AppFlowSystem + boot seeding (Phase 1)"
```

---

## Final verification

- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` → `All ECS tests passed.`
- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe` → `All menu tests passed.`
- [ ] Human smoke per Task 3 Step 7 (DX12; optionally Vulkan).

## Self-review notes (vs. spec §15 Phase 1)

- Covers: GameStateId→common, GameStateComponent singleton (not persisted), ActionQueueComponent (cleared per tick), gating the 3 gameplay systems to InLevel, AppFlowSystem skeleton (drains queue) + boot seeding + Current→StateId mirror + switch collapse, GAME_API bump, pure ScopeAllows + test, temporary TAB toggle for testability.
- Deferred to later phases (correctly absent here): StateScopeComponent the component (Phase 3), mouse input + viewport origin (Phase 2), UIRect/MenuButton/Actions.h/menu interaction (Phases 3–4). `ScopeAllows` lands now as a pure helper but isn't wired into rendering until Phase 3.
- `ActionEvent` is defined now (needed by `ActionQueueComponent`) but has no producers until Phase 4 — intentional.
