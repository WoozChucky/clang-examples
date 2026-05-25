# State-Gated Menu — Phase 4 (Menu Buttons + Action Layer) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn authored UI rects into clickable menu buttons that emit decoupled `ActionEvent`s, consumed by an `AppFlowSystem` that drives state transitions (Play → InLevel, Quit → exit, Back/ESC → MainMenu) — a working main menu.

**Architecture:** `MenuButtonComponent` (authored: `ActionId` + hover/press colors) marks a UI-rect entity as a button. `MenuInteractionSystem` hit-tests scoped buttons against the UI-space mouse, drives their `UIRectComponent.Color` (Normal/Hover/Press), and on a press→release click pushes an `ActionEvent{ActionId}` into the per-tick `ActionQueue`. `AppFlowSystem` (rewritten) consumes `Nav`-category actions and performs the transitions. A code-spawned fallback menu makes it usable without authoring.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), hot-reloaded `Game.dll` systems, NVRHI UI pass, Dear ImGui editor, CMake (`msvc-win64-vs2026-community`).

**Spec:** `docs/superpowers/specs/2026-05-25-state-gated-menu-design.md` (§5, §6, §7, §8, §9, §10, §15 Phase 4). Builds on Phases 1-3 (already on this branch).

---

## Reference facts (verified against the codebase)

- Pure helpers in `src/common/include/MenuHitTest.h`: `ToUiSpace(double,double,uint32_t,uint32_t)->glm::vec2` (Phase 2). Includes `<cstdint>` + `<glm/vec2.hpp>`.
- `ScopeAllows(mask,cur)` in `StateScope.h`; `GameStateComponent{Current}`, `ActionQueueComponent{std::vector<ActionEvent>}`, `ActionEvent{uint32_t ActionId; EntityId Source; uint64_t Param;}` in `ECS.h` (Phase 1); `UIRectComponent{glm::vec2 Size; glm::vec4 Color;}`, `StateScopeComponent{uint32_t StateMask;}` (Phase 3); `InputStateComponent.MouseDown[8]/MousePressed[8]` + `ViewportComponent.OriginX/OriginY` (Phase 2). `MOUSE_BUTTON_LEFT = MOUSE_BUTTON_1 = 0`, `KEY_ESCAPE = 256` (input.h).
- `game.cpp` current state: `AppFlowSystem` (drain-stub + TEMP TAB toggle, lines ~222-246); `GameRegisterSystems` order Text/DayNight/AppFlow/DebugSpawn/PlayerMovement/CameraZoom/IsometricFollow/Quit (lines ~250-260); `QuitRequestSystem` (ESC→quit) still registered; `GameUpdate` boot block seeds singletons + spawns a fallback scene under `if (!g_GameState->WorldLoaded)` (lines ~292-336). `game.cpp` includes `"Game.h"`, `Systems.h`, `PlayerMovement.h`, `CameraFollow.h`, `ApplicationContext.h`, `Input.h`, glm.
- ECS authorable-component pattern (Phase 3 `UIRectComponent`): X-macro entry; ECSCommands `ApplyComponentCommand` + `RemoveComponentByType`; `ComponentSerialization.h` `to_json`/`from_json`; `WorldManager.cpp` save/load branches; `EcsInspectorPanel.{h,cpp}` add/remove/edit + members; `DuplicateEntityComponents` (`ECSCommands.h`). Runtime singletons (`GameStateComponent`/`ActionQueueComponent`) are X-macro only (no commands/serialization), seeded in the boot block.
- `Each<A,B,...>(EntityId-only lambda)` collects ids first, so `Modify<T>(e, …)` inside the loop is safe (clones the array) — established by `PlayerMovementSystem`.
- `game.h`: `GAME_API_VERSION 11u` (after Phase 3).

> **ECS.h changes → rebuild `ecs`+`editor`+`runtime`+`game` + restart editor once.** `GAME_API_VERSION` → `12u` (Task 1).
> **Input-routing reminder (memory `editor-input-routing`):** in the editor the game only receives mouse input in **Play mode** (viewport hovered, no gizmo). Smoke-test menu clicks in **Play mode** or `runtime.exe`.

