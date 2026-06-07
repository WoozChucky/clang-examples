# Migrate PlayerComponent to Game.dll — Design

**Date:** 2026-06-07
**Status:** Approved (brainstorm) — ready for implementation plan

## Problem

`PlayerComponent` (`struct PlayerComponent { float MoveSpeed = 5.0f; }`) is pure gameplay — only
`Game.dll` reads/writes it (the player movement system); zero engine references. Yet it lives as an
engine **builtin**: declared in `ECS.h`, in the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro,
registered as a builtin serializer in `ecs.dll`, with typed branches in `src/common/ECSCommands.h`
and a dedicated `PlayerEditor` in `editor.exe`. It belongs in `Game.dll`.

The blocker — game-owned components were wiped on hot-reload — was removed by the game-component
hot-reload survival feature (`docs/superpowers/specs/2026-06-07-game-component-reload-survival-design.md`).
With preserve/restore in place, `PlayerComponent` can now move and survive reload.

This migration is also the first real end-to-end exercise of three new mechanisms on one component:
reload-survival (byte path — `PlayerComponent` is trivially copyable), the `editorDraw` hook (custom
inspector from `Game.dll`), and registry-driven entity duplication.

## Goals

- `PlayerComponent` is defined in `Game.dll`, registered via `GameRegisterComponents`, and no longer
  referenced by `ecs.dll`, `Engine`, or `editor.exe`.
- It survives `Game.cpp` hot-reload with `MoveSpeed` intact (byte path).
- It is editable in the inspector via a `Game.dll`-provided `editorDraw` hook.
- It is copied on entity duplication (Ctrl+D) via a new registry-driven duplicate path that fixes
  duplication for **all** game components, not just this one.
