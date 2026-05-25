# State-Gated Menu — Phase 3 (Authorable UI Primitives) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let designers author screen-space UI rectangles in the editor and scope any entity to specific game states, with both persisted in `world.json` and the renderer showing scoped entities only in their state.

**Architecture:** Two new authorable ECS components — `UIRectComponent` (solid quad) and `StateScopeComponent` (state bitmask) — are editor-editable (ECSCommands), persisted (ComponentSerialization + WorldManager), and inspectable. `UiRenderPass` emits `UIRectComponent` entities as solid-color instances (the shader's existing `SAMPLE_TEXTURE`-off path) before text glyphs, and filters both rects and text by `ScopeAllows(scope, GameStateComponent.Current)` read from the snapshot.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), NVRHI UI pass (instanced quads), nlohmann/json, Dear ImGui editor, CMake (`msvc-win64-vs2026-community`).

**Spec:** `docs/superpowers/specs/2026-05-25-state-gated-menu-design.md` (§2, §4, §6, §15 Phase 3). Builds on Phases 1-2 (already on this branch).

---

## Reference facts (verified against the codebase)

- `ScopeAllows(uint32_t mask, GameStateId cur)` exists in `src/common/include/StateScope.h` (Phase 1): `mask==0 || (mask & (1u<<uint32_t(cur)))`.
- `GameStateComponent { GameStateId Current; }` singleton exists (Phase 1), in the ECS snapshot. `GameStateId { Uninitialized=0, MainMenu=1, InLevel=2, InEditor=3, Paused=4 }`.
- `UiRenderPass.cpp`: instance type `UIInstanceCPU { glm::mat4 Transform; glm::vec4 Color; glm::vec4 UVRect; uint32_t Flags; uint32_t _pad[3]; }`; `Flags = 1u<<0` = SAMPLE_TEXTURE (PS samples the atlas), `Flags = 0` = solid `Color`. Static unit quad VB `[0,1]²`, IB 6 indices (`m_IndexCount=6`). Text path: per font size, build `UIInstanceCPU` per glyph (`Transform = T*R*S * localGlyphTransform`, `Flags=SAMPLE_TEXTURE`), `writeBuffer(m_InstanceBuffer, …)`, bind `atlas->uiBindingSet`, `drawIndexed(instanceCount=out)`. The default font atlas + its `uiBindingSet` are created in `Initialize` (`FontManager::DEFAULT_FONT`). UI ortho (`UICameraComponent`) is top-left pixel space.
- ECSCommands (`src/common/include/ECSCommands.h`): `ApplyComponentCommand` has a per-type `else if (componentData.Type == std::type_index(typeid(T))) { if (auto* p = componentData.Get<T>()) world.AddComponent(entity, *p); }` (handles Add + Modify); `RemoveComponentByType` has `else if (typeIndex == std::type_index(typeid(T))) world.RemoveComponent<T>(entity);`. PlayerComponent is the latest example (~lines 263, 291).
- Serialization (`src/common/include/ComponentSerialization.h`): free `to_json`/`from_json` per component (PlayerComponent at ~line 118); has `glm::vec3`/`glm::vec4` (de)serializers but **no `glm::vec2`** — serialize `Size` inline as `{"X":…,"Y":…}`. `glm::vec4` free `to_json` writes `{"R","G","B","A"}`.
- WorldManager (`src/engine/src/utilities/WorldManager.cpp`): save loop `if (world->HasComponent<T>(entity)) jEntity["T"] = *world->GetComponent<T>(entity);` (~line 51); load loop `if (jEntity.contains("T")) world->AddComponent(createdEntity, jEntity["T"].get<T>());` (~line 109).
- Inspector (`src/editor/src/rendering/imgui/EcsInspectorPanel.cpp`): add-menu (`if (!HasComponent) if (MenuItem("Add …")) push ECSCommand::AddComponent`), remove-menu (`ECSCommand::RemoveComponent<T>`), edit-header (`CollapsingHeader` + `edit<T>`/`lastEdited<T>Entity`/`<t>Modified` members + `ECSCommand::ModifyComponent`). PlayerComponent at ~lines 170, 236, 645. Members declared in `EcsInspectorPanel.h`.
- `game.h`: `GAME_API_VERSION 10u` (after Phase 2).

> **ECS.h + ECSCommands.h change → rebuild `ecs`+`editor`+`runtime`+`game` + restart editor once.** `GAME_API_VERSION` → `11u` (Task 1).
> **Editor input routing reminder:** menus render in the editor regardless of state, but game UI is visible per the render filter. (Clicks aren't wired until Phase 4; routing only matters then.)

---

## File Structure

- `src/common/include/ECS.h` — `UIRectComponent`, `StateScopeComponent` + 2 X-macro entries.
- `src/common/include/ECSCommands.h` — Apply + Remove branches for both.
- `src/common/include/ComponentSerialization.h` — `to_json`/`from_json` for both.
- `src/engine/src/utilities/WorldManager.cpp` — save/load branches for both.
- `src/engine/src/rendering/passes/UiRenderPass.cpp` — solid-rect emission + scope filter (rects + text).
- `src/editor/src/rendering/imgui/EcsInspectorPanel.{h,cpp}` — add/remove/edit for both.
- `src/game/include/game.h` — `GAME_API_VERSION` 10→11.
- `tests/test_worldserial.cpp` — round-trip cases for both.

---

### Task 1: Components + X-macro + ECSCommands + version bump

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/common/include/ECSCommands.h`
- Modify: `src/game/include/game.h`

- [ ] **Step 1: Add the two component structs**

In `src/common/include/ECS.h`, immediately after the `struct ViewportComponent { … };` line (line ~173), add:

```cpp
// Solid screen-space UI quad (authorable). Positioned by TransformComponent (top-left, pixels);
// rendered by UiRenderPass via the UI shader's solid-color path. Size is in pixels.
struct UIRectComponent {
    glm::vec2 Size{160.0f, 48.0f};
    glm::vec4 Color{0.15f, 0.15f, 0.18f, 1.0f};
};

// Scopes an entity to one or more game states (bit i = GameStateId value i; 0 = always-on).
// The UI renderer + menu interaction only act on entities whose scope allows the current state.
struct StateScopeComponent {
    uint32_t StateMask = 0;
};
```

- [ ] **Step 2: Register both in the X-macro**

In `src/common/include/ECS.h`, change the tail of `ECS_FOR_EACH_REGISTERED_COMPONENT` from:

```cpp
    X(GameStateComponent) \
    X(ActionQueueComponent)
```

to:

```cpp
    X(GameStateComponent) \
    X(ActionQueueComponent) \
    X(UIRectComponent) \
    X(StateScopeComponent)
```

- [ ] **Step 3: Register both in ECSCommands (Apply + Remove)**

In `src/common/include/ECSCommands.h`, in `ApplyComponentCommand`, after the `PlayerComponent` add-branch (the `else if (componentData.Type == std::type_index(typeid(PlayerComponent))) { … }`), add:

```cpp
        } else if (componentData.Type == std::type_index(typeid(UIRectComponent))) {
            if (auto* rect = componentData.Get<UIRectComponent>()) {
                world.AddComponent(entity, *rect);
            }
        } else if (componentData.Type == std::type_index(typeid(StateScopeComponent))) {
            if (auto* scope = componentData.Get<StateScopeComponent>()) {
                world.AddComponent(entity, *scope);
            }
```

In `RemoveComponentByType`, after the `PlayerComponent` remove-branch (`else if (typeIndex == std::type_index(typeid(PlayerComponent))) world.RemoveComponent<PlayerComponent>(entity);`), add:

```cpp
        } else if (typeIndex == std::type_index(typeid(UIRectComponent))) {
            world.RemoveComponent<UIRectComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(StateScopeComponent))) {
            world.RemoveComponent<StateScopeComponent>(entity);
```

(Place each new `} else if` so it chains correctly with the surrounding `if/else if` ladder — i.e. insert it before the final closing `}` / trailing comment of each ladder.)

- [ ] **Step 4: Bump the game API version**

In `src/game/include/game.h`, change `#define GAME_API_VERSION 10u` to `#define GAME_API_VERSION 11u`.