---

## File Structure

- Create `src/common/include/Actions.h` — `ActionCategory`, `MakeAction`, `CategoryOf`, `Actions::{None,Play,Quit,Back}`.
- `src/common/include/ECS.h` — `MenuButtonComponent` (authored) + `MenuStateComponent` (runtime singleton) + 2 X-macro entries.
- `src/common/include/ECSCommands.h` — `MenuButtonComponent` Apply + Remove + DuplicateEntityComponents.
- `src/common/include/ComponentSerialization.h` + `src/engine/src/utilities/WorldManager.cpp` — `MenuButtonComponent` persistence.
- `src/common/include/MenuHitTest.h` — add `PointInRect`.
- `src/editor/src/rendering/imgui/EcsInspectorPanel.{h,cpp}` — `MenuButtonComponent` add/remove/edit (ActionId combo + colors).
- `src/game/src/game.cpp` — `MenuInteractionSystem`, rewritten `AppFlowSystem`, registration, boot seed + fallback menu, `#include "Actions.h"`.
- `src/game/include/game.h` — `GAME_API_VERSION` 11→12.
- `tests/test_menu.cpp` (PointInRect), `tests/test_worldserial.cpp` (MenuButton round-trip).

---

### Task 1: Actions.h + MenuButtonComponent + MenuStateComponent (ECS + commands)

**Files:**
- Create: `src/common/include/Actions.h`
- Modify: `src/common/include/ECS.h`
- Modify: `src/common/include/ECSCommands.h`
- Modify: `src/game/include/game.h`

- [ ] **Step 1: Create the action-id header**

Create `src/common/include/Actions.h`:

```cpp
#pragma once
#include <cstdint>

// Action identifiers carried by ActionEvent and authored on MenuButtonComponent. Namespaced by
// category in the high 16 bits so each owning system claims a category and ignores the rest.
enum class ActionCategory : uint16_t {
    None = 0,
    Nav  = 1,
    // future: Inventory = 2, Ability = 3, ... (each owned by its own system)
};

constexpr uint32_t MakeAction(ActionCategory c, uint16_t local) {
    return (static_cast<uint32_t>(c) << 16) | local;
}
constexpr ActionCategory CategoryOf(uint32_t id) {
    return static_cast<ActionCategory>(id >> 16);
}

namespace Actions {
    inline constexpr uint32_t None = 0;
    inline constexpr uint32_t Play = MakeAction(ActionCategory::Nav, 1);
    inline constexpr uint32_t Quit = MakeAction(ActionCategory::Nav, 2);
    inline constexpr uint32_t Back = MakeAction(ActionCategory::Nav, 3);
}
```

- [ ] **Step 2: Add the two components**

In `src/common/include/ECS.h`, immediately after the `struct StateScopeComponent { … };` (Phase 3, line ~188), add:

```cpp
// Marks a UI-rect entity as a clickable menu button (authored). ActionId is an Actions:: id
// (data binding to behavior; 0 = none). The interaction system drives UIRectComponent.Color
// between Normal/Hover/Press based on the pointer.
struct MenuButtonComponent {
    uint32_t  ActionId = 0;
    glm::vec4 Normal{0.15f, 0.15f, 0.18f, 1.0f};
    glm::vec4 Hover {0.25f, 0.25f, 0.30f, 1.0f};
    glm::vec4 Press {0.35f, 0.35f, 0.42f, 1.0f};
};

// Runtime singleton: the button currently held under a left-press (click latch). 0 = none.
// Not persisted, not authored (seeded in the boot block like ActionQueueComponent).
struct MenuStateComponent {
    EntityId ArmedButton = 0;
};
```

- [ ] **Step 3: Register both in the X-macro**

In `src/common/include/ECS.h`, change the tail of `ECS_FOR_EACH_REGISTERED_COMPONENT` from:

```cpp
    X(UIRectComponent) \
    X(StateScopeComponent)
```

