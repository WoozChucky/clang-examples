# Player Component + Movement System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `PlayerComponent` and a `PlayerMovementSystem` that moves any entity carrying it on the XZ plane via WASD, tagged onto an existing entity through the editor inspector.

**Architecture:** New ECS component (`ECS.h` X-macro + `ECSCommands` registration). A pure `ComputePlanarMove` helper (game header, unit-tested) drives a `PlayerMovementSystem` (`ISystem`, Simulation phase) registered in `GameRegisterSystems`. The editor inspector gains add/remove/edit for the component. Camera-follow, scene-persistence, and a default-scene spawn are deferred.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), game `ISystem`/`SystemScheduler` (hot-reloaded `Game.dll`), Dear ImGui editor inspector, GLM. Pure helper unit-tested via a new `test_playermove` target.

**Spec:** `docs/superpowers/specs/2026-05-25-player-movement-design.md`

---

## Build & test reference (every task)

Build preset: **`msvc-win64-vs2026-community`** (enterprise preset is NOT installed — never use it). Binaries in `out/build/msvc-win64-vs2026-community/bin/Debug/`.

```powershell
cmake --preset msvc-win64-vs2026-community                                   # reconfigure (after CMakeLists changes)
cmake --build --preset msvc-win64-vs2026-community --target <target>          # build
./out/build/msvc-win64-vs2026-community/bin/Debug/<test>.exe                   # run a test
```

**Commit identity (MANDATORY):** `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "<msg>"`. Never the work email. Never `git add .claude/`.

**Restart caveat:** Task 1 changes `ECS.h` layout + bumps `GAME_API_VERSION`. After the editor + game are rebuilt (Tasks 2–3), the user must **restart `editor.exe`** (the running one has the old layout linked in). A subagent cannot restart it — build everything and report that a restart + GUI smoke is required.

## File map

- `src/common/include/ECS.h` — `PlayerComponent` + X-macro. (Task 1)
- `src/common/include/ECSCommands.h` — add/modify + remove dispatch. (Task 1)
- `src/game/include/Game.h` — `GAME_API_VERSION` 6 → 7. (Task 1)
- `src/game/src/PlayerMovement.h` — **new.** Pure `ComputePlanarMove`. (Task 2)
- `src/game/src/game.cpp` — `PlayerMovementSystem` + register + include. (Task 2)
- `tests/test_playermove.cpp` + `tests/CMakeLists.txt` — **new** test target. (Task 2)
- `src/editor/src/rendering/imgui/EcsInspectorPanel.h` — 3 edit-state members. (Task 3)
- `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp` — add/remove/edit UI. (Task 3)

---

## Task 1: `PlayerComponent` + command registration + version bump

**Files:**
- Modify: `src/common/include/ECS.h` (struct after `ViewportComponent` line 175; X-macro line 199)
- Modify: `src/common/include/ECSCommands.h` (`ApplyComponentCommand` ~line 262; `RemoveComponentByType` ~line 287)
- Modify: `src/game/include/Game.h` (line 20)

Structural task; verification is a clean `ecs` build (serialization/UI come later).

- [ ] **Step 1: Add `PlayerComponent` to `ECS.h`**

In `src/common/include/ECS.h`, immediately after `struct ViewportComponent { ... };` (line 175) and before the X-macro comment, insert:

```cpp
// Marks the player-controlled entity. Moved by PlayerMovementSystem (game) from input.
struct PlayerComponent {
    float MoveSpeed = 5.0f; // world units / second
};
```

- [ ] **Step 2: Register it in the X-macro**

