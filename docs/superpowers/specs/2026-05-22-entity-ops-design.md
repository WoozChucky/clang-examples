# Entity Ops on Selection (Delete + Duplicate) — Design

**Date:** 2026-05-22
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main`.

## Goal

Keyboard ops on the currently-selected entity in the editor:
- **Delete** (`Del`) — destroy the selected entity.
- **Duplicate** (`Ctrl+D`) — create a copy of the selected entity (same components/values).

Both also exposed as inspector context-menu items (Delete already exists; add Duplicate). Mutations
go through the existing `ECSCommand` ring (RenderThread editor → GameThread applies). Editor-side
only; no gameplay/runtime change.

## Background (verified)

- **Commands** (`src/common/include/ECSCommands.h:14-20`): `enum class ECSCommandType { CreateEntity,
  DestroyEntity, AddComponent, RemoveComponent, ModifyComponent }`. `DestroyEntity` factory exists
  (`ECSCommand::DestroyEntity(EntityId)`). **No `DuplicateEntity`** — to add.
- **Processor** (`ECSCommands.h:157-248`, header-only → unit-testable): `ECSCommandProcessor::ProcessCommands(ECS&, ring)`
  drains on the GameThread; `CreateEntity` → `world.CreateEntity()` (returns id synchronously *on the
  GameThread*); `DestroyEntity` → `world.DestroyEntity(id)`. `ApplyComponentCommand` /
  `RemoveComponentByType` dispatch by `std::type_index` over exactly six editor-facing components:
  **TransformComponent, LightningComponent, MeshComponent, MaterialComponent, TextComponent, SunMarker**.
- **Inspector UI** (`EcsInspectorPanel.cpp`): "Create Entity" pushes `CreateEntity()`
  (`:33-44`); the per-entity context menu (`BeginPopupContextItem`) has "Delete Entity" →
  `DestroyEntity(entity)` + clears `selectedEntity` (`:76-84`).
- **ECS API** (`ECS.h`): `EntityId CreateEntity()`, `void DestroyEntity(EntityId)`,
  `AddComponent<T>`, `GetComponent<T>`, `HasComponent<T>`, `bool IsValidEntity(EntityId)`. The
  registered-component X-macro (`ECS.h:143-158`) lists 15 types — but most are **singletons**
  (WorldCameraComponent, UICameraComponent, ViewportComponent, InputStateComponent,
  AppControlComponent, DayNightConfigComponent, FreeLookControlComponent) or **hierarchy**
  (ParentComponent, ChildComponent) and must NOT be copied to a duplicated entity.
- **Async-id confirmed**: `CreateEntity` is applied on the GameThread next tick; the editor never
  learns the new id (no ECS response channel — `RGCommandRing` is renderer-only). → Duplicate must
  be a single GameThread-side `DuplicateEntity(src)` command, and (per user decision) selection
  stays on the source.
- **Keyboard** (`ImGuiRenderer.cpp`): `Del` and `D` are mapped to ImGui keys; modifiers fed to
  `io`. `io.WantTextInput` is already used to gate game input — reuse it to avoid acting while
  typing in a field.

## Scope

**In scope:** `ECSCommandType::DuplicateEntity` + factory + `ProcessCommands` case + a
`DuplicateEntityComponents` helper (copies the 6 editor components); Del/Ctrl+D handling in
`EcsInspectorPanel::Draw`; a "Duplicate Entity" context-menu item; a `test_ecs` case.

**Out of scope / non-goals:** undo/redo (delete is instant, matching today's menu); auto-selecting
the duplicate (user chose "keep selection on source" — avoids a cross-thread id round-trip);
offsetting the duplicate's transform (created in place, overlapping the source — move with the
gizmo); duplicating singleton/hierarchy components (only the 6 per-entity editor components);
multi-select. No `GAME_API_VERSION` bump (new enum value + command case; no GameState/component-
layout change).

## Design

### 1. `DuplicateEntity` command (`ECSCommands.h`)
- Enum: add `DuplicateEntity` (e.g. `= 5`) to `ECSCommandType`.
- Factory: `static ECSCommand DuplicateEntity(EntityId src)` — sets `Type = DuplicateEntity`,
  `TargetEntity = src` (reuses the existing field).
- `ProcessCommands` case:
  ```cpp
  case ECSCommandType::DuplicateEntity: {
      if (cmd.TargetEntity != INVALID_ENTITY && world.IsValidEntity(cmd.TargetEntity)) {
          const EntityId dst = world.CreateEntity();
          DuplicateEntityComponents(world, cmd.TargetEntity, dst);
      }
      break;
  }
  ```
- Helper (static in `ECSCommands.h`, mirroring the `ApplyComponentCommand` component set):
  ```cpp
  static void DuplicateEntityComponents(ECS& world, EntityId src, EntityId dst) {
      if (auto* c = world.GetComponent<TransformComponent>(src)) world.AddComponent(dst, *c);
      if (auto* c = world.GetComponent<LightningComponent>(src)) world.AddComponent(dst, *c);
      if (auto* c = world.GetComponent<MeshComponent>(src))      world.AddComponent(dst, *c);
      if (auto* c = world.GetComponent<MaterialComponent>(src))  world.AddComponent(dst, *c);
      if (auto* c = world.GetComponent<TextComponent>(src))      world.AddComponent(dst, *c);
      if (world.HasComponent<SunMarker>(src))                    world.AddComponent(dst, SunMarker{});
  }
  ```
  (`GetComponent` returns null when absent, so the `if` doubles as the has-check; `SunMarker` is a
  tag → `HasComponent` + default-construct. This is the SAME six-component set the editor already
  add/remove-dispatches, so no singleton/hierarchy component is duplicated.)

### 2. Keyboard handling (`EcsInspectorPanel::Draw`)
Once per Draw, when an entity is selected and no text field is active:
```cpp
ImGuiIO& io = ImGui::GetIO();
if (selectedEntity != INVALID_ENTITY && !io.WantTextInput) {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) {
        ctx.App->ECSCommandRing.Push(ECSCommand::DuplicateEntity(selectedEntity)); // warn on full
    } else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        ctx.App->ECSCommandRing.Push(ECSCommand::DestroyEntity(selectedEntity));   // warn on full
        selectedEntity = INVALID_ENTITY;
    }
}
```
(Check Ctrl+D before plain Del so the chord isn't masked; both `Push` calls `SM_WARN` on a full
ring, matching the existing handlers. `IsKeyChordPressed`/`IsKeyPressed` are global ImGui state, so
this works regardless of which panel is focused — gated only by "an entity is selected" and "not
typing".)

### 3. Context-menu "Duplicate Entity"
In the per-entity `BeginPopupContextItem` block (next to "Delete Entity"), add:
```cpp
if (ImGui::MenuItem("Duplicate Entity")) {
    ctx.App->ECSCommandRing.Push(ECSCommand::DuplicateEntity(entity)); // warn on full
}
```
(Delete stays as-is. Selection unchanged on duplicate.)

## Data flow

Editor (RenderThread): key/menu → push `DestroyEntity`/`DuplicateEntity` to `ECSCommandRing`.
GameThread (next tick): `ProcessCommands` applies — Destroy removes the entity; Duplicate creates a
new entity and copies the 6 components. The new/removed state appears in the next ECS snapshot the
editor renders + lists. Delete clears the editor's selection immediately; Duplicate leaves it on the
source (the copy overlaps it until moved).

## Build / verification

Build preset `msvc-win64-vs2026-community`. No `GAME_API_VERSION` bump (rebuild engine/editor/game
anyway since `ECSCommands.h` is shared).
- **Unit test (`test_ecs`)**: create an entity, add e.g. Transform(+nonzero values)+Mesh; push
  `ECSCommand::DuplicateEntity(src)` into a `SpscRing<ECSCommand,128>`; `ProcessCommands(world, ring)`;
  assert entity count increased by 1 and the new entity has Transform+Mesh with the same values, and
  that singleton/other components were not spuriously added. Also a `DestroyEntity` round-trip
  (push → process → entity gone) if not already covered. `All ECS tests passed.`
- `editor`/`runtime` build clean; other `test_*` stay green.
- **GUI smoke (user):** select an entity, press `Ctrl+D` → a second identical entity appears (Entity
  Count +1; visible if it has a mesh, overlapping the original — move it with the gizmo to see both);
  press `Del` → selected entity vanishes, selection clears, gizmo/outline gone; context-menu
  Duplicate/Delete work the same; typing a value in an inspector field and pressing Del does NOT
  delete the entity (text-edit guard); `runtime.exe` unaffected.

## Risks

- **Duplicating wrong components** (singletons/hierarchy) → corrupt world. Mitigated by copying only
  the explicit 6 editor components (same set the inspector manages), not the full X-macro.
- **Del while typing** → accidental delete. Mitigated by the `!io.WantTextInput` guard.
- **Stale src** (deleted same frame as a duplicate) → mitigated by the `IsValidEntity` guard in the
  command case.
- **No undo** — delete is instant and unrecoverable, identical to the existing menu behavior;
  out of scope to add now.