to:

```cpp
    X(UIRectComponent) \
    X(StateScopeComponent) \
    X(MenuButtonComponent) \
    X(MenuStateComponent)
```

- [ ] **Step 4: Register MenuButtonComponent in ECSCommands (Apply + Remove + Duplicate)**

In `src/common/include/ECSCommands.h`, in `ApplyComponentCommand`, after the `StateScopeComponent` add-branch (Phase 3), add:

```cpp
        } else if (componentData.Type == std::type_index(typeid(MenuButtonComponent))) {
            if (auto* btn = componentData.Get<MenuButtonComponent>()) {
                world.AddComponent(entity, *btn);
            }
```

In `RemoveComponentByType`, after the `StateScopeComponent` remove-branch, add:

```cpp
        } else if (typeIndex == std::type_index(typeid(MenuButtonComponent))) {
            world.RemoveComponent<MenuButtonComponent>(entity);
```

In `DuplicateEntityComponents`, after the `StateScopeComponent` copy line, add:

```cpp
        if (auto* c = world.GetComponent<MenuButtonComponent>(src))  world.AddComponent(dst, *c);
```

(`MenuStateComponent` is a runtime singleton — do NOT add it to ECSCommands or Duplicate.)

- [ ] **Step 5: Bump the game API version**

In `src/game/include/game.h`, change `#define GAME_API_VERSION 11u` to `#define GAME_API_VERSION 12u`.

- [ ] **Step 6: Configure + build**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target ecs editor runtime game
```
Expected: all build with no errors.

- [ ] **Step 7: Verify the ECS test still passes**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

- [ ] **Step 8: Commit**

```bash
git add src/common/include/Actions.h src/common/include/ECS.h src/common/include/ECSCommands.h src/game/include/game.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(menu): add Actions.h + MenuButtonComponent + MenuStateComponent"
```

---

### Task 2: PointInRect pure helper + test (TDD)

**Files:**
- Modify: `src/common/include/MenuHitTest.h`
- Modify: `tests/test_menu.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_menu.cpp`, add after the existing `T03_to_ui_space()`:

```cpp
// Half-open rect [pos, pos+size): inside, on each edge, outside.
static void T04_point_in_rect() {
    const glm::vec2 pos(100.0f, 50.0f), size(40.0f, 20.0f);
    EXPECT(PointInRect(glm::vec2(120.0f, 60.0f), pos, size));  // inside
    EXPECT(PointInRect(glm::vec2(100.0f, 50.0f), pos, size));  // top-left inclusive
    EXPECT(!PointInRect(glm::vec2(140.0f, 60.0f), pos, size)); // right edge exclusive (100+40)
    EXPECT(!PointInRect(glm::vec2(120.0f, 70.0f), pos, size)); // bottom edge exclusive (50+20)
    EXPECT(!PointInRect(glm::vec2(99.0f, 60.0f), pos, size));  // left of
    EXPECT(!PointInRect(glm::vec2(120.0f, 49.0f), pos, size)); // above
}
```

And call it in `main()` after `T03_to_ui_space();`:

```cpp
    T04_point_in_rect();
```

- [ ] **Step 2: Configure + build, verify it FAILS**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_menu
```
Expected: compile error — `PointInRect` undefined.

- [ ] **Step 3: Add the helper**

In `src/common/include/MenuHitTest.h`, after the `ToUiSpace` function, add:

```cpp
// True if point p is inside the half-open screen rect [pos, pos+size). Top-left inclusive,
// bottom-right exclusive — so adjacent rects don't both claim a shared edge pixel.
inline bool PointInRect(glm::vec2 p, glm::vec2 pos, glm::vec2 size) {
    return p.x >= pos.x && p.x < pos.x + size.x &&
           p.y >= pos.y && p.y < pos.y + size.y;
}
```