In `ECS_FOR_EACH_REGISTERED_COMPONENT`, change the final two lines (currently
`    X(AppControlComponent) \` / `    X(ViewportComponent)`) to add `PlayerComponent`
(keep the macro's no-trailing-backslash-on-last-line rule):

```cpp
    X(AppControlComponent) \
    X(ViewportComponent) \
    X(PlayerComponent)
```

- [ ] **Step 3: Register in `ECSCommands.h` `ApplyComponentCommand`**

After the `SkyComponent` branch (ends ~line 262, before `// Add more component types as needed`), add:

```cpp
        } else if (componentData.Type == std::type_index(typeid(PlayerComponent))) {
            if (auto* player = componentData.Get<PlayerComponent>()) {
                world.AddComponent(entity, *player);
            }
```

- [ ] **Step 4: Register in `ECSCommands.h` `RemoveComponentByType`**

After the `SkyComponent` branch (~line 286), add:

```cpp
        } else if (typeIndex == std::type_index(typeid(PlayerComponent))) {
            world.RemoveComponent<PlayerComponent>(entity);
```

- [ ] **Step 5: Bump `GAME_API_VERSION` in `Game.h`**

In `src/game/include/Game.h`, change line 20:

```cpp
#define GAME_API_VERSION 7u
```

- [ ] **Step 6: Build `ecs` to verify the new instantiation compiles**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target ecs
```
Expected: builds clean (the X-macro now instantiates `ComponentArray<PlayerComponent>`).

- [ ] **Step 7: Commit**

```powershell
git add src/common/include/ECS.h src/common/include/ECSCommands.h src/game/include/Game.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs): add PlayerComponent (+ ECSCommands, GAME_API_VERSION 7)"
```

---

## Task 2: Movement helper + `PlayerMovementSystem` + test (TDD)

**Files:**
- Create: `src/game/src/PlayerMovement.h`
- Create: `tests/test_playermove.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/game/src/game.cpp`

The pure helper is testable with no game/engine link (`ECS.h` self-defines `ECS_API` and the test only constructs the plain `InputStateComponent` + calls the helper). TDD it first, then wire the system.

- [ ] **Step 1: Write the failing test `tests/test_playermove.cpp`**

```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "PlayerMovement.h" // ComputePlanarMove + InputStateComponent/KEY_* via ECS.h

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void T00_cardinals()
{
    InputStateComponent w{}; w.KeysDown[KEY_W] = true;
    const glm::vec3 d = ComputePlanarMove(w, 5.0f, 2.0f); // speed*dt = 10
    EXPECT(near(d.x, 0.0f) && near(d.y, 0.0f) && near(d.z, -10.0f));

    InputStateComponent a{}; a.KeysDown[KEY_A] = true;
    EXPECT(near(ComputePlanarMove(a, 5.0f, 2.0f).x, -10.0f));
    InputStateComponent s{}; s.KeysDown[KEY_S] = true;
    EXPECT(near(ComputePlanarMove(s, 5.0f, 2.0f).z, 10.0f));
    InputStateComponent d2{}; d2.KeysDown[KEY_D] = true;
    EXPECT(near(ComputePlanarMove(d2, 5.0f, 2.0f).x, 10.0f));
}

static void T01_diagonal_normalized()
{
    InputStateComponent in{}; in.KeysDown[KEY_W] = true; in.KeysDown[KEY_D] = true;
    const glm::vec3 d = ComputePlanarMove(in, 5.0f, 2.0f); // length must be speed*dt = 10, not 10*sqrt(2)
    EXPECT(near(glm::length(d), 10.0f));
    EXPECT(d.x > 0.0f && d.z < 0.0f);
    EXPECT(near(d.y, 0.0f));
}

static void T02_opposing_and_none_zero()
{
    InputStateComponent ws{}; ws.KeysDown[KEY_W] = true; ws.KeysDown[KEY_S] = true;
    EXPECT(near(glm::length(ComputePlanarMove(ws, 5.0f, 2.0f)), 0.0f));

    InputStateComponent none{};
    EXPECT(near(glm::length(ComputePlanarMove(none, 5.0f, 2.0f)), 0.0f));
}

static void T03_scaling()
{
    InputStateComponent in{}; in.KeysDown[KEY_W] = true;
    const float l1 = glm::length(ComputePlanarMove(in, 5.0f, 1.0f));
    const float l2 = glm::length(ComputePlanarMove(in, 5.0f, 2.0f));
    const float l3 = glm::length(ComputePlanarMove(in, 10.0f, 1.0f));
    EXPECT(near(l2, 2.0f * l1));
    EXPECT(near(l3, 2.0f * l1));
}