- [ ] **Step 5: Configure + build**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target ecs editor runtime game
```
Expected: all build with no errors.

- [ ] **Step 6: Verify the ECS test still passes**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

- [ ] **Step 7: Commit**

```bash
git add src/common/include/ECS.h src/common/include/ECSCommands.h src/game/include/game.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ui): add UIRectComponent + StateScopeComponent (ECS + command ring)"
```

---

### Task 2: Serialization round-trip (TDD)

**Files:**
- Modify: `src/common/include/ComponentSerialization.h`
- Modify: `src/engine/src/utilities/WorldManager.cpp`
- Modify: `tests/test_worldserial.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_worldserial.cpp`, add after the existing `T06_player_roundtrip()` function:

```cpp
static void T07_uirect_roundtrip()
{
    UIRectComponent in;
    in.Size = glm::vec2(123.0f, 45.0f);
    in.Color = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    const nlohmann::json j = in;
    const auto out = j.get<UIRectComponent>();
    EXPECT(near(out.Size.x, in.Size.x) && near(out.Size.y, in.Size.y));
    EXPECT(veq(glm::vec3(out.Color), glm::vec3(in.Color)) && near(out.Color.a, in.Color.a));
}

static void T08_statescope_roundtrip()
{
    StateScopeComponent in;
    in.StateMask = (1u << 1) | (1u << 4); // MainMenu | Paused
    const nlohmann::json j = in;
    const auto out = j.get<StateScopeComponent>();
    EXPECT(out.StateMask == in.StateMask);
}
```

And call them in `main()` after `T06_player_roundtrip();`:

```cpp
    T07_uirect_roundtrip();
    T08_statescope_roundtrip();