- [ ] **Step 4: Build + run, verify it PASSES**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_menu
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
```
Expected: `All menu tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/common/include/MenuHitTest.h tests/test_menu.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(menu): add pure PointInRect helper + test"
```

---

### Task 3: MenuButtonComponent persistence + inspector

**Files:**
- Modify: `src/common/include/ComponentSerialization.h`
- Modify: `src/engine/src/utilities/WorldManager.cpp`
- Modify: `tests/test_worldserial.cpp`
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.h`
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp`

- [ ] **Step 1: Write the failing serialization test**

In `tests/test_worldserial.cpp`, add after `T08_statescope_roundtrip()`:

```cpp
static void T09_menubutton_roundtrip()
{
    MenuButtonComponent in;
    in.ActionId = 0x00010002u; // Nav/Quit
    in.Normal = glm::vec4(0.1f, 0.2f, 0.3f, 1.0f);
    in.Hover  = glm::vec4(0.4f, 0.5f, 0.6f, 1.0f);
    in.Press  = glm::vec4(0.7f, 0.8f, 0.9f, 1.0f);
    const nlohmann::json j = in;
    const auto out = j.get<MenuButtonComponent>();
    EXPECT(out.ActionId == in.ActionId);
    EXPECT(veq(glm::vec3(out.Normal), glm::vec3(in.Normal)));
    EXPECT(veq(glm::vec3(out.Hover),  glm::vec3(in.Hover)));
    EXPECT(veq(glm::vec3(out.Press),  glm::vec3(in.Press)));
}
```

And call it in `main()` after `T08_statescope_roundtrip();`:

```cpp
    T09_menubutton_roundtrip();
```

- [ ] **Step 2: Build, verify it FAILS**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_worldserial`
Expected: compile error — no json conversion for `MenuButtonComponent`.

- [ ] **Step 3: Add the serializer**

In `src/common/include/ComponentSerialization.h`, after the `StateScopeComponent` `from_json` (Phase 3), add:

```cpp
inline void to_json(nlohmann::json& j, const MenuButtonComponent& t) {
    j = nlohmann::json{
        {"ActionId", t.ActionId},
        {"Normal", t.Normal},
        {"Hover",  t.Hover},
        {"Press",  t.Press}
    };
}
inline void from_json(const nlohmann::json& j, MenuButtonComponent& t) {
    j.at("ActionId").get_to(t.ActionId);
    j.at("Normal").get_to(t.Normal);
    j.at("Hover").get_to(t.Hover);
    j.at("Press").get_to(t.Press);
}
```

- [ ] **Step 4: Add WorldManager save + load branches**

In `src/engine/src/utilities/WorldManager.cpp`, in the SAVE loop after the `StateScopeComponent` block, add:

```cpp
        if (world->HasComponent<MenuButtonComponent>(entity)) {
            jEntity["MenuButtonComponent"] = *(world->GetComponent<MenuButtonComponent>(entity));
        }
```

In the LOAD loop after the `StateScopeComponent` line, add:

```cpp
            if (jEntity.contains("MenuButtonComponent"))
                world->AddComponent(createdEntity, jEntity["MenuButtonComponent"].get<MenuButtonComponent>());
```

- [ ] **Step 5: Build + run the serialization test, verify it PASSES**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: `All world-serialization tests passed.`

- [ ] **Step 6: Add inspector edit-state members**

In `src/editor/src/rendering/imgui/EcsInspectorPanel.h`, next to the Phase-3 `editUIRect`/`editScope` members, add:

```cpp
    MenuButtonComponent editMenuBtn{};
    EntityId            lastEditedMenuBtnEntity = 0;
    bool                menuBtnModified = false;
```

- [ ] **Step 7: Add the inspector add/remove menu items**

In `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp`, ensure the action ids are available — add near the top with the other includes:

```cpp
#include "Actions.h"
```

After the `Add State Scope Component` menu block (Phase 3), add:

```cpp
                if (!ctx.WorldSnapshot->HasComponent<MenuButtonComponent>(entity)) {
                    if (ImGui::MenuItem("Add Menu Button Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, MenuButtonComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }
```

After the `Remove State Scope Component` menu block, add:

```cpp
                if (ctx.WorldSnapshot->HasComponent<MenuButtonComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Menu Button Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<MenuButtonComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }
```

- [ ] **Step 8: Add the inspector edit header**

After the `Edit State Scope Component` collapsing-header block (Phase 3), add:

```cpp
            // Edit Menu Button Component
            if (ctx.WorldSnapshot->HasComponent<MenuButtonComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Menu Button Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* btn = ctx.WorldSnapshot->GetComponent<MenuButtonComponent>(selectedEntity);
                    if (btn) {
                        if (lastEditedMenuBtnEntity != selectedEntity) {
                            editMenuBtn = *btn;
                            lastEditedMenuBtnEntity = selectedEntity;
                            menuBtnModified = false;
                        }
                        if (!menuBtnModified) {
                            editMenuBtn = *btn;
                        }
                        static const char* kActionNames[] = { "None", "Play", "Quit", "Back" };
                        static const uint32_t kActionIds[] = { Actions::None, Actions::Play, Actions::Quit, Actions::Back };
                        int curIdx = 0;
                        for (int i = 0; i < 4; ++i) if (editMenuBtn.ActionId == kActionIds[i]) curIdx = i;
                        if (ImGui::Combo("Action", &curIdx, kActionNames, 4)) {
                            editMenuBtn.ActionId = kActionIds[curIdx];
                            menuBtnModified = true;
                        }
                        if (ImGui::ColorEdit4("Normal##MenuBtn", &editMenuBtn.Normal.x)) menuBtnModified = true;
                        if (ImGui::ColorEdit4("Hover##MenuBtn",  &editMenuBtn.Hover.x))  menuBtnModified = true;
                        if (ImGui::ColorEdit4("Press##MenuBtn",  &editMenuBtn.Press.x))  menuBtnModified = true;
                        ImGui::Spacing();
                        if (menuBtnModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editMenuBtn);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            menuBtnModified = false;
                        }
                    }
                }
            }
```

