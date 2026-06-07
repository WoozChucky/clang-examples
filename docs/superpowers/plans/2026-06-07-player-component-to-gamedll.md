# Migrate PlayerComponent to Game.dll Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `PlayerComponent` from an engine builtin to a `Game.dll`-owned non-builtin component — surviving hot-reload (byte path), edited via a game-provided `editorDraw` hook, and copied on entity-duplicate via a new registry-driven path that fixes duplication for all game components.

**Architecture:** First make entity duplication registry-driven (a new `copyTo` fn-ptr on the serializer entry) — behavior-preserving because the serializer registry's registered set is exactly the old curated duplicate list. Then atomically move `PlayerComponent` out of `ECS.h`/`src/common` into a new `Game.dll` header, register it in `GameRegisterComponents` (with an `editorDraw` hook), delete the dedicated `PlayerEditor`, and remove the now-impossible typed command branches.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-enterprise`), CMake, nlohmann/json. Builds on the hot-reload-survival feature (`PreserveNonBuiltinComponents`/`GameRegisterComponents`) and the editor-hook feature (`RegisterEditorHook`/`EditorUI`).

**Scope:** Implements `docs/superpowers/specs/2026-06-07-player-component-to-gamedll-design.md`. Touches `ECS.h` (X-macro) ⇒ rebuild **ecs + Engine + editor + game** and **restart the editor**. No `GAME_API_VERSION` bump (GameState layout + export signatures unchanged).

---

## Background facts (verified)

- `PlayerComponent` is defined in `src/common/include/ECS.h:217-220` and in the X-macro at `ECS.h:394` (`X(PlayerComponent)`). Its `to_json`/`from_json` are at `src/common/include/ComponentSerialization.h:130-135`. Its builtin serializer registration is `src/ecs/src/ComponentSerializers.cpp:16` (`r.Register<PlayerComponent>("PlayerComponent", true);`).
- **Only `src/game/src/game.cpp` uses `PlayerComponent` in `src/game`** (verified via grep). `src/game/src/PlayerMovement.{h,cpp}` and `tests/test_playermove.cpp` do NOT reference `PlayerComponent` (they use `ComputePlanarMove` + `InputStateComponent`). So `test_playermove` needs no edit — only a build confirmation.
- **No engine reference to `PlayerComponent`** (verified): only `ecs.dll` (`ECS.h`, `ComponentSerializers.cpp`) and `editor` (`PlayerEditor.{cpp,h}`) and `src/common/ECSCommands.h` reference it.
- `ECSCommands.h` references: `DuplicateEntityComponents` curated copy list (`:299-317`, Player at `:305`), `ApplyComponentCommand` typed branch (`:356-359`), `RemoveComponentByType` typed branch (`:426-427`).
- **The registry's registered set == the duplicate set.** `ComponentSerializers.cpp` registers exactly these 17 per-entity types: Transform, Mesh, Material, Lightning, Text, SunMarker, Player, UIRect, StateScope, MenuButton, Collider, NavMeshSource, NavObstacle, NavAgent, NavTarget, NavConstrained, NavClass. `DuplicateEntityComponents` copies the same 17. Singletons (DayNight/Fog/Sky/NavMeshConfig) and hierarchy (Parent/Child) are NOT registered, so a registry-driven duplicate naturally excludes them — **behavior-preserving**.
- Editor wiring: `PlayerEditor` in `src/editor/CMakeLists.txt:35`, included at `EcsInspectorPanel.cpp:20`, registered at `EcsInspectorPanel.cpp:40`. Non-builtin components route through `EcsInspectorPanel.cpp:203-206` (the `if (en.builtin ...) continue;` loop) → `m_GenericEditor.Draw` → `entry->editorDraw` hook when present.
- `GameRegisterComponents()` is an empty stub at `src/game/src/game.cpp:927` (from the hot-reload feature). `game.cpp` already includes `ComponentSerializerRegistry.h` (`:3`).
- `EditorUI` bridge (`src/common/include/EditorUI.h`) has `bool (*DragFloat)(nlohmann::json& obj, const char* key, float speed);`. `RegisterEditorHook(name, fn)` is on the registry.
- `ComponentSerializerEntry` member order (after the hot-reload feature): `name, has, save, load, addDefault, remove, builtin, editorDraw, reloadExtract, reloadIngest`. `Register<T>` value-inits `e{}` then field-assigns.
- `ECS` API: `CreateEntity()`, `GetComponent<T>(EntityId) const` → `const T*`, `AddComponent<T>(EntityId, T)`, `HasComponent<T>`, `RemoveComponent<T>`.