```

- [ ] **Step 2: Configure + build, verify it FAILS**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
```
Expected: compile error — no `to_json`/`from_json` for `UIRectComponent`/`StateScopeComponent` (json conversion fails to resolve).

- [ ] **Step 3: Add the serializers**

In `src/common/include/ComponentSerialization.h`, after the `PlayerComponent` `from_json` (~line 123), add:

```cpp
inline void to_json(nlohmann::json& j, const UIRectComponent& t) {
    j = nlohmann::json{
        {"Size", {{"X", t.Size.x}, {"Y", t.Size.y}}},
        {"Color", t.Color}   // glm::vec4 round-trips via its registered nlohmann serializer
    };
}
inline void from_json(const nlohmann::json& j, UIRectComponent& t) {
    t.Size.x = j.at("Size").at("X").get<float>();
    t.Size.y = j.at("Size").at("Y").get<float>();
    j.at("Color").get_to(t.Color);
}

inline void to_json(nlohmann::json& j, const StateScopeComponent& t) {
    j = nlohmann::json{{"StateMask", t.StateMask}};
}
inline void from_json(const nlohmann::json& j, StateScopeComponent& t) {
    j.at("StateMask").get_to(t.StateMask);
}
```

- [ ] **Step 4: Add WorldManager save + load branches**

In `src/engine/src/utilities/WorldManager.cpp`, in the per-entity **save** loop, after the `PlayerComponent` save block (~line 53), add:

```cpp
        if (world->HasComponent<UIRectComponent>(entity)) {
            jEntity["UIRectComponent"] = *(world->GetComponent<UIRectComponent>(entity));
        }
        if (world->HasComponent<StateScopeComponent>(entity)) {
            jEntity["StateScopeComponent"] = *(world->GetComponent<StateScopeComponent>(entity));
        }
```

In the per-entity **load** loop, after the `PlayerComponent` load line (~line 110), add:

```cpp
            if (jEntity.contains("UIRectComponent"))
                world->AddComponent(createdEntity, jEntity["UIRectComponent"].get<UIRectComponent>());
            if (jEntity.contains("StateScopeComponent"))
                world->AddComponent(createdEntity, jEntity["StateScopeComponent"].get<StateScopeComponent>());
```

