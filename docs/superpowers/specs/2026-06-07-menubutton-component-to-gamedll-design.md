# Migrate MenuButtonComponent to Game.dll — Design

**Date:** 2026-06-07
**Status:** Approved (brainstorm) — ready for implementation plan

## Problem

`MenuButtonComponent` (`ActionId` + `Normal`/`Hover`/`Press` `glm::vec4` colors) is pure gameplay/UI:
its `ActionId` binds to the game's `Actions::` vocabulary, and the menu interaction that consumes it
(`MenuInteractionSystem`) is game code. Zero engine references. Yet it lives as an engine builtin
(declared in `ECS.h`, in the X-macro, builtin-registered in `ecs.dll`, with typed command branches
and a dedicated `MenuButtonEditor`). It belongs in `Game.dll`.

This is the second component migration after `PlayerComponent`
(`docs/superpowers/specs/2026-06-07-player-component-to-gamedll-design.md`), following the same
pattern but exercising a richer editor: a named action dropdown plus three color pickers. The named
dropdown requires one new `EditorUI` bridge primitive, because the existing `EditorUI::Combo` stores
the selected *index*, whereas `ActionId` is an opaque id (e.g. `0x00010001`), not a `0..N-1` index.

The game's `Actions.h` action vocabulary, currently in `src/common`, has no engine consumer; its only
non-game user is `MenuButtonEditor`, which this migration deletes. So `Actions.h` is relocated into
`Game.dll` as part of the migration.

## Goals

- `MenuButtonComponent` is defined in `Game.dll`, registered (non-builtin) via `GameRegisterComponents`
  with an `editorDraw` hook, and no longer referenced by `ecs.dll`, `Engine`, or `editor.exe`.
- The inspector preserves today's UX: a named action dropdown (None/Play/Quit/Back) and three RGBA
  color pickers — delivered by a game-side `editorDraw` hook.
- A reusable `EditorUI::ComboMapped` primitive supports enum-like ids (stored value ≠ index) for this
  and future game components.
- `Actions.h` lives in `Game.dll` (game owns its action vocabulary).
- Reload survival (byte path — `MenuButtonComponent` is trivially copyable), entity duplication
  (registry-driven `copyTo`), and `world.json` round-trip all continue to work.

## Non-goals

- No `GAME_API_VERSION` bump (`GameState` layout + export signatures unchanged).
- No change to `world.json` format.
- No migration of other components.

## Design

### 1. New `EditorUI` primitive: `ComboMapped`

The existing `EditorUI::Combo(obj, key, labels, count)` reads/writes `obj[key]` as a `0..count-1`
index. `MenuButtonComponent::ActionId` is an opaque id, not an index, so a value↔index mapping is
required (exactly what `MenuButtonEditor` does today with `kActionNames`/`kActionIds`).

- **`src/common/include/EditorUI.h`** — add to the `EditorUI` struct:
  ```cpp
  // Edits an integer json value chosen from a fixed (label,value) set. Reads obj[key], selects the
  // index whose values[i] == obj[key] (default 0 if none match), shows a combo, and on change writes
  // values[selected] back to obj[key]. For enum-like ids whose stored value is NOT a 0..N-1 index.
  bool (*ComboMapped)(nlohmann::json& obj, const char* key,
                      const char* const* labels, const int* values, int count);
  ```
- **`src/editor/src/panels/inspector/EditorUIImpl.cpp`** — implement `UI_ComboMapped`:
  read `obj[key]` as int (no-op returning false if missing/non-integer); find `idx` where
  `values[idx] == current` (default 0); `ImGui::Combo(key, &idx, labels, count)`; on change set
  `obj[key] = values[idx]` and return true. Wire it into the `EditorUIInstance()` table in struct
  order.
- Build-verified only (the ImGui-backed `EditorUIImpl` widgets have no unit tests, by existing
  convention); the dropdown is covered by manual smoke.

### 2. Move the type → `src/game/src/MenuButtonComponent.h`

