# Entity dev-names — Design

**Date:** 2026-06-07
**Branch:** `feat/entity-names`
**Status:** Approved (brainstorm) — ready for implementation plan

## Problem

The ECS inspector lists every entity as `Entity %llu` (`EcsInspectorPanel.cpp:98`). As a scene
grows this is poor dev UX — there is no way to tell entities apart without expanding each one's
components. We want optional, dev-authored names shown in the list, with the entity id as a
fallback for unnamed entities.

## Goal

- An optional, engine-owned `NameComponent { std::string Name; }`, persisted in `world.json`,
  copied on duplicate.
- The inspector entity list shows the name when present, falling back to `Entity %llu`.
- Renaming is fast: an always-on rename field for the selected entity, plus **F2** to jump
  straight into it (Unity/Unreal outliner feel). Typing a name auto-adds the component; clearing
  it removes the component.

## Non-goals

- No name uniqueness enforcement (matches Bevy `Name` / flecs; Unreal-style auto-suffixing is out
  of scope).
- No auto-suffix (" (Copy)") on duplicate — the copy keeps the same name; rename after. (YAGNI.)
- No `GAME_API_VERSION` bump (`GameState` layout + game export signatures unchanged).
- No rename-from-the-list inline edit (the always-on field + F2 covers it).

## Context (as-built, verified)

- **Entity list:** `EcsInspectorPanel.cpp:92-99` loops `ctx.WorldSnapshot->GetActiveEntities()`,
  formats `snprintf(entityLabel, ..., "Entity %llu", entity)`, and renders an `ImGui::Selectable`.
  The editor section header at `:187` prints `Editing Entity %llu`.
- **Snapshots are copy-on-write** (`ecs.cpp:95-107`): `CreateSnapshot` does a shallow shared_ptr
  copy of the component-array map (`CopyArraysFrom`), and an array only clones on the *first write
  that tick* (dirty-tracked, `ecs.cpp:167-168`). So a `NameComponent` array that isn't mutated a
  given tick is shared by pointer across snapshots — **zero copies, zero allocs** in steady state.
  A `std::string` inside a component is only costly for components mutated every tick by simulation;
  a name is mutated only on a human rename. Short names hit MSVC SSO (no heap). Cost ≈ free.
- **Builtin component registration:** the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro in `ECS.h`
  drives `ComponentArray<T>` instantiation in `ecs.dll` and membership in `BuiltinComponentTypes()`
  (`ecs.cpp:113-122`). Builtin (de)serialization is registered in
  `src/ecs/src/ComponentSerializers.cpp` via `r.Register<T>("Name", true)`, which wires
  `save`/`load`/`addDefault`/`remove`/`copyTo` (the last drives registry-based entity duplication).
- **Command apply path is registry-driven and builtin-agnostic** (`ECSCommands.h:250-269`):
  `AddComponentByName` → `en->addDefault`, `RemoveComponentByName` → `en->remove`,
  `ModifyComponentJson` → `en->load(world, entity, json)`. None of these check the `builtin` flag —
  they look the entry up by name in `SerializerRegistry()`. So a builtin component can be driven
  entirely through the name+JSON command path with no new command type and no typed branch.
- **`ECSCommand` already carries `std::string`** members (`ComponentName`/`ComponentJson`,
  `ECSCommands.h:86-87`) across the SPSC `ECSCommandRing`, and `ComponentData::Create` uses
  `make_shared<T>(component)` (a real copy, not memcpy) — so string-bearing payloads are safe.
- **Generic inspector loop skips builtins** (`EcsInspectorPanel.cpp:199-202`): the JSON-tree editor
  only renders registry entries where `!en.builtin`. `NameComponent` is builtin → it will not be
  double-edited by the generic loop, and it will not appear in the generic "Add/Remove" menus
  (`:131-159`). The bespoke-editor menus iterate `m_Editors`; `NameComponent` has no `IComponentEditor`,
  so it won't appear there either. The rename field is its only editing surface — intended.

## Design

### 1. `NameComponent` (engine builtin, optional)