- [ ] **Step 5: Build + run the test, verify it PASSES**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: `All world-serialization tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/common/include/ComponentSerialization.h src/engine/src/utilities/WorldManager.cpp tests/test_worldserial.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ui): persist UIRectComponent + StateScopeComponent in world.json"
```

---

### Task 3: Render solid rects + scope-filter the UI pass

**Files:**
- Modify: `src/engine/src/rendering/passes/UiRenderPass.cpp`

- [ ] **Step 1: Include the scope helper**

At the top of `src/engine/src/rendering/passes/UiRenderPass.cpp`, with the other includes, add:

```cpp
#include "StateScope.h" // ScopeAllows
```

- [ ] **Step 2: Resolve the current state once + a scope-check lambda**

In `UiRenderPass::Render`, inside the `if (world) {` block, **before** the text-entity gather (before `auto* textEnts = …`), add:

```cpp
        // Current state drives scope filtering (entities with a StateScopeComponent only render
        // when their mask allows the current state; unscoped entities always render).
        GameStateId uiState = GameStateId::MainMenu;
        if (const auto* gs = world->GetSingleton<GameStateComponent>()) uiState = gs->Current;
        auto scopeVisible = [&](EntityId e) -> bool {
            const auto* sc = world->GetComponent<StateScopeComponent>(e);
            return !sc || ScopeAllows(sc->StateMask, uiState);
        };
```

- [ ] **Step 3: Emit solid rects before text**

In `UiRenderPass::Render`, immediately after the `scopeVisible` lambda (still inside `if (world)`, before the text gather), add:

```cpp
        // Solid UI rectangles (drawn before text so labels sit on top). One instanced draw
        // using the default font atlas's binding set (its texture is bound but unused since
        // Flags=0 => the PS outputs solid Color).
        if (auto* rectAtlas = m_FontManager.GetAtlas(FontManager::DEFAULT_FONT, m_Device, commandList);
            rectAtlas && rectAtlas->uiBindingSet) {
            auto* rectEnts = frameAllocator->AllocateArray<EntityId>(world->GetEntityCount());
            uint32_t rectCount = 0;
            if (rectEnts) {
                world->Each<UIRectComponent, TransformComponent>([&](EntityId e){
                    if (scopeVisible(e)) rectEnts[rectCount++] = e;
                });
            }
            uint32_t rectN = std::min(rectCount, m_MaxInstances);
            if (rectN > 0) {
                auto* rectInstances = frameAllocator->AllocateArray<UIInstanceCPU>(rectN);
                if (rectInstances) {
                    uint32_t out = 0;
                    for (uint32_t ri = 0; ri < rectCount && out < rectN; ++ri) {
                        const EntityId e = rectEnts[ri];
                        const auto* tr = world->GetComponent<TransformComponent>(e);
                        const auto* rc = world->GetComponent<UIRectComponent>(e);
                        if (!tr || !rc) continue;
                        const glm::mat4 T = glm::translate(glm::mat4(1.0f), tr->Position);
                        const glm::mat4 R = glm::rotate(glm::mat4(1.0f), tr->Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
                        const glm::mat4 S = glm::scale(glm::mat4(1.0f), glm::vec3(rc->Size.x, rc->Size.y, 1.0f));
                        UIInstanceCPU inst{};
                        inst.Transform = T * R * S;   // unit quad [0,1] -> [pos, pos+Size]
                        inst.Color = rc->Color;
                        inst.UVRect = glm::vec4(0.0f);
                        inst.Flags = 0u;              // solid color (no atlas sample)
                        rectInstances[out++] = inst;
                    }
                    if (out > 0) {
                        commandList->writeBuffer(m_InstanceBuffer, rectInstances, out * sizeof(UIInstanceCPU));
                        state.bindings = { rectAtlas->uiBindingSet };
                        commandList->setGraphicsState(state);
                        nvrhi::DrawArguments rectArgs;
                        rectArgs.vertexCount = m_IndexCount;
                        rectArgs.instanceCount = out;
                        rectArgs.startIndexLocation = 0;
                        rectArgs.startVertexLocation = 0;
                        commandList->drawIndexed(rectArgs);
                    }
                }
            }
        }
```

