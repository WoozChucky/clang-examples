# EcsInspectorPanel Decomposition — Design Spec

**Date:** 2026-05-28
**Status:** Approved
**Type:** Refactor — decompose a 1119-line god-file into per-component editor units
**Sequence:** Spec B of 2 (builds on Spec A folder restructure, merged at `731901d`; EcsInspectorPanel now in `src/editor/src/panels/`)

---

## Goal

`src/editor/src/panels/EcsInspectorPanel.cpp` is 1119 lines. Each editable component has **3 repeated touchpoints** in the file: an add-component context-menu entry, a remove-component context-menu entry, and a per-component editor block. Adding a component means editing 3 places here (plus `ECS.h` X-macro and `ECSCommands.h`). The 15 editor blocks also repeat an identical scaffold (entity-switch reset → live-refresh → dirty → `ModifyComponent` push) that drowns the per-component widget logic.

Decompose into an `IComponentEditor` interface + a registry of per-component editor objects. Each component's add/remove/edit logic lives in one small `*Editor.{cpp,h}` file. `EcsInspectorPanel` becomes a thin shell that iterates the registry. **Zero behavior change** — same panels, same widgets, same UX order.

---

## Architecture

**Interface + registry vector** (Option 1 of the brainstorm). The inspector owns `std::vector<std::unique_ptr<IComponentEditor>>`, built once in the constructor in the current display order. The 3 touchpoints collapse to 3 loops over the registry:

- **Add menu:** for each editor where `!Has(snap,e)` → `MenuItem(Label)` → `AddDefault(ctx,e)`.
- **Remove menu:** for each editor where `Has(snap,e)` → `MenuItem(Label)` → `Remove(ctx,e)`.
- **Editor section:** for each editor where `Has(snap,sel)` → `CollapsingHeader(Label)` → `DrawEditor(ctx,sel)`.

Each editor object owns its own per-frame edit-state (the former `editX` / `lastEditedXEntity` / `xModified` triplet), encapsulated in an `EditState<T>` helper that also centralizes the repeated scaffold.

All editors live in `src/editor/src/panels/inspector/`. The shell (`EcsInspectorPanel.{cpp,h}`) stays in `src/editor/src/panels/`.

---

## Core types

```cpp
// panels/inspector/IComponentEditor.h
#pragma once
#include "ECS.h"   // EntityId
struct EditorContext;
class  ECS;

// One editor per inspector-editable component type. Registered in
// EcsInspectorPanel's constructor; iterated for the add-menu, remove-menu,
// and editor sections. Each concrete editor owns its own EditState.
class IComponentEditor {
public:
    virtual ~IComponentEditor() = default;
    virtual const char* Label() const = 0;                              // menu + header text
    virtual bool Has(const ECS& snap, EntityId e) const = 0;            // HasComponent<T>
    virtual void AddDefault(const EditorContext& ctx, EntityId e) = 0;  // push AddComponent(e, T{})
    virtual void Remove(const EditorContext& ctx, EntityId e) = 0;      // push RemoveComponent<T>(e)
    virtual void DrawEditor(const EditorContext& ctx, EntityId e) = 0;  // widgets + edit-state
};
```

```cpp
// panels/inspector/EditState.h
#pragma once
#include "ECS.h"
#include "EditorContext.h"
#include "ApplicationContext.h"   // ECSCommandRing
#include "ECSCommands.h"
#include "lib.h"                  // SM_WARN

// Per-component working copy + entity-switch + dirty tracking. Encapsulates the
// scaffold that the monolithic inspector repeated 14 times. GUI-thread only
// (the editor draws on the ImGui overlay).
template <class T>
struct EditState {
    T        edit{};
    EntityId last = INVALID_ENTITY;
    bool     modified = false;

    // Returns the live snapshot component (nullptr if absent). On entity switch,
    // copies snapshot -> edit and clears modified. While not editing, live-refreshes
    // edit from the snapshot each frame so game-driven mutations (e.g. day/night
    // moving a light) show up. While editing (modified==true), preserves the user's
    // in-progress edit.
    const T* Begin(const EditorContext& ctx, EntityId e) {
        const T* c = ctx.WorldSnapshot->GetComponent<T>(e);
        if (!c) return nullptr;
        if (last != e) { edit = *c; last = e; modified = false; }
        else if (!modified) { edit = *c; }
        return c;
    }

    // Pushes a ModifyComponent command iff modified, then clears the flag.
    void Commit(const EditorContext& ctx, EntityId e) {
        if (!modified) return;
        modified = false;
        if (!ctx.App->ECSCommandRing.Push(ECSCommand::ModifyComponent(e, edit)))
            SM_WARN("ECS command queue full! Modify command dropped.");
    }
};
```

Concrete editor pattern (trivial example):