## Type/symbol contract (keep exact)

- New entry member (LAST member of `ComponentSerializerEntry`): `void (*copyTo)(ECS&, EntityId src, EntityId dst) = nullptr;`.
- New header `src/game/src/PlayerComponent.h`: `struct PlayerComponent { float MoveSpeed = 5.0f; };` + ADL `to_json`/`from_json`.

---

### Task 1: Registry-driven entity duplication (`copyTo`) + unit test

**Files:**
- Modify: `src/common/include/ComponentSerializerRegistry.h`
- Modify: `src/common/include/ECSCommands.h`
- Modify: `tests/test_compserial.cpp`

- [ ] **Step 1: Write the failing test (`tests/test_compserial.cpp`)**

Add after `T08_preservation_strategy_selection` (uses the existing `PersistProbe`):
```cpp
static void T09_copyto_duplicates_component()
{
    SerializerRegistry().Register<PersistProbe>("PersistProbe");
    const auto* e = SerializerRegistry().Find("PersistProbe");
    EXPECT(e && e->copyTo != nullptr);

    ECS w;
    const EntityId a = w.CreateEntity();
    const EntityId b = w.CreateEntity();
    w.AddComponent<PersistProbe>(a, PersistProbe{ 5, 2.0f });
    e->copyTo(w, a, b);
    const PersistProbe* gb = w.GetComponent<PersistProbe>(b);
    EXPECT(gb && gb->A == 5 && std::fabs(gb->B - 2.0f) < 1e-6f);

    // copyTo where src lacks the component is a no-op.
    const EntityId c = w.CreateEntity();
    const EntityId d = w.CreateEntity();
    e->copyTo(w, c, d);
    EXPECT(!w.HasComponent<PersistProbe>(d));
}
```
Register `T09_copyto_duplicates_component();` in `main()` after `T08_...`.

- [ ] **Step 2: Build — expect RED**
```
cmake --build --preset msvc-win64-vs2026-enterprise --target test_compserial
```
Expected: compile error — `copyTo` is not a member of `ComponentSerializerEntry`.

- [ ] **Step 3: Add the `copyTo` member (`ComponentSerializerRegistry.h`)**

Add as the LAST member of `ComponentSerializerEntry` (after `reloadIngest`):
```cpp
    // Deep-copy this component from one entity to another within the SAME world (entity duplicate).
    // Installed for every registered type (all components are copy-constructible). No-op if src lacks it.
    void (*copyTo)(ECS&, EntityId src, EntityId dst) = nullptr;
```

- [ ] **Step 4: Install `copyTo` in `Register<T>` (`ComponentSerializerRegistry.h`)**

In `Register<T>`, after the `if constexpr (std::is_trivially_copyable_v<T>) { ... }` block and before the upsert loop, add:
```cpp
        e.copyTo = [](ECS& w, EntityId src, EntityId dst) {
            if (const T* p = w.GetComponent<T>(src)) w.AddComponent<T>(dst, *p);
        };
```

- [ ] **Step 5: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs --target test_compserial
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_compserial.exe
```
Expected: `All component-serializer tests passed.`

- [ ] **Step 6: Make `DuplicateEntityComponents` registry-driven (`ECSCommands.h`)**

First confirm `ECSCommands.h` includes the registry header. Near the top includes, ensure `#include "ComponentSerializerRegistry.h"` is present (add it if absent).