New header with the struct and its ADL `to_json`/`from_json` (moved verbatim from `ECS.h` and
`ComponentSerialization.h`):
```cpp
#pragma once
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

// Marks a UI-rect entity as a clickable menu button (authored). ActionId is an Actions:: id
// (0 = none). The game interaction system drives UIRectComponent.Color between Normal/Hover/Press.
// Game-owned component: registered via GameRegisterComponents (non-builtin).
struct MenuButtonComponent {
    uint32_t  ActionId = 0;
    glm::vec4 Normal{0.15f, 0.15f, 0.18f, 1.0f};
    glm::vec4 Hover {0.25f, 0.25f, 0.30f, 1.0f};
    glm::vec4 Press {0.35f, 0.35f, 0.42f, 1.0f};
};
// to_json/from_json moved from ComponentSerialization.h (ActionId + Normal/Hover/Press).
```
- **`ECS.h`**: delete the `MenuButtonComponent` struct and remove `X(MenuButtonComponent)` from the
  X-macro (keep the backslash-continuation chain valid).
- **`ComponentSerialization.h`**: remove the `MenuButtonComponent` `to_json`/`from_json`. The
  `glm::vec4` (de)serializers it relies on stay in `ComponentSerialization.h` (other builtins use
  them); the new game header includes `ComponentSerialization.h` if needed for the `vec4` ADL
  conversions, OR defines `to_json`/`from_json` over the vec4 fields directly — the plan picks
  whichever compiles cleanly without pulling engine-only types into `Game.dll` (the vec4 glm
  serializers are header-only and safe to reuse).
- **`ComponentSerializers.cpp`**: remove the builtin registration line.

> Implementation note for the plan: confirm where the `glm::vec4` `to_json`/`from_json` live. If they
> are in `ComponentSerialization.h` (a common header), the game header can `#include` it (or just the
> vec4 helpers) so `MenuButtonComponent`'s serializers compile in `Game.dll`. Do not move the vec4
> helpers — other builtins depend on them.

### 3. Move `Actions.h` → `src/game/src/Actions.h`

Relocate the file verbatim. Update `game.cpp`'s `#include` to the new path. `Actions.h` is pure
`constexpr uint32_t` constants with no dependency on `MenuButtonComponent`, and `ActionEvent`/
`ActionQueueComponent` (still in `ECS.h`) reference `ActionId` only as `uint32_t` — so the move is
self-contained.

### 4. Remove typed command branches + delete the dedicated editor

- **`ECSCommands.h`**: delete the `MenuButtonComponent` branch in `ApplyComponentCommand` and in
  `RemoveComponentByType`. Keep the surrounding `else if` chains well-formed. (Duplication is already
  registry-driven via `copyTo`.)
- Delete **`src/editor/src/panels/inspector/MenuButtonEditor.{cpp,h}`**; remove from
  `editor/CMakeLists.txt` and the `m_Editors` registration in `EcsInspectorPanel.cpp`. The component
  now routes through the generic editor → its `editorDraw` hook.

### 5. Register + hook (`GameRegisterComponents`, `src/game/src/game.cpp`)

Add includes (`MenuButtonComponent.h`; `Actions.h` already included by `game.cpp` — update its path)
and register alongside `PlayerComponent`:
```cpp
SerializerRegistry().Register<MenuButtonComponent>("MenuButtonComponent");
SerializerRegistry().RegisterEditorHook("MenuButtonComponent", [](const EditorUI& ui, nlohmann::json& j) {
    static const char* names[] = { "None", "Play", "Quit", "Back" };
    static const int    ids[]  = { (int)Actions::None, (int)Actions::Play, (int)Actions::Quit, (int)Actions::Back };
    bool changed = false;
    changed |= ui.ComboMapped(j, "ActionId", names, ids, 4);
    changed |= ui.ColorEdit4(j, "Normal");
    changed |= ui.ColorEdit4(j, "Hover");
    changed |= ui.ColorEdit4(j, "Press");
    return changed;
});
```
The action `{names, ids}` table (previously `kActionNames`/`kActionIds` in `MenuButtonEditor.cpp`) now
lives game-side in the hook. `EditorUI::ColorEdit4` already handles the vec4 json shape
(`{R,G,B,A}` or `{X,Y,Z,W}`). `RegisterEditorHook` is called after `Register` (the `Register` upsert
clears `editorDraw`), and both re-run on every reload.

### 6. Cold-start ordering

Already fixed for the Player migration: `GameRegisterComponents` runs inside the initial
`LoadOrReload`, which now precedes `LoadWorldSnapshot` on both the GameThread and ServerApplication
startup paths. So a persisted `MenuButtonComponent` in `world.json` is registered before the world
loads. No further ordering work is needed.

## Affected files