`src/common/include/ECS.h` — add the struct (near the other component declarations) and an X-macro
entry:
```cpp
// Optional dev-authored display name for an entity. Editor/tooling concern (shown in the ECS
// inspector list); persisted in world.json. Absent on most entities — the inspector falls back to
// "Entity <id>". First std::string-bearing builtin; cheap under COW snapshots (only the rename tick
// clones the array).
struct NameComponent {
    std::string Name;
};
```
Add `X(NameComponent) \` to `ECS_FOR_EACH_REGISTERED_COMPONENT` (keep the backslash chain valid).
Confirm `<string>` is included by `ECS.h` (it is, transitively; add an explicit `#include <string>`
if not).

### 2. Serialization (builtin)

`src/common/include/ComponentSerialization.h`:
```cpp
inline void to_json(nlohmann::json& j, const NameComponent& t) {
    j = nlohmann::json{ {"Name", t.Name} };
}
inline void from_json(const nlohmann::json& j, NameComponent& t) {
    j.at("Name").get_to(t.Name);
}
```
`src/ecs/src/ComponentSerializers.cpp` — register as builtin alongside the others:
```cpp
r.Register<NameComponent>("NameComponent", true);
```
This wires `save`/`load`/`addDefault`/`remove`/`copyTo`. `copyTo` makes duplicate copy the name;
`save`/`load` make it round-trip in `world.json` (under per-entity key `"NameComponent"`).

### 3. Inspector — list label (`EcsInspectorPanel.cpp`)

In the entity-list loop, replace the unconditional `"Entity %llu"` label with name-or-id:
```cpp
char entityLabel[128];
const NameComponent* nameComp = ctx.WorldSnapshot->GetComponent<NameComponent>(entity);
if (nameComp && !nameComp->Name.empty())
    snprintf(entityLabel, sizeof(entityLabel), "%s", nameComp->Name.c_str());
else
    snprintf(entityLabel, sizeof(entityLabel), "Entity %llu", entity);
```
(`GetComponent<T>` returns `const T*` or `nullptr`. `PushID(entity)` already disambiguates
duplicate-named rows for ImGui.) Optionally append the id dimly for disambiguation — deferred; the
plan may add `"%s  (#%llu)"` if it reads cleanly, but the default is name-only with id fallback.

### 4. Inspector — always-on rename field + F2 (`EcsInspectorPanel.cpp`)