Replace the entire `DuplicateEntityComponents` body (`:296-317`):
```cpp
    // Copy the editor-facing per-entity components from src to dst. Deliberately excludes
    // singletons (cameras/viewport/input/atmosphere/game-state/action-queue) and hierarchy
    // (Parent/Child). Keep in sync when a new authorable per-entity component is added.
    static void DuplicateEntityComponents(ECS& world, EntityId src, EntityId dst) {
        if (auto* c = world.GetComponent<TransformComponent>(src))  world.AddComponent(dst, *c);
        ... (long typed list) ...
        if (world.HasComponent<SunMarker>(src))                     world.AddComponent(dst, SunMarker{});
    }
```
with:
```cpp
    // Copy the editor-facing per-entity components from src to dst via the serializer registry.
    // The registry holds exactly the persisted per-entity types — singletons (cameras/viewport/
    // input/atmosphere/game-state/action-queue/navmesh-config) and hierarchy (Parent/Child) are
    // not registered, so they are naturally excluded. Game-defined components are copied too.
    static void DuplicateEntityComponents(ECS& world, EntityId src, EntityId dst) {
        for (const auto& entry : SerializerRegistry().Entries())
            if (entry.copyTo) entry.copyTo(world, src, dst);
    }
```

- [ ] **Step 7: Build + run — GREEN (ecs + duplicate path compiles)**
```
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs --target test_compserial --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```
Expected: both print their pass lines. (`PlayerComponent` is still builtin+registered here, so duplicate still copies it — no regression.)

- [ ] **Step 8: Commit**
```
git -C C:/dev/personal/clang-examples add src/common/include/ComponentSerializerRegistry.h src/common/include/ECSCommands.h tests/test_compserial.cpp
git -C C:/dev/personal/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(ecs): registry-driven entity duplication via copyTo fn-ptr"
```
Verify exactly those three files.

---

### Task 2: Move `PlayerComponent` to Game.dll (atomic migration)

**Files:**
- Create: `src/game/src/PlayerComponent.h`
- Modify: `src/common/include/ECS.h`
- Modify: `src/common/include/ComponentSerialization.h`
- Modify: `src/ecs/src/ComponentSerializers.cpp`
- Modify: `src/common/include/ECSCommands.h`
- Modify: `src/game/src/game.cpp`
- Delete: `src/editor/src/panels/inspector/PlayerEditor.cpp`, `src/editor/src/panels/inspector/PlayerEditor.h`
- Modify: `src/editor/CMakeLists.txt`
- Modify: `src/editor/src/panels/EcsInspectorPanel.cpp`
- Modify: `tests/test_worldserial.cpp`

This task is atomic: the tree does not build until every edit is done. Make all edits, then build once at the end.

- [ ] **Step 1: Create the game-owned header (`src/game/src/PlayerComponent.h`)**
```cpp
#pragma once
#include <nlohmann/json.hpp>

// Marks the player-controlled entity. Moved by the player movement system (game) from input.
// Game-owned component: registered via GameRegisterComponents (non-builtin), not an engine builtin.
struct PlayerComponent {
    float MoveSpeed = 5.0f; // world units / second
};

inline void to_json(nlohmann::json& j, const PlayerComponent& t) {
    j = nlohmann::json{{"MoveSpeed", t.MoveSpeed}};
}
inline void from_json(const nlohmann::json& j, PlayerComponent& t) {
    j.at("MoveSpeed").get_to(t.MoveSpeed);
}
```

- [ ] **Step 2: Remove the struct + X-macro entry (`src/common/include/ECS.h`)**