- [ ] **Step 9: Build the editor + game**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor game`
Expected: builds with no errors.

- [ ] **Step 10: Commit**

```bash
git add src/common/include/ComponentSerialization.h src/engine/src/utilities/WorldManager.cpp tests/test_worldserial.cpp src/editor/src/rendering/imgui/EcsInspectorPanel.h src/editor/src/rendering/imgui/EcsInspectorPanel.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(menu): persist + inspect MenuButtonComponent (ActionId combo + colors)"
```

---

### Task 4: MenuInteractionSystem (hit-test → hover/press → emit ActionEvent)

**Files:**
- Modify: `src/game/src/game.cpp`

- [ ] **Step 1: Add includes**

At the top of `src/game/src/game.cpp`, with the other includes, add:

```cpp
#include "MenuHitTest.h" // ToUiSpace + PointInRect
#include "StateScope.h"  // ScopeAllows
#include "Actions.h"     // ActionCategory / Actions::
```

- [ ] **Step 2: Add MenuInteractionSystem**

In `src/game/src/game.cpp`, in the anonymous namespace, immediately **before** `AppFlowSystem`, add:

```cpp
// Hit-tests scoped menu buttons against the UI-space mouse, drives their UIRectComponent.Color
// (Normal/Hover/Press), and on a press-inside -> release-inside click pushes an ActionEvent.
// Runs before AppFlowSystem so the action is consumed the same tick. Scope filtering means it's
// inert in states with no scoped buttons (e.g. InLevel).
class MenuInteractionSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (!in) return;

        GameStateId cur = GameStateId::MainMenu;
        if (const auto* gs = ctx.world.GetSingleton<GameStateComponent>()) cur = gs->Current;

        const auto* vp = ctx.world.GetSingleton<ViewportComponent>();
        const uint32_t ox = vp ? vp->OriginX : 0u;
        const uint32_t oy = vp ? vp->OriginY : 0u;
        const glm::vec2 mouse = ToUiSpace(in->MouseX, in->MouseY, ox, oy);
        const bool down    = in->MouseDown[MOUSE_BUTTON_LEFT];
        const bool pressed = in->MousePressed[MOUSE_BUTTON_LEFT];

        // Ensure the runtime singleton exists, then read the armed button.
        if (!ctx.world.GetSingleton<MenuStateComponent>())
            ctx.world.SetSingleton(MenuStateComponent{});
        EntityId armed = 0;
        if (const auto* ms = ctx.world.GetSingleton<MenuStateComponent>()) armed = ms->ArmedButton;

        auto scopeVisible = [&](EntityId e) {
            const auto* sc = ctx.world.GetComponent<StateScopeComponent>(e);
            return !sc || ScopeAllows(sc->StateMask, cur);
        };
        auto rectOf = [&](EntityId e, glm::vec2& pos, glm::vec2& size) {
            const auto* tr = ctx.world.GetComponent<TransformComponent>(e);
            const auto* rc = ctx.world.GetComponent<UIRectComponent>(e);
            if (!tr || !rc) return false;
            pos = glm::vec2(tr->Position.x, tr->Position.y);
            size = rc->Size;
            return true;
        };

        // 1) Resolve a release of the armed button (armed set on a previous tick, now up).
        uint32_t firedAction = 0; EntityId firedSrc = 0;
        if (armed != 0 && !down) {
            glm::vec2 pos, size;
            if (scopeVisible(armed) && rectOf(armed, pos, size) && PointInRect(mouse, pos, size)) {
                if (const auto* mb = ctx.world.GetComponent<MenuButtonComponent>(armed)) {
                    firedAction = mb->ActionId;
                    firedSrc = armed;
                }
            }
            armed = 0;
        }

        // 2) Hover/press colors + arm-on-press over scoped buttons.
        ctx.world.Each<MenuButtonComponent, UIRectComponent, TransformComponent>([&](EntityId e) {
            if (!scopeVisible(e)) return;
            glm::vec2 pos, size;
            if (!rectOf(e, pos, size)) return;
            const bool inside = PointInRect(mouse, pos, size);
            if (pressed && inside) armed = e;
            const auto* mb = ctx.world.GetComponent<MenuButtonComponent>(e);
            glm::vec4 col = mb->Normal;
            if (inside) col = (down && armed == e) ? mb->Press : mb->Hover;
            ctx.world.Modify<UIRectComponent>(e, [&](UIRectComponent& r){ r.Color = col; });
        });

        // 3) Persist armed state + emit the click action (consumed by AppFlowSystem this tick).
        ctx.world.ModifySingleton<MenuStateComponent>([&](MenuStateComponent& m){ m.ArmedButton = armed; });
        if (firedAction != 0) {
            ctx.world.ModifySingleton<ActionQueueComponent>([&](ActionQueueComponent& q){
                q.Events.push_back(ActionEvent{ firedAction, firedSrc, 0 });
            });
        }
    }
    const char* Name() const override { return "MenuInteractionSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};
```

- [ ] **Step 3: Register it before AppFlowSystem + seed the singleton**

In `GameRegisterSystems`, insert `MenuInteractionSystem` immediately before `AppFlowSystem`:

```cpp
    s->Register(std::make_unique<DayNightSystem>());
    s->Register(std::make_unique<MenuInteractionSystem>());  // emits actions; before AppFlow
    s->Register(std::make_unique<AppFlowSystem>());
```

In `GameUpdate`'s boot block, after `g_GameState->World.SetSingleton(ActionQueueComponent{});`, add:

```cpp
        g_GameState->World.SetSingleton(MenuStateComponent{});
```

- [ ] **Step 4: Build**

Run: `cmake --build --preset msvc-win64-vs2026-community --target ecs editor runtime game`
Expected: builds with no errors.

- [ ] **Step 5: Static verification (no GUI)**

Re-read the final `MenuInteractionSystem` and confirm: (a) registered before `AppFlowSystem`; (b) release-resolution runs before arm-on-press; (c) `Modify<UIRectComponent>` inside the `Each<…>(EntityId)` loop is the safe id-first pattern; (d) `MenuStateComponent` seeded in boot; (e) emits into `ActionQueueComponent`. Report what you verified. (Full GUI smoke after Task 5.)

- [ ] **Step 6: Commit**

```bash
git add src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(menu): MenuInteractionSystem (hover/press + click -> ActionEvent)"
```

---

### Task 5: AppFlowSystem nav dispatch + fallback menu

**Files:**
- Modify: `src/game/src/game.cpp`

- [ ] **Step 1: Rewrite AppFlowSystem to consume Nav actions**

In `src/game/src/game.cpp`, replace the entire `AppFlowSystem` class (the Phase-1 drain-stub + TEMP TAB version) with:

```cpp
// Owns game-state transitions. Consumes Nav-category actions from the ActionQueue (emitted by
// MenuInteractionSystem) and applies them; also returns to the menu on ESC while in-level.
class AppFlowSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (const auto* q = ctx.world.GetSingleton<ActionQueueComponent>()) {
            for (const ActionEvent& e : q->Events) {
                if (CategoryOf(e.ActionId) != ActionCategory::Nav) continue; // owned elsewhere
                if (e.ActionId == Actions::Play)      SetState(ctx, GameStateId::InLevel);
                else if (e.ActionId == Actions::Back) SetState(ctx, GameStateId::MainMenu);
                else if (e.ActionId == Actions::Quit)
                    ctx.world.ModifySingleton<AppControlComponent>([](AppControlComponent& a){ a.QuitRequested = true; });
            }
        }

        // ESC returns to the menu while in-level (replaces the old always-quit ESC binding).
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
        if (in && gs && gs->Current == GameStateId::InLevel && in->Pressed[KEY_ESCAPE])
            SetState(ctx, GameStateId::MainMenu);
    }
    const char* Name() const override { return "AppFlowSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
private:
    static void SetState(SystemContext& ctx, GameStateId s) {
        ctx.world.ModifySingleton<GameStateComponent>([&](GameStateComponent& g){ g.Current = s; });
    }
};
```

- [ ] **Step 2: Remove QuitRequestSystem from registration**

In `GameRegisterSystems`, delete the line:

```cpp
    s->Register(std::make_unique<QuitRequestSystem>(KEY_ESCAPE));