- [ ] **Step 4: Scope-filter the text glyph generation**

In `UiRenderPass::Render`, in the glyph-instance generation loop, find the line:

```cpp
                if (transform && text && text->FontSize == fontSize) {
```

and change it to also require scope visibility:

```cpp
                if (transform && text && text->FontSize == fontSize && scopeVisible(entity)) {
```

(The per-font glyph **count** loop above may stay unfiltered — it is only an upper bound on the allocation; the generation loop's filter is what actually omits scoped-out text.)

- [ ] **Step 5: Build**

Run: `cmake --build --preset msvc-win64-vs2026-community --target ecs editor runtime game`
Expected: builds with no errors.

- [ ] **Step 6: Manual smoke — SKIP (no GUI here)**

Static re-read confirms: (a) the rect draw is emitted before the text font-size loop; (b) rects + text both gated by `scopeVisible`; (c) `Flags=0` for rects, `SAMPLE_TEXTURE` unchanged for glyphs; (d) the rect draw reuses `m_InstanceBuffer` + the default atlas binding set. Report what you verified. (Human GUI-smokes after Task 4 once rects are authorable.)

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/rendering/passes/UiRenderPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ui): render UIRectComponent solid quads + scope-filter the UI pass"
```

---

### Task 4: Editor inspector — add/remove/edit for both components

**Files:**
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.h`
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp`

- [ ] **Step 1: Add edit-state members**

In `src/editor/src/rendering/imgui/EcsInspectorPanel.h`, next to the existing `editPlayer` / `lastEditedPlayerEntity` / `playerModified` members, add:

```cpp
    UIRectComponent editUIRect{};
    EntityId        lastEditedUIRectEntity = 0;
    bool            uiRectModified = false;

    StateScopeComponent editScope{};
    EntityId            lastEditedScopeEntity = 0;
    bool                scopeModified = false;