| File | Change |
|------|--------|
| `src/common/include/EditorUI.h` | add `ComboMapped` fn-ptr |
| `src/editor/src/panels/inspector/EditorUIImpl.cpp` | implement + wire `UI_ComboMapped` |
| `src/game/src/MenuButtonComponent.h` | **new** — struct + `to_json`/`from_json` |
| `src/game/src/Actions.h` | **moved** from `src/common/include/Actions.h` |
| `src/common/include/Actions.h` | **deleted** (moved) |
| `src/common/include/ECS.h` | remove struct + X-macro entry |
| `src/common/include/ComponentSerialization.h` | remove `MenuButtonComponent` (de)serializers |
| `src/ecs/src/ComponentSerializers.cpp` | remove builtin registration |
| `src/common/include/ECSCommands.h` | remove 2 typed `MenuButtonComponent` branches |
| `src/game/src/game.cpp` | include new headers; register + hook; update `Actions.h` path |
| `src/editor/src/panels/inspector/MenuButtonEditor.{cpp,h}` | **delete** |
| `src/editor/CMakeLists.txt` | remove `MenuButtonEditor.cpp` |
| `src/editor/src/panels/EcsInspectorPanel.cpp` | remove `MenuButtonEditor` include + registration |
| `tests/test_worldserial.cpp` | delete `T09_menubutton_roundtrip` + its `main()` call |

## Error handling / edge cases

- **`ComboMapped` with a stored `ActionId` not in the table** — defaults the combo to index 0
  (`None`), but does NOT overwrite `obj[key]` unless the user changes the selection (returns false on
  no-op). So an unrecognized id is preserved until explicitly edited.
- **`ComboMapped` missing/non-integer key** — no-op returning false (consistent with the other
  `EditorUI` widgets).
- **`world.json` loaded before registration** — cannot happen (ordering fixed; see §6); a stray
  unregistered load would `SM_WARN` + skip (existing `LoadEntityComponents` behavior).
- **Duplicate / reload** — handled by the existing registry-driven `copyTo` and the byte
  reload-preservation path (`MenuButtonComponent` is trivially copyable).

## Testing

- **`test_worldserial`**: remove `T09_menubutton_roundtrip` (type now game-owned; generic round-trip
  covered by `test_compserial`'s synthetic probes).
- **`ComboMapped`**: no unit test (ImGui layer, consistent with the rest of `EditorUIImpl`);
  build-verified.
- **Full regression**: `test_compserial`, `test_reloadpreserve`, `test_ecs`, `test_worldserial`,
  `test_playermove` all green; full tree builds.
- **Manual smoke (human-owned)**: with a menu-button entity — (a) inspector shows the action dropdown
  (None/Play/Quit/Back) and three RGBA color pickers; selecting an action and editing colors commits;
  (b) edit `Game.cpp`, hot-reload → `ActionId` + colors survive; (c) Ctrl+D duplicates with the
  component intact; (d) `world.json` round-trips; an existing world with a `"MenuButtonComponent"`
  entry still loads and the menu still functions (hover/press coloring + action firing).

## Done criteria

- `MenuButtonComponent` is defined only in `src/game/src/MenuButtonComponent.h`; `git grep
  MenuButtonComponent` matches nothing under `src/ecs`, `src/engine`, `src/editor`, `src/common`.
- `Actions.h` lives in `src/game`; no copy remains in `src/common`.
- `EditorUI::ComboMapped` exists and the menu-button hook uses it for the action dropdown + 3×
  `ColorEdit4`.
- `MenuButtonEditor` deleted; typed `ApplyComponentCommand`/`RemoveComponentByType` branches removed.
- Reload survival (byte), duplicate (`copyTo`), and `world.json` round-trip verified; full tree builds;
  the five test suites green.
- No `GAME_API_VERSION` bump; full rebuild + editor restart documented.

## Notes

- Pattern follows the `PlayerComponent` migration; the only new surface is the `ComboMapped` bridge
  primitive (§1), which is reusable for any future enum-like game component field.
- Connects to the recorded follow-up theme "enrich the `EditorUI` bridge as real hooks land" — this
  migration adds `ComboMapped`; a separate follow-up still tracks adding min/max/format to `DragFloat`.
- `MenuButtonComponent` is POD → reload path is byte (memcpy); it keeps `to_json`/`from_json` for
  `world.json` disk persistence (independent paths).
```