Delete the struct definition (`:217-220`):
```cpp
// Marks the player-controlled entity. Moved by PlayerMovementSystem (game) from input.
struct PlayerComponent {
    float MoveSpeed = 5.0f; // world units / second
};
```
Delete the X-macro line (`:394`):
```cpp
    X(PlayerComponent) \
```
(Make sure the X-macro list is still well-formed — the line above `X(PlayerComponent)` ends with ` \` and the line below continues the list; removing the line keeps the backslash-continuation chain intact.)

- [ ] **Step 3: Remove the builtin (de)serializers (`src/common/include/ComponentSerialization.h`)**

Delete (`:130-135`):
```cpp
inline void to_json(nlohmann::json& j, const PlayerComponent& t) {
    j = nlohmann::json{{"MoveSpeed", t.MoveSpeed}};
}
inline void from_json(const nlohmann::json& j, PlayerComponent& t) {
    j.at("MoveSpeed").get_to(t.MoveSpeed);
}
```

- [ ] **Step 4: Remove the builtin registration (`src/ecs/src/ComponentSerializers.cpp`)**

Delete line `:16`:
```cpp
        r.Register<PlayerComponent>("PlayerComponent", true);
```

- [ ] **Step 5: Remove the typed command branches (`src/common/include/ECSCommands.h`)**

In `ApplyComponentCommand`, delete the branch (`:356-359`):
```cpp
        } else if (componentData.Type == std::type_index(typeid(PlayerComponent))) {
            if (auto* player = componentData.Get<PlayerComponent>()) {
                world.AddComponent(entity, *player);
            }
```
(Re-join the `else if` chain: the code after this block — `} else if (... UIRectComponent ...)` — must still chain off the preceding `SkyComponent` branch. After deletion, the `SkyComponent` block's closing `}` is directly followed by `else if (... UIRectComponent ...)`.)

In `RemoveComponentByType`, delete the branch (`:426-427`):
```cpp
        } else if (typeIndex == std::type_index(typeid(PlayerComponent))) {
            world.RemoveComponent<PlayerComponent>(entity);
```
(Same care: the `SkyComponent` remove branch must chain directly into the `UIRectComponent` remove branch.)

(The `DuplicateEntityComponents` Player reference was already removed in Task 1 — it is now registry-driven.)

- [ ] **Step 6: Include + register in the game (`src/game/src/game.cpp`)**

Add includes near the top (after the existing `#include "ComponentSerializerRegistry.h"` at `:3`):
```cpp
#include "PlayerComponent.h"  // game-owned component (moved out of ECS.h)
#include "EditorUI.h"         // EditorUI bridge for the inspector hook
```
Replace the empty `GameRegisterComponents` stub (`:927`) body:
```cpp
void GameRegisterComponents() {
    SerializerRegistry().Register<PlayerComponent>("PlayerComponent");
    SerializerRegistry().RegisterEditorHook("PlayerComponent",
        [](const EditorUI& ui, nlohmann::json& j) { return ui.DragFloat(j, "MoveSpeed", 0.1f); });
}
```

- [ ] **Step 7: Delete the dedicated editor**

Delete both files:
```
git -C C:/dev/personal/clang-examples rm src/editor/src/panels/inspector/PlayerEditor.cpp src/editor/src/panels/inspector/PlayerEditor.h
```
In `src/editor/CMakeLists.txt`, delete line `:35`:
```cmake
    src/panels/inspector/PlayerEditor.cpp
```
In `src/editor/src/panels/EcsInspectorPanel.cpp`, delete the include (`:20`):
```cpp
#include "inspector/PlayerEditor.h"
```
and the registration (`:40`):
```cpp
    m_Editors.push_back(std::make_unique<PlayerEditor>());
```

- [ ] **Step 8: Remove the now-orphaned serializer test (`tests/test_worldserial.cpp`)**

Delete `T06_player_roundtrip` (`:142-148`):
```cpp
static void T06_player_roundtrip()
{
    PlayerComponent in; in.MoveSpeed = 13.5f; // non-default
    const nlohmann::json j = in;
    const auto out = j.get<PlayerComponent>();
    EXPECT(near(out.MoveSpeed, in.MoveSpeed));
}
```
and remove its call from `main()` (search for `T06_player_roundtrip();` and delete that line).

- [ ] **Step 9: Reconfigure + full build (green)**
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise
```
Expected: all targets build clean (`ecs`, `Engine`, `editor`, `game`, all tests). Watch for: any lingering `PlayerComponent` reference outside `src/game` would fail to compile.

- [ ] **Step 10: Verify the type fully moved**
```
git -C C:/dev/personal/clang-examples grep -n "PlayerComponent" -- src/ecs src/engine src/editor src/common
```
Expected: NO matches under `src/ecs`, `src/engine`, `src/editor`, or `src/common` (the type now lives only in `src/game`). If any match remains, fix it before committing.

- [ ] **Step 11: Commit**
```
git -C C:/dev/personal/clang-examples add -A
git -C C:/dev/personal/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(game): migrate PlayerComponent to Game.dll (non-builtin + editor hook); remove typed branches + PlayerEditor"
```
Verify the commit contains: `src/game/src/PlayerComponent.h` (new), `ECS.h`, `ComponentSerialization.h`, `ComponentSerializers.cpp`, `ECSCommands.h`, `game.cpp`, `PlayerEditor.{cpp,h}` (deleted), `editor/CMakeLists.txt`, `EcsInspectorPanel.cpp`, `test_worldserial.cpp` — and nothing unrelated.

---

### Task 3: Full regression + smoke notes

**Files:** none (verification; commit fixups only if needed).

- [ ] **Step 1: Full clean build**
```
cmake --build --preset msvc-win64-vs2026-enterprise
```
Expected: all targets, no errors/`LNK`.

- [ ] **Step 2: Suites**
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_reloadpreserve.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_playermove.exe
```
Expected: each prints its pass line. (`test_compserial` emits the expected `NopeNotReal` warn; `test_reloadpreserve` the expected `BadComp` warn.)

- [ ] **Step 3: Manual smoke (optional/deferred, human-owned)**

After restarting the editor (required — `ECS.h` changed): with a Player entity in the scene —
1. **Reload survival:** edit `src/game/src/Game.cpp` (a `.cpp`-only change), rebuild `game`, and confirm after hot-reload the entity still has `PlayerComponent` with its `MoveSpeed`.
2. **Custom inspector:** confirm the inspector shows a `MoveSpeed` `DragFloat` widget (the game `editorDraw` hook) and edits commit.
3. **Duplicate:** Ctrl+D the entity; confirm the copy has `PlayerComponent` with the same `MoveSpeed`.
4. **Persistence:** save + reload `world.json`; confirm `MoveSpeed` round-trips. Confirm an existing `world.json` with a `"PlayerComponent"` entry still loads.

Note: if the editor's "Add Component" menu does not list non-builtin (game) components, adding `PlayerComponent` via the UI may be unavailable — Player entities normally come from `world.json`/game seeding. This is a pre-existing non-builtin add-menu limitation, out of scope here; flag it if it blocks the smoke test.

- [ ] **Step 4: Commit fixups (only if needed).**

---

## Done criteria

- Entity duplication is registry-driven via `copyTo`; `test_compserial` T09 green; duplicate behavior unchanged for builtins (same 17 types) and now includes game components (Task 1).
- `PlayerComponent` is defined only in `src/game/src/PlayerComponent.h`; `git grep PlayerComponent` matches nothing under `src/ecs`, `src/engine`, `src/editor`, `src/common`; it is registered (non-builtin) + hooked in `GameRegisterComponents` (Task 2).
- `PlayerEditor` deleted; Player edits via the generic editor + the game `editorDraw` `DragFloat`; typed `ApplyComponentCommand`/`RemoveComponentByType` Player branches removed; `world.json` round-trips via the game serializer (Task 2).
- Full tree builds; `test_compserial`/`test_reloadpreserve`/`test_ecs`/`test_worldserial`/`test_playermove` green (Task 3).
- No `GAME_API_VERSION` bump; full rebuild + editor restart documented.

## Notes

- Behavior-preserving duplicate: the serializer registry's registered set is exactly the old curated `DuplicateEntityComponents` list (17 per-entity types); singletons + hierarchy are unregistered and thus excluded as before.
- `PlayerComponent` is POD → its reload-survival path is byte (memcpy); it keeps `to_json`/`from_json` for `world.json` disk persistence (independent paths).
- The `editorDraw` hook must be set AFTER `Register<PlayerComponent>` in `GameRegisterComponents` (the `Register` upsert clears `editorDraw`). On every reload both run, so the hook is always present.
```