int main()
{
    T00_cardinals();
    T01_diagonal_normalized();
    T02_opposing_and_none_zero();
    T03_scaling();
    if (g_Failures == 0) { std::printf("All player movement tests passed.\n"); return 0; }
    std::printf("%d player movement test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the `test_playermove` target to `tests/CMakeLists.txt`**

Append at the end:

```cmake
add_executable(test_playermove
    test_playermove.cpp
)

target_link_libraries(test_playermove PRIVATE
    glm::glm
)

target_include_directories(test_playermove PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/game/src
)

target_compile_definitions(test_playermove PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_playermove PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Configure + build to verify FAIL (header missing)**

```powershell
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_playermove
```
Expected: FAIL — `Cannot open include file: 'PlayerMovement.h'`.

- [ ] **Step 4: Create `src/game/src/PlayerMovement.h`**

```cpp
#pragma once

#include <glm/glm.hpp>

#include "ECS.h" // InputStateComponent + KEY_* codes (via input.h)

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

- [ ] **Step 5: Build + run the test → PASS**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_playermove
./out/build/msvc-win64-vs2026-community/bin/Debug/test_playermove.exe
```
Expected: `All player movement tests passed.`

- [ ] **Step 6: Add `PlayerMovementSystem` to `src/game/src/game.cpp`**

(a) Add the include near the top (after `#include "Systems.h"`, line 2):

```cpp
#include "PlayerMovement.h"
```

(b) Add the system class inside the anonymous namespace, after `DebugSpawnSystem` (just before `} // namespace` at line 174):

```cpp
// Moves entities tagged with PlayerComponent on the XZ plane from WASD input.
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

(c) Register it in `GameRegisterSystems` (after the `DebugSpawnSystem` registration, line 181):

```cpp
    s->Register(std::make_unique<PlayerMovementSystem>());
```

- [ ] **Step 7: Build `game` to verify the system compiles**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target game
```
Expected: builds clean.

- [ ] **Step 8: Commit**

```powershell
git add src/game/src/PlayerMovement.h tests/test_playermove.cpp tests/CMakeLists.txt src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(game): PlayerMovementSystem + ComputePlanarMove helper + tests"
```

---

## Task 3: Editor inspector — add / remove / edit PlayerComponent

**Files:**
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.h` (3 members after the Text edit-state, ~line 39)
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp` (add-menu ~line 168, remove-menu ~line 221, editor block near the other component editors)

Mirror the existing per-component pattern exactly (the `MeshComponent`/`LightningComponent` add/remove/edit blocks).

- [ ] **Step 1: Add the edit-state members to `EcsInspectorPanel.h`**

In `class EcsInspectorPanel` private members, after the Text block
(`TextComponent editTextComp{}; EntityId lastEditedTextEntity = INVALID_ENTITY; bool textModified = false;`),
add:

```cpp
    PlayerComponent    editPlayer{};
    EntityId           lastEditedPlayerEntity = INVALID_ENTITY;
    bool               playerModified = false;
```

(`PlayerComponent` is visible — the header already `#include`s `"ECS.h"`.)

- [ ] **Step 2: Add the "Add Player Component" menu item**

In `EcsInspectorPanel.cpp`, in the add-component menu, after the `SunMarker` add block
(ends ~line 168, before `ImGui::Separator();` at line 170), add:

```cpp
                if (!ctx.WorldSnapshot->HasComponent<PlayerComponent>(entity)) {
                    if (ImGui::MenuItem("Add Player Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, PlayerComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }
```

- [ ] **Step 3: Add the "Remove Player Component" menu item**

In the remove-component menu, after the `SunMarker` remove block (~line 221), add:

```cpp
                if (ctx.WorldSnapshot->HasComponent<PlayerComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Player Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<PlayerComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }
```

- [ ] **Step 4: Add the PlayerComponent editor block**

Among the per-component editors (after the Text/Material editor block; place it next to
the other `if (ctx.WorldSnapshot->HasComponent<...>(selectedEntity))` editor blocks),
add (mirrors the Lightning editor's reset-on-switch / live-refresh / dirty-push pattern):

```cpp
            if (ctx.WorldSnapshot->HasComponent<PlayerComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Player Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* player = ctx.WorldSnapshot->GetComponent<PlayerComponent>(selectedEntity);
                    if (player) {
                        if (lastEditedPlayerEntity != selectedEntity) {
                            editPlayer = *player;
                            lastEditedPlayerEntity = selectedEntity;
                            playerModified = false;
                        }
                        if (!playerModified) {
                            editPlayer = *player;
                        }
                        if (ImGui::DragFloat("Move speed", &editPlayer.MoveSpeed, 0.1f, 0.5f, 50.0f, "%.1f")) {
                            playerModified = true;
                        }
                        ImGui::Spacing();
                        if (playerModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editPlayer);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            playerModified = false;
                        }
                    }
                }
            }
```

- [ ] **Step 5: Build `editor`**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds clean.

- [ ] **Step 6: Full rebuild + regression tests**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target editor
cmake --build --preset msvc-win64-vs2026-community --target game
cmake --build --preset msvc-win64-vs2026-community --target test_playermove
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_playermove.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `editor` + `game` build clean; both tests print their `All … passed.` lines.

- [ ] **Step 7: Commit**

```powershell
git add src/editor/src/rendering/imgui/EcsInspectorPanel.h src/editor/src/rendering/imgui/EcsInspectorPanel.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): inspector add/remove/edit PlayerComponent"
```

> After this, the user restarts `editor.exe` (ECS.h layout + `GAME_API_VERSION` 7) and GUI-smokes.

---

## Done criteria

- `test_playermove` + `test_ecs` print their `All … passed.` lines; `ecs`/`editor`/`game` build clean.
- User GUI smoke (after editor restart): select a mesh entity → Inspector → "Add Player Component" → press WASD → the entity slides on the XZ plane (diagonal not faster); adjust "Move speed" in the inspector and the speed changes; "Remove Player Component" stops it.