- `world.json` round-trips unchanged (name-keyed load through the game's `to_json`/`from_json`).

## Non-goals

- No `GAME_API_VERSION` bump: `GameState` layout and export signatures are unchanged.
- No change to `world.json` format.
- No migration of other game-candidate builtins (e.g. `MenuButtonComponent`) — separate work.

## Design

### 1. Move the type into Game.dll

- **New header `src/game/src/PlayerComponent.h`**: the `PlayerComponent` struct plus its
  `to_json`/`from_json` (moved verbatim from `ECS.h` and `src/common/include/ComponentSerialization.h`).
- **`ECS.h`**: remove `X(PlayerComponent)` from `ECS_FOR_EACH_REGISTERED_COMPONENT` and delete the
  `struct PlayerComponent` definition. `ecs.dll` stops explicitly instantiating
  `ComponentArray<PlayerComponent>`; `Game.dll` (and `test_playermove`) header-instantiate it locally,
  which the ECS boundary already supports for game types.
- **`ComponentSerialization.h`**: remove the `PlayerComponent` `to_json`/`from_json` (moved to the game
  header).
- **`ComponentSerializers.cpp`**: remove the `r.Register<PlayerComponent>("PlayerComponent", true)`
  builtin registration.
- Game systems that use `PlayerComponent` (`game.cpp` and any system header) include the new header.

### 2. Registry-driven entity duplication (`copyTo`)

The current `ECSCommands.h` `DuplicateEntity` deep-copies components via a hardcoded **typed list**
(`world.GetComponent<X>(src)` → `world.AddComponent(dst, *c)` for a curated set). A game-owned type
cannot appear in that list (`src/common` cannot reference a `Game.dll` type), so without this change
Ctrl+D would silently drop `PlayerComponent` and every future game component.

- **`ComponentSerializerEntry`**: add `void (*copyTo)(ECS&, EntityId src, EntityId dst) = nullptr;`,
  installed **unconditionally** in `Register<T>` (every component type is copy-constructible):
  ```cpp
  e.copyTo = [](ECS& w, EntityId src, EntityId dst) {
      if (const T* p = w.GetComponent<T>(src)) w.AddComponent<T>(dst, *p);
  };
  ```
- **`ECSCommands.h` `DuplicateEntity`**: replace the typed copy list with a loop over
  `SerializerRegistry().Entries()` calling `entry.copyTo(world, src, dst)`.

**Behavior note:** the serializer registry holds only **persisted** component types (runtime-only
scratch like `InputStateComponent`/`ActionQueueComponent` is never registered), so the registry-driven
duplicate copies exactly the authored set — a superset of the old curated typed list, and more correct.
The plan MUST diff the old typed list against the registered set and confirm no surprising new copy
(and that nothing the old list intentionally omitted gets copied harmfully).

### 3. Remove typed `PlayerComponent` command branches

`ECSCommands.h` has two typed branches keyed on `std::type_index(typeid(PlayerComponent))`:
`ApplyComponentCommand` (typed add) and `RemoveComponentByType` (typed remove). Delete both. The
game-owned component uses the **name-based** command path
(`AddComponentByName`/`ModifyComponentJson`/`RemoveComponentByName`), which dispatches through the
registry by string name and never references the C++ type — so `src/common` no longer mentions
`PlayerComponent`.

### 4. Editor: delete the dedicated editor, add an `editorDraw` hook

- Delete `src/editor/src/panels/inspector/PlayerEditor.{cpp,h}`; remove the source from
  `editor/CMakeLists.txt` and the editor's registration in its `m_Editors` list
  (`EcsInspectorPanel.cpp`). `PlayerComponent`, now non-builtin, routes through the generic-editor
  path (`GenericComponentEditor::Draw`), which invokes its `editorDraw` hook when present.
- In `GameRegisterComponents` (`game.cpp`):
  ```cpp
  SerializerRegistry().Register<PlayerComponent>("PlayerComponent");
  SerializerRegistry().RegisterEditorHook("PlayerComponent",
      [](const EditorUI& ui, nlohmann::json& j) { return ui.DragFloat(j, "MoveSpeed", 0.1f); });
  ```
  Order matters: `Register` first (upsert installs serializer + reload byte path + copyTo), then
  `RegisterEditorHook` (the upsert in `Register` clears `editorDraw`, so the hook must be set after).

### 5. Registration ordering & persistence

`GameRegisterComponents` runs inside `GameLibrary::LoadOrReload` — at startup and on every reload —
before the world is loaded (first `GameUpdate`) and before reload-restore. So both `world.json` load
(`LoadEntityComponents` → registry `Find("PlayerComponent")` → game `from_json`) and reload-restore
find the registered serializer. Existing `world.json` files with `"PlayerComponent"` entries load
unchanged.

`PlayerComponent` is trivially copyable, so its **reload-survival** path is the byte (memcpy) path; it
keeps `to_json`/`from_json` for `world.json` **disk** persistence (the two paths are independent by
design).

## Build / deploy impact

- No `GAME_API_VERSION` bump (GameState layout + exports unchanged).
- `ECS.h` X-macro change ⇒ rebuild **ecs + Engine + editor + game** and **restart the editor** (the
  running editor links the old `ComponentArray` instantiation set).

## Affected files

| File | Change |
|------|--------|
| `src/game/src/PlayerComponent.h` | **new** — struct + `to_json`/`from_json` |
| `src/common/include/ECS.h` | remove struct + X-macro entry |
| `src/common/include/ComponentSerialization.h` | remove `PlayerComponent` (de)serializers |
| `src/ecs/src/ComponentSerializers.cpp` | remove builtin registration |
| `src/common/include/ComponentSerializerRegistry.h` | add `copyTo` member + install in `Register<T>` |
| `src/common/include/ECSCommands.h` | registry-driven `DuplicateEntity`; remove 2 typed `PlayerComponent` branches |
| `src/game/src/game.cpp` | include header; register component + editor hook in `GameRegisterComponents`; include `EditorUI.h` |
| `src/editor/src/panels/inspector/PlayerEditor.{cpp,h}` | **delete** |
| `src/editor/CMakeLists.txt` | remove `PlayerEditor.cpp` |
| `src/editor/src/panels/EcsInspectorPanel.cpp` | remove `PlayerEditor` from `m_Editors` |
| `tests/test_worldserial.cpp` | delete `T06_player_roundtrip` |
| `tests/test_playermove.cpp` | include new header; confirm builds/passes |
| `tests/test_compserial.cpp` | add a `copyTo` duplicate-copy test |

## Error handling / edge cases

- **`world.json` loaded before the game registers the type** — cannot happen: `GameRegisterComponents`
  runs during `LoadOrReload`, the world loads later on first `GameUpdate`. If it ever did, the existing
  `LoadEntityComponents` warns ("no serializer for component 'PlayerComponent'") and skips — no crash.
- **Duplicate of an entity without `PlayerComponent`** — `copyTo` guards on `GetComponent` returning
  null; no-op.
- **`editorDraw` hook cleared on reload** — `Register` upsert nulls `editorDraw`; `GameRegisterComponents`
  re-applies `RegisterEditorHook` after `Register` on every load, so the hook is always present.
- **Editor inspector for `PlayerComponent` before the game registers a hook** — falls back to the
  generic JSON tree (still editable); not a failure.

## Testing

- **`test_compserial`**: add a test that registers a probe, populates an entity, calls the entry's
  `copyTo` to a second entity, and asserts the value copied (covers the new registry duplicate
  primitive without needing the full `DuplicateEntity` command).
- **`test_worldserial`**: remove `T06_player_roundtrip` (the type's serializer is now game-owned; the
  generic round-trip is covered by `test_compserial`'s synthetic probes).
- **`test_playermove`**: include the new header; confirm it still builds and passes (movement logic
  unchanged).
- **Full regression**: `test_compserial`, `test_reloadpreserve`, `test_ecs`, `test_worldserial`,
  `test_playermove` all green; full tree builds.
- **Manual smoke (human-owned)**: with a Player entity in the scene — (a) edit `Game.cpp` and
  hot-reload; confirm `MoveSpeed` survives; (b) confirm the inspector shows the custom `DragFloat`
  widget and edits commit; (c) Ctrl+D duplicates the entity and the copy has `PlayerComponent` with the
  same `MoveSpeed`; (d) save/load `world.json` round-trips `MoveSpeed`.

## Done criteria

- `PlayerComponent` is defined only in `src/game/src/PlayerComponent.h`; `grep PlayerComponent`
  matches no file under `src/ecs`, `src/engine`, or `src/editor`, and only the registry-agnostic
  machinery under `src/common`.
- `ecs.dll` no longer instantiates `ComponentArray<PlayerComponent>`; `Game.dll` registers it
  (non-builtin) with an `editorDraw` hook.
- Entity duplication copies all registered (persisted) components via `copyTo`, including
  `PlayerComponent`.
- Reload survival (byte), custom inspector (hook), duplicate (copyTo), and `world.json` round-trip all
  verified — unit tests green + manual smoke confirmed.
- No `GAME_API_VERSION` bump; full rebuild + editor restart documented.
```