```cpp
// panels/inspector/NavTargetEditor.{h,cpp}
class NavTargetEditor final : public IComponentEditor {
    EditState<NavTargetComponent> m_St;
public:
    const char* Label() const override { return "NavMesh Target"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<NavTargetComponent>(e); }
    void AddDefault(const EditorContext& c, EntityId e) override {
        c.App->ECSCommandRing.Push(ECSCommand::AddComponent(e, NavTargetComponent{}));
    }
    void Remove(const EditorContext& c, EntityId e) override {
        c.App->ECSCommandRing.Push(ECSCommand::RemoveComponent<NavTargetComponent>(e));
    }
    void DrawEditor(const EditorContext& ctx, EntityId e) override {
        if (!m_St.Begin(ctx, e)) return;
        if (ImGui::InputFloat3("Destination", &m_St.edit.Destination.x)) m_St.modified = true;
        m_St.Commit(ctx, e);
    }
};
```

Tag-component editor (SunMarker — no fields, no EditState, no Commit):

```cpp
// panels/inspector/SunMarkerEditor.{h,cpp}
class SunMarkerEditor final : public IComponentEditor {
public:
    const char* Label() const override { return "Sun Marker"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<SunMarker>(e); }
    void AddDefault(const EditorContext& c, EntityId e) override {
        c.App->ECSCommandRing.Push(ECSCommand::AddComponent(e, SunMarker{}));
    }
    void Remove(const EditorContext& c, EntityId e) override {
        c.App->ECSCommandRing.Push(ECSCommand::RemoveComponent<SunMarker>(e));
    }
    void DrawEditor(const EditorContext&, EntityId) override {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Tagged as Sun");
        ImGui::TextDisabled("Day/night cycle drives this entity.");
    }
};
```

---

## The 15 editors (registry order = current UX order)

| # | Editor | Notes |
|---|---|---|
| 1 | `TransformEditor` | Owns `GizmoController m_Gizmo` (moved off the panel); reads `ctx.EditorCam*` / `WorldCameraComponent`; ImGuizmo decompose. Most complex. |
| 2 | `LightningEditor` | Live-refresh exposes day/night cycle changes. |
| 3 | `MeshEditor` | Mesh dropdown via `ctx.MeshSys`; thumbnail via `ctx.Preview`. |
| 4 | `MaterialEditor` | Material dropdown via `ctx.MatSys`; texture flag; `ctx.Preview`. |
| 5 | `TextEditor` | String buffer edit. |
| 6 | `SunMarkerEditor` | Tag — no EditState/Commit; 2 info-text lines. |
| 7 | `PlayerEditor` | |
| 8 | `UIRectEditor` | |
| 9 | `StateScopeEditor` | Per-state checkboxes (bitmask `GameStateId`). |
| 10 | `MenuButtonEditor` | Action combo (None/Play/Quit/Back). |
| 11 | `ColliderEditor` | Shape combo (Box/Sphere/Capsule). |
| 12 | `NavMeshSourceEditor` | Geometry combo (`-- choose --`/Collider/Mesh). |
| 13 | `NavObstacleEditor` | Shape combo (Cylinder/Box). |
| 14 | `NavAgentEditor` | |
| 15 | `NavTargetEditor` | Trivial (1 float3). |

Each is a `*Editor.{h,cpp}` pair. The widget bodies are moved verbatim from the monolith (only the `editX`/`lastEditedXEntity`/`xModified` references rewrite to `m_St.edit`/`m_St.Begin`/`m_St.modified`, and the entity-switch+refresh+command scaffold is replaced by `Begin`/`Commit`). No widget logic changes.

---

## EcsInspectorPanel after decomposition

```cpp
// panels/EcsInspectorPanel.h (shell)
class EcsInspectorPanel {
public:
    EcsInspectorPanel();                          // builds m_Editors in order
    void Draw(const EditorContext& ctx);
    void SetSelectedEntity(EntityId e) { selectedEntity = e; }
    EntityId GetSelectedEntity() const { return selectedEntity; }
private:
    EntityId selectedEntity = INVALID_ENTITY;
    std::vector<std::unique_ptr<IComponentEditor>> m_Editors;
};
```

All 14 `editX` / `lastEditedXEntity` / `xModified` triplets and the `GizmoController m_Gizmo` member are **removed** from the header (they migrate into the editor objects). `Draw()` keeps the entity-list browse + create + right-click context menu (now 2 registry loops for add/remove) + selected-entity editor section (1 registry loop). Target: ~1119 → ~250 lines.

`Draw()` builds the snapshot reference once (`const ECS& snap = *ctx.WorldSnapshot;`) and passes it to the `Has` checks; `ctx` flows to `AddDefault`/`Remove`/`DrawEditor` unchanged.

---

## File changes

**New (`src/editor/src/panels/inspector/`):**
- `IComponentEditor.h`
- `EditState.h`
- 15 × `*Editor.h` + `*Editor.cpp`

**Modified:**
- `src/editor/src/panels/EcsInspectorPanel.h` — shell (drop triplets + gizmo, add registry vector + ctor).
- `src/editor/src/panels/EcsInspectorPanel.cpp` — shell Draw() (entity list + 3 registry loops); widget bodies removed (moved to editors).
- `src/editor/CMakeLists.txt` — add the 15 `*Editor.cpp` to the editor source list; add `src/panels/inspector` to `target_include_directories`.