```

(The `QuitRequestSystem` class itself may remain defined but unused; leave it — removing the class is optional cleanup. ESC is now handled by `AppFlowSystem`; Quit by the menu button.)

- [ ] **Step 3: Spawn a fallback menu when no world is loaded**

In `GameUpdate`'s boot block, inside the `if (!g_GameState->WorldLoaded) { … }` branch, **replace** the `textEntity` ("Hello, Game!") spawn (the `const auto textEntity = …` + its two `AddComponent` lines) with the fallback menu below. Keep the `sun` and `pointLight` spawns that follow it (the 3D scene behind the menu).

```cpp
            // Fallback main menu (no world.json): title + Play/Quit buttons, scoped to MainMenu,
            // so the menu is usable without authoring. Mirrors what you'd author in the editor.
            const uint32_t menuScope = 1u << static_cast<uint32_t>(GameStateId::MainMenu);

            const auto title = g_GameState->World.CreateEntity();
            g_GameState->World.AddComponent(title, TransformComponent{.Position = glm::vec3{200.f, 140.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}});
            g_GameState->World.AddComponent(title, TextComponent{.Text = "My Game", .Color = glm::vec4{1.0f}, .FontSize = 64});
            g_GameState->World.AddComponent(title, StateScopeComponent{ .StateMask = menuScope });

            auto spawnButton = [&](float x, float y, const char* label, uint32_t action) {
                const auto e = g_GameState->World.CreateEntity();
                g_GameState->World.AddComponent(e, TransformComponent{.Position = glm::vec3{x, y, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}});
                g_GameState->World.AddComponent(e, UIRectComponent{ .Size = glm::vec2{220.f, 56.f} });
                g_GameState->World.AddComponent(e, TextComponent{.Text = label, .Color = glm::vec4{1.0f}, .FontSize = 28});
                g_GameState->World.AddComponent(e, MenuButtonComponent{ .ActionId = action });
                g_GameState->World.AddComponent(e, StateScopeComponent{ .StateMask = menuScope });
            };
            spawnButton(200.f, 260.f, "Play", Actions::Play);
            spawnButton(200.f, 330.f, "Quit", Actions::Quit);