```

- [ ] **Step 2: Add the "Add Component" menu items**

In `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp`, after the `Add Player Component` menu block (~line 175), add:

```cpp
                if (!ctx.WorldSnapshot->HasComponent<UIRectComponent>(entity)) {
                    if (ImGui::MenuItem("Add UI Rect Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, UIRectComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }
                if (!ctx.WorldSnapshot->HasComponent<StateScopeComponent>(entity)) {
                    if (ImGui::MenuItem("Add State Scope Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, StateScopeComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }
```

- [ ] **Step 3: Add the "Remove Component" menu items**

In the same file, after the `Remove Player Component` menu block (~line 241), add:

```cpp
                if (ctx.WorldSnapshot->HasComponent<UIRectComponent>(entity)) {
                    if (ImGui::MenuItem("Remove UI Rect Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<UIRectComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }
                if (ctx.WorldSnapshot->HasComponent<StateScopeComponent>(entity)) {
                    if (ImGui::MenuItem("Remove State Scope Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<StateScopeComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }
```

- [ ] **Step 4: Add the edit headers**

In the same file, after the `Edit Player Component` collapsing-header block (~line 669), add:

```cpp
            // Edit UI Rect Component
            if (ctx.WorldSnapshot->HasComponent<UIRectComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("UI Rect Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* rect = ctx.WorldSnapshot->GetComponent<UIRectComponent>(selectedEntity);
                    if (rect) {
                        if (lastEditedUIRectEntity != selectedEntity) {
                            editUIRect = *rect;
                            lastEditedUIRectEntity = selectedEntity;
                            uiRectModified = false;
                        }
                        if (!uiRectModified) {
                            editUIRect = *rect;
                        }
                        if (ImGui::DragFloat2("Size (px)", &editUIRect.Size.x, 1.0f, 1.0f, 4096.0f, "%.0f")) {
                            uiRectModified = true;
                        }
                        if (ImGui::ColorEdit4("Color##UIRect", &editUIRect.Color.x)) {
                            uiRectModified = true;
                        }
                        ImGui::Spacing();
                        if (uiRectModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editUIRect);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            uiRectModified = false;
                        }
                    }
                }
            }

            // Edit State Scope Component (one checkbox per state; mask bit i = GameStateId i)
            if (ctx.WorldSnapshot->HasComponent<StateScopeComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("State Scope Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* scope = ctx.WorldSnapshot->GetComponent<StateScopeComponent>(selectedEntity);
                    if (scope) {
                        if (lastEditedScopeEntity != selectedEntity) {
                            editScope = *scope;
                            lastEditedScopeEntity = selectedEntity;
                            scopeModified = false;
                        }
                        if (!scopeModified) {
                            editScope = *scope;
                        }
                        ImGui::TextDisabled("Active in states (none = always):");
                        struct { const char* label; GameStateId id; } kStates[] = {
                            {"Main Menu", GameStateId::MainMenu},
                            {"In Level",  GameStateId::InLevel},
                            {"In Editor", GameStateId::InEditor},
                            {"Paused",    GameStateId::Paused},
                        };
                        for (const auto& s : kStates) {
                            const uint32_t bit = 1u << static_cast<uint32_t>(s.id);
                            bool on = (editScope.StateMask & bit) != 0u;
                            if (ImGui::Checkbox(s.label, &on)) {
                                if (on) editScope.StateMask |= bit; else editScope.StateMask &= ~bit;
                                scopeModified = true;
                            }
                        }
                        ImGui::Spacing();
                        if (scopeModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editScope);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            scopeModified = false;
                        }
                    }
                }
            }
```

- [ ] **Step 5: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds with no errors.

- [ ] **Step 6: Manual smoke (human — restart editor; ECS.h + GAME_API changed in Task 1)**

Restart the editor. Then:
- Select an entity → right-click → **Add UI Rect Component** + add a `TransformComponent` if absent. Set its Transform position to e.g. `(200, 200, 0)`. A solid rectangle appears at that screen position. Edit Size/Color in the inspector → updates live (1-2 frames).
- **Add State Scope Component** → check only **In Level**. The rect now renders only when the game is in InLevel (press **TAB** — Phase-1 toggle — to switch states and watch it appear/disappear). Uncheck all (always-on) → always renders.
- **Save** the scene, reload (or restart) → the rect + its scope persist (world.json round-trip).
- Verify on **DX12 and Vulkan** (UI pass now emits solid quads — exercise the `Flags=0` path on both).

- [ ] **Step 7: Commit**

```bash
git add src/editor/src/rendering/imgui/EcsInspectorPanel.h src/editor/src/rendering/imgui/EcsInspectorPanel.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): inspector add/remove/edit for UIRect + StateScope"
```

---

## Final verification

- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` → `All ECS tests passed.`
- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe` → `All world-serialization tests passed.`
- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe` → `All menu tests passed.` (no regression)
- [ ] Human smoke per Task 4 Step 6 (DX12 + Vulkan).

## Self-review notes (vs. spec §2/§4/§6/§15 Phase 3)

- Covers: `UIRectComponent` + `StateScopeComponent` (ECS.h + X-macro) + ECSCommands (editor-editable); Ui pass solid-quad emission (`Flags=0`, before text) + scope filter on rects AND text via `ScopeAllows(scope, GameStateComponent.Current)` from the snapshot; serialization round-trip (ComponentSerialization + WorldManager) + test; inspector add/remove/edit (StateScope as per-state checkboxes). GAME_API bump.
- Deferred (correctly absent): `MenuButtonComponent`, `Actions.h`, `ActionEvent` producers, `MenuInteractionSystem`/`AppFlowSystem` action routing, `PointInRect`/click-latch (all Phase 4).
- `Size` is serialized inline (`{"X","Y"}`) since there's no `glm::vec2` (de)serializer; `Color` reuses the existing `glm::vec4` one.
- Rect transform ignores `TransformComponent.Scale` (UIRectComponent.Size is the authoritative pixel size) — matches the "Size in pixels" intent; rotation via `Rotation.z` is honored for parity with text.