**New test:**
- `tests/test_inspector_editstate.cpp` + `tests/CMakeLists.txt` target.

---

## Testing

**Unit test `EditState<T>::Begin`** — the trickiest extracted logic, ImGui-free. Construct a test `ECS`, add a component, publish a snapshot, build a lightweight `EditorContext{ .WorldSnapshot = snap }` (App may be null — Begin never touches it), then assert:

- **T01 — entity-switch reset:** `Begin` on entity A copies snapshot→edit, sets `last=A`, `modified=false`. Switch to entity B → re-copies, `last=B`.
- **T02 — live-refresh while not modified:** mutate the snapshot's component value, call `Begin` again (same entity, `modified==false`) → `edit` reflects the new snapshot value.
- **T03 — preserve while modified:** set `modified=true`, change `edit`, mutate the snapshot, call `Begin` (same entity) → `edit` keeps the user's in-progress value (NOT overwritten by snapshot).
- **T04 — absent component:** `Begin` on an entity lacking T returns `nullptr`.

Use a trivially-editable component for the test (e.g. `NavTargetComponent` — 1 `glm::vec3`). `Commit` (command-ring push) + the ImGui widget bodies are verified by **GUI smoke**, since the codebase has no ImGui test harness (existing editor tests cover pure logic only).

**Regression:** existing editor-linked tests (`test_editorprefs`, `test_editorcam`, `test_metrichistory`, `test_transientstatus`) + full suite stay green. Editor builds clean.

**GUI smoke (user):** every component editor opens, edits, and round-trips a `ModifyComponent` exactly as before; add/remove via right-click context menu works for all 15; Transform gizmo manipulates + decomposes; Mesh/Material dropdowns + thumbnails render; day/night live-refresh still shows in Lightning/Transform while not editing.

---

## Risks

1. **UX-order regression.** The editor display order is defined by the registry `push_back` sequence. Mitigation: register in the exact current order (table above). Reviewer diffs the order against the pre-refactor file.
2. **Per-editor state lifetime.** The former panel-member triplets persist across frames; the editor objects (owned by the registry vector, alive for the panel's lifetime) preserve that. Each `DrawEditor` call reuses the same `EditState` instance — verify editors are NOT recreated per frame.
3. **Tag-component (SunMarker).** No `EditState`/`Commit` — `DrawEditor` only draws text. The interface permits it (the body is free to do nothing edit-related). Confirmed against the current SunMarker block (2 info lines).
4. **Mesh/Material editor dependencies.** Their dropdowns + thumbnails read `ctx.MeshSys`/`ctx.MatSys`/`ctx.Preview`. These are the same `ctx` fields the monolith used — no new dependency, same null-safety as today.
5. **Big diff (32 new files).** Mechanical per editor (widget body moved verbatim + scaffold swapped for Begin/Commit). EcsInspectorPanel.cpp shrinks ~870 lines. Reviewer focuses on: (a) no widget-logic change, (b) registry order, (c) EditState semantics match the old reset/refresh/command behavior exactly.
6. **`#include "imgui.h"` per editor cpp.** Each `*Editor.cpp` needs ImGui + glm + the component header + EditState.h. Straightforward; the include set mirrors what EcsInspectorPanel.cpp pulled.

---

## Out-of-scope (explicit)

- **X-macro / CRTP registry** (Options 2/3 of the brainstorm) — chose explicit interface + per-file editors for readability + file isolation.
- **New editor features / widget changes** — pure move; widgets are byte-for-byte behavior-identical.
- **Entity-list browse/create/delete extraction** — stays in the shell; it isn't the per-component repetition pain.
- **Auto-registration from the ECS X-macro** — editors are registered explicitly (not every ECS component is inspector-editable; e.g. singletons like `InputStateComponent`, `ViewportComponent` are excluded). Explicit registry is the intentional editable allow-list.
- **ImGui test harness** — not introduced; widget bodies stay GUI-smoke-verified.

---

## Commit estimate

5-7 commits:
1. `IComponentEditor.h` + `EditState.h` + `test_inspector_editstate.cpp` (TDD: EditState first, tests green).
2-5. Editor extraction in batches (e.g. core: Transform/Lightning/Mesh/Material/Text/SunMarker; gameplay: Player/UIRect/StateScope/MenuButton; nav: Collider/NavSource/NavObstacle/NavAgent/NavTarget) — each batch moves widget bodies + wires into the registry, building green.
6. Shell finalization: strip the migrated triplets/gizmo from EcsInspectorPanel, collapse the 3 sections to registry loops.
7. CMake + final verification.

The plan will decide exact batching; each commit must build green (the registry can hold a partial editor set while the monolith's remaining blocks still compile, IF the shell keeps un-migrated blocks until their editor lands — OR migrate all at once. Plan resolves the green-build sequencing).