```

(`Actions::Play`/`Quit` resolve via the `#include "Actions.h"` added in Task 4. `MenuButtonComponent`/`UIRectComponent`/`StateScopeComponent` are designated-initializer-constructed like the surrounding spawns.)

- [ ] **Step 4: Build**

Run: `cmake --build --preset msvc-win64-vs2026-community --target ecs editor runtime game`
Expected: builds with no errors.

- [ ] **Step 5: Manual smoke (human — restart editor; ECS.h + GAME_API changed in Task 1)**

Restart the editor. In **Play mode** (so the game receives mouse input — see the input-routing note) **or run `runtime.exe`**:
- Boot to **MainMenu**: a "My Game" title + **Play** and **Quit** buttons (dark rects) appear; the 3D scene is behind them; gameplay is frozen.
- **Hover** a button → it lightens (Hover color); **press-hold** → darkens (Press); release inside → fires.
- Click **Play** → state → **InLevel**: the menu disappears (scoped to MainMenu), gameplay resumes (WASD/zoom/camera).
- Press **ESC** in-level → back to **MainMenu** (menu reappears, gameplay frozen).
- Click **Quit** in the menu → the app exits.
- **Editor authoring**: select an entity, Add UI Rect + Text + Menu Button + State Scope(MainMenu); set the Menu Button's Action to a value via the combo; Save; reload → the authored button persists and works. Duplicate an authored button → the copy keeps its rect/scope/button.
- Verify on **DX12 and Vulkan**.

- [ ] **Step 6: Commit**

```bash
git add src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(menu): AppFlowSystem nav dispatch (Play/Quit/Back+ESC) + fallback menu"
```

---

## Final verification

- [ ] `test_ecs.exe` → `All ECS tests passed.`
- [ ] `test_menu.exe` → `All menu tests passed.`
- [ ] `test_worldserial.exe` → `All world-serialization tests passed.`
- [ ] Human smoke per Task 5 Step 5 (DX12 + Vulkan, in Play mode / runtime.exe).

## Self-review notes (vs. spec §5-§10, §15 Phase 4)

- Covers: `Actions.h` (categories + Nav ids); `MenuButtonComponent` (authored: ActionId + 3 colors) + `MenuStateComponent` (runtime click-latch singleton); `PointInRect` pure helper + test; `MenuInteractionSystem` (scope-filtered hit-test → Normal/Hover/Press + press→release click-latch → `ActionEvent` into `ActionQueue`); `AppFlowSystem` rewrite consuming Nav actions (Play→InLevel, Quit→QuitRequested, Back→MainMenu) + ESC→MainMenu in-level; `QuitRequestSystem` removed from registration; fallback code-spawned menu (§10); serialization + inspector (ActionId combo) + DuplicateEntityComponents. GAME_API bump.
- The action layer is event-driven per §5: the button emits an `ActionEvent`; the owning system (`AppFlowSystem`, category `Nav`) consumes it — no god-switch; new categories add their own systems later. `CategoryOf` routes.
- Click-latch (§7): press-inside arms (`MenuStateComponent.ArmedButton`), release-inside fires; drag-off cancels. Resolution runs before arm so a same-tick press+release fires the next tick (benign).
- TEMP TAB toggle (Phase 1) removed; state now driven entirely by actions + ESC.
- Cosmetic note: a button's `TextComponent` label shares the entity's `TransformComponent` with its `UIRectComponent`, so the label is positioned at the rect's top-left (text baseline) rather than centered — acceptable for the prototype; precise centering is future polish (would need per-glyph layout or a label offset).