At the top of the `=== COMPONENT EDITOR FOR SELECTED ENTITY ===` block (after the
`Editing Entity %llu` line, before the component editors), add a rename field bound to the selected
entity. Behavior:
- Each frame, sync a `char m_RenameBuf[128]` from the snapshot's current name (or empty) **when the
  selection changes** (track `m_RenameBufFor` = the EntityId the buffer currently reflects; re-sync
  when `selectedEntity != m_RenameBufFor`, so live typing isn't clobbered every frame).
- Render `ImGui::InputText("Name", m_RenameBuf, sizeof(m_RenameBuf),
  ImGuiInputTextFlags_EnterReturnsTrue)`. Commit on Enter **and** on deactivation-after-edit
  (`ImGui::IsItemDeactivatedAfterEdit()`), so click-away also saves.
- On commit, let `buf` = trimmed buffer, `has` = entity currently has `NameComponent`:
  - `buf` non-empty, `!has` → push `AddComponentByName(e,"NameComponent")` **then**
    `ModifyComponentJson(e,"NameComponent", {"Name":buf})`.
  - `buf` non-empty, `has` → push `ModifyComponentJson(e,"NameComponent", {"Name":buf})`.
  - `buf` empty, `has` → push `RemoveComponentByName(e,"NameComponent")`.
  - `buf` empty, `!has` → no-op.
  - Each push checks the ring and `SM_WARN`s on a full ring (matches the existing delete/duplicate
    handlers).
- **F2:** in the existing keyboard-ops block (`:167-181`, gated on
  `selectedEntity != INVALID_ENTITY && !io.WantTextInput`), add
  `if (ImGui::IsKeyPressed(ImGuiKey_F2)) m_FocusRename = true;`. Next frame, immediately before
  rendering the `InputText`, `if (m_FocusRename) { ImGui::SetKeyboardFocusHere(); m_FocusRename =
  false; }`. This focuses the field for instant typing.

New `EcsInspectorPanel` members: `char m_RenameBuf[128] = {};`, `EntityId m_RenameBufFor =
INVALID_ENTITY;`, `bool m_FocusRename = false;`.

The JSON is built with nlohmann (`nlohmann::json{{"Name", buf}}.dump()`), matching the registry
`load` path. No new `ECSCommand` type, no typed branch, no `IComponentEditor`.

## Data flow

Editor (RenderThread, inside the ImGui frame): selected entity → rename field reads the snapshot's
`NameComponent` → on commit pushes name+JSON command(s) into `ECSCommandRing` → GameThread drains
them before game logic and applies via `SerializerRegistry().Find("NameComponent")`'s
`addDefault`/`load`/`remove` → next snapshot carries the new/changed/removed name → list + field
reflect it. 1–2 frame latency, identical to every other editor edit. Duplicate/persistence go
through the builtin `copyTo`/`save`/`load` with no inspector involvement.

## Error handling / edge cases

- **Unnamed entity** → list shows `Entity <id>` (today's behavior, now the fallback branch).
- **Empty / whitespace-only name** → component removed (and never added); no empty `NameComponent`
  persisted. (Trim before the empty check.)
- **Old `world.json` without the key** → loads fine; the entity is simply unnamed (optional
  component; absent key = no component).
- **Ring full** → `SM_WARN`, command dropped (consistent with existing handlers; never silent).
- **Selection changes mid-type** → buffer re-syncs to the newly selected entity's name (the
  `m_RenameBufFor` guard prevents clobbering live typing on the *same* entity).
- **Duplicate-named entities** → allowed; `PushID(entity)` keeps ImGui rows distinct; list rows are
  visually identical but select independently. (Optional id suffix mitigates; deferred.)
- **Selected entity destroyed** → existing `Selected entity no longer exists.` path is unchanged
  (the rename field lives inside the `IsValidEntity` block).

## Testing

- **`test_worldserial`**: add a `NameComponent` round-trip case (serialize `{Name:"Hero"}` →
  `get<NameComponent>` → `EXPECT(out.Name == "Hero")`), and an absent-key/default case
  (`NameComponent{}` → empty name). Register the call in `main()`.
- **Snapshot COW**: already covered by existing ECS tests; no new test (the cost analysis is a
  property of the existing COW mechanism, not new code).
- **Rename field + F2**: ImGui-coupled → build-verified + manual smoke.
- **Full regression**: `test_ecs`, `test_worldserial`, `test_compserial`, `test_reloadpreserve`,
  `test_playermove` green; full tree builds.
- **Manual smoke (human-owned):** (a) name an entity via the field → list updates, persists to
  `world.json`; (b) select an entity, press **F2** → rename field focused, type + Enter commits;
  (c) clear a name to empty → component removed, list falls back to `Entity <id>`; (d) Ctrl+D a
  named entity → copy carries the name; (e) reload `world.json` → names restored; an old world
  without names still loads.

## Done criteria

- `NameComponent` exists as an engine builtin (struct + X-macro + serializer + builtin
  registration); `world.json` round-trips it; duplicate copies it.
- Inspector list shows the name when present, `Entity <id>` otherwise.
- Always-on rename field commits via the name+JSON command path (add-on-type, remove-on-empty);
  **F2** focuses it for the selected entity.
- No `GAME_API_VERSION` bump; full rebuild (ecs + Engine + editor + game) + editor restart
  documented (ECS.h X-macro changed).
- `test_worldserial` gains a `NameComponent` round-trip; all five suites green; full tree builds.

## Notes

- Idiomatic-ECS choice (optional component, not an intrinsic per-entity field): matches Bevy `Name`
  and flecs's built-in name component. Keeps entities as bare ids and only named entities pay for a
  string. See [[project_engine_game_boundary]] for why builtin (engine/editor tooling concern, not
  gameplay) rather than a Game.dll component.
- First `std::string`-bearing builtin component; safe because builtin components are excluded from
  the game-component byte reload path, and COW snapshots make per-tick cost ~zero.
- Reuses the Piece-5 name+JSON command path (`ModifyComponentJson`/`AddComponentByName`/
  `RemoveComponentByName`) — no new ECS command type, no typed `ECSCommands` branch, no dedicated
  editor.
