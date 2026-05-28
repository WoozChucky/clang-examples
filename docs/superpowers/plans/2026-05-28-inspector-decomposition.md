# EcsInspectorPanel Decomposition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decompose the 1119-line `src/editor/src/panels/EcsInspectorPanel.cpp` into an `IComponentEditor` interface + a registry of 15 per-component editor objects under `src/editor/src/panels/inspector/`, with zero behavior change.

**Architecture:** Each editable component gets a `*Editor.{h,cpp}` implementing `IComponentEditor` (Label/Has/AddDefault/Remove/DrawEditor). The inspector holds `std::vector<std::unique_ptr<IComponentEditor>>` built in its constructor (current UX order) and iterates it for the 3 touchpoints (add-menu, remove-menu, editor-section). An `EditState<T>` helper centralizes the repeated entity-switch/refresh/dirty/command scaffold. Migration is incremental, front-to-back in UX order, so each commit builds green with the registry rendering the migrated prefix and the monolith's inline blocks rendering the un-migrated suffix — a component is in EITHER the registry OR an inline block, never both.

**Tech Stack:** C++23, ImGui, ImGuizmo, GLM, custom ECS, ECSCommands ring.

**Spec reference:** `docs/superpowers/specs/2026-05-28-inspector-decomposition-design.md` (commit `93863a0`).

---

## Codebase orientation (read once before Task 1)

- **Branch `feat/inspector-decomposition` exists**, spec at `93863a0`. Builds on Spec A (merged `731901d`): editor src is in topic dirs; `EcsInspectorPanel.{cpp,h}` lives in `src/editor/src/panels/`.
- **Current monolith structure** (`src/editor/src/panels/EcsInspectorPanel.cpp`, 1119 lines):
  - Lines 1-19: includes.
  - `Draw()` opens at 21. Entity-creation section (~31-58), entity-list loop start (~60-74).
  - Right-click context menu (~75-409): "Delete Entity", "Duplicate Entity", then **"Add component options"** (~95-190: one `if (!HasComponent<X>) { MenuItem("Add X") { ...construct...; AddComponent } }` per component) and **"Remove component options"** (~193-300+: one `if (HasComponent<X>) { MenuItem("Remove X") { RemoveComponent<X> } }` per component).
  - Selected-entity editor section (~410-1103): one `// Edit X Component` block per component.
- **Anchors for migration** (robust against line-shift — search these, don't rely on line numbers):
  - Editor blocks: the comment `// Edit <Name> Component` (e.g. `// Edit Mesh Component`), and for SunMarker the `if (ctx.WorldSnapshot->HasComponent<SunMarker>(selectedEntity))` at the "Sun Marker" header.
  - Add-menu blocks: under `// Add component options`, each `if (!ctx.WorldSnapshot->HasComponent<X>(entity))`.
  - Remove-menu blocks: under `// Remove component options`, each `if (ctx.WorldSnapshot->HasComponent<X>(entity))`.
- **CRITICAL — AddDefault is NOT always `T{}`.** Several add-menu blocks construct authoring defaults, e.g.:
  - Lightning: `Type=Directional; Direction=(0,-1,0,0); Color=vec4(1); Intensity=1`.
  - Mesh: `MeshId=0; Visible=false`.
  - Material: `MaterialId=0; BaseColor=vec4(1); Flags=0`.
  - Text: `Text="Sample text"`.
  Each editor's `AddDefault` must replicate the EXACT construction from the monolith's add-block verbatim — NOT a bare `T{}`. (Transform's block sets explicit identity, which may equal `T{}` — copy it verbatim regardless.)
- **Per-component editor state** in the header today: 14 triplets `editX` / `lastEditedXEntity` / `xModified` + `GizmoController m_Gizmo`. Each migrates into its editor as an `EditState<T> m_St` member (Transform also gets the `GizmoController`). SunMarker has no triplet (tag).
- **EditorContext** (`src/editor/src/app/EditorContext.h`) carries everything editors need: `App` (command ring), `MeshSys`, `MatSys`, `Preview`, `World`, `WorldSnapshot`, `Snapshot`, `EditorCameraActive`/`EditorCamView`/`EditorCamProj`. The `DrawEditor(ctx,e)` signature suffices for all editors.
- **Editor display order (UX) — DO NOT change:** Transform, Lightning, Mesh, Material, Text, SunMarker, Player, UIRect, StateScope, MenuButton, Collider, NavMeshSource, NavObstacle, NavAgent, NavTarget.
- **ECS snapshot in tests:** `ECS::CreateSnapshot()` returns `std::shared_ptr<const ECS>` (per test_navagent precedent). `EditorContext` is an aggregate — `EditorContext{}` then assign `.WorldSnapshot`.
- **Build preset:** `msvc-win64-vs2026-community`. Reconfigure after adding source files / include dirs.

---

## Task 0: Verify branch state

**Files:** none (git only)

- [ ] **Step 1: Confirm branch + baseline build**

```bash
git status -sb
# Expected: "## feat/inspector-decomposition" — clean.
git log --oneline -1
# Expected: 93863a0 docs(editor): design spec for EcsInspectorPanel decomposition (Spec B)
cmake --build out/build/msvc-win64-vs2026-community --target editor --config Debug 2>&1 | tail -3
# Expected: clean build (baseline before refactor).
```

---

## Task 1: Infrastructure — IComponentEditor + EditState + test (TDD)

**Files:**
- Create: `src/editor/src/panels/inspector/IComponentEditor.h`
- Create: `src/editor/src/panels/inspector/EditState.h`
- Create: `tests/test_inspector_editstate.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `IComponentEditor.h`**

```cpp
// src/editor/src/panels/inspector/IComponentEditor.h
#pragma once
#include "ECS.h"   // EntityId

struct EditorContext;
class  ECS;

// One editor per inspector-editable component type. Registered in
// EcsInspectorPanel's constructor (in display order) and iterated for the
// add-menu, remove-menu, and editor sections. Each concrete editor owns its
// own per-frame EditState. GUI-thread only (drawn on the ImGui overlay).
class IComponentEditor {
public:
    virtual ~IComponentEditor() = default;
    virtual const char* Label() const = 0;                              // menu + header text
    virtual bool Has(const ECS& snap, EntityId e) const = 0;            // HasComponent<T>
    virtual void AddDefault(const EditorContext& ctx, EntityId e) = 0;  // push AddComponent
    virtual void Remove(const EditorContext& ctx, EntityId e) = 0;      // push RemoveComponent<T>
    virtual void DrawEditor(const EditorContext& ctx, EntityId e) = 0;  // widgets + edit-state
};
```

- [ ] **Step 2: Create `EditState.h`**

```cpp
// src/editor/src/panels/inspector/EditState.h
#pragma once
#include "ECS.h"
#include "EditorContext.h"
#include "ApplicationContext.h"   // ECSCommandRing
#include "ECSCommands.h"
#include "lib.h"                  // SM_WARN

// Per-component working copy + entity-switch + dirty tracking. Encapsulates the
// scaffold the monolithic inspector repeated 14 times. GUI-thread only.
template <class T>
struct EditState {
    T        edit{};
    EntityId last = INVALID_ENTITY;
    bool     modified = false;

    // Returns the live snapshot component (nullptr if absent). On entity switch,
    // copies snapshot -> edit and clears modified. While not editing, live-refreshes
    // edit from the snapshot each frame (so game-driven mutations show up). While
    // editing (modified == true), preserves the user's in-progress edit.
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

- [ ] **Step 3: Write the failing test**

```cpp
// tests/test_inspector_editstate.cpp
#include <cstdio>
#include <cstdlib>

#include <glm/glm.hpp>

#include "ECS.h"
#include "EditorContext.h"
#include "EditState.h"

void platform_debug_break(const char* expr, const char* file, int line, const char* message) {
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 (file ? file : "<unknown>"), line,
                 (message ? message : "<no message>"), (expr ? expr : "<none>"));
    std::abort();
}

static int g_Failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                     \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_Failures;                                                  \
    } } while (0)

// Build a snapshot-backed EditorContext. App stays null — Begin never touches it.
static EditorContext CtxFor(std::shared_ptr<const ECS>& snap) {
    EditorContext ctx{};
    ctx.WorldSnapshot = snap;
    return ctx;
}

static void T01_entity_switch_resets() {
    ECS w;
    const EntityId a = w.CreateEntity();
    w.AddComponent(a, NavTargetComponent{ glm::vec3(1, 2, 3) });
    const EntityId b = w.CreateEntity();
    w.AddComponent(b, NavTargetComponent{ glm::vec3(7, 8, 9) });
    auto snap = w.CreateSnapshot();
    auto ctx = CtxFor(snap);

    EditState<NavTargetComponent> st;
    const auto* ca = st.Begin(ctx, a);
    EXPECT(ca != nullptr);
    EXPECT(st.last == a);
    EXPECT(st.edit.Destination == glm::vec3(1, 2, 3));

    // Pretend user edited, then switch entities -> reset to b's snapshot value, modified cleared.
    st.modified = true;
    st.edit.Destination = glm::vec3(99, 99, 99);
    const auto* cb = st.Begin(ctx, b);
    EXPECT(cb != nullptr);
    EXPECT(st.last == b);
    EXPECT(st.modified == false);
    EXPECT(st.edit.Destination == glm::vec3(7, 8, 9));
}

static void T02_live_refresh_while_not_modified() {
    ECS w;
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, NavTargetComponent{ glm::vec3(1, 0, 0) });
    auto snap1 = w.CreateSnapshot();
    auto ctx1 = CtxFor(snap1);

    EditState<NavTargetComponent> st;
    st.Begin(ctx1, e);
    EXPECT(st.edit.Destination == glm::vec3(1, 0, 0));

    // Game mutates the component; a fresh snapshot reflects it. Not modified -> refresh.
    w.Modify<NavTargetComponent>(e, [](NavTargetComponent& t){ t.Destination = glm::vec3(5, 0, 0); });
    auto snap2 = w.CreateSnapshot();
    auto ctx2 = CtxFor(snap2);
    st.Begin(ctx2, e);
    EXPECT(st.edit.Destination == glm::vec3(5, 0, 0));   // refreshed
}

static void T03_preserve_while_modified() {
    ECS w;
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, NavTargetComponent{ glm::vec3(1, 0, 0) });
    auto snap1 = w.CreateSnapshot();
    auto ctx1 = CtxFor(snap1);

    EditState<NavTargetComponent> st;
    st.Begin(ctx1, e);

    // User is editing.
    st.modified = true;
    st.edit.Destination = glm::vec3(42, 0, 0);

    // Snapshot changes underneath, but modified==true -> preserve user edit.
    w.Modify<NavTargetComponent>(e, [](NavTargetComponent& t){ t.Destination = glm::vec3(5, 0, 0); });
    auto snap2 = w.CreateSnapshot();
    auto ctx2 = CtxFor(snap2);
    st.Begin(ctx2, e);
    EXPECT(st.edit.Destination == glm::vec3(42, 0, 0));  // preserved, NOT overwritten
}

static void T04_absent_component_returns_null() {
    ECS w;
    const EntityId e = w.CreateEntity();  // no NavTargetComponent
    auto snap = w.CreateSnapshot();
    auto ctx = CtxFor(snap);

    EditState<NavTargetComponent> st;
    EXPECT(st.Begin(ctx, e) == nullptr);
}

int main() {
    T01_entity_switch_resets();
    T02_live_refresh_while_not_modified();
    T03_preserve_while_modified();
    T04_absent_component_returns_null();

    if (g_Failures == 0) { std::printf("All inspector EditState tests passed.\n"); return 0; }
    std::fprintf(stderr, "inspector EditState tests: %d failure(s)\n", g_Failures);
    return 1;
}
```

- [ ] **Step 4: Add the test target to `tests/CMakeLists.txt`**

Append:

```cmake
add_executable(test_inspector_editstate
    test_inspector_editstate.cpp
)

target_link_libraries(test_inspector_editstate PRIVATE
    CommonHeaders
    glm::glm
    ecs
    nlohmann_json::nlohmann_json
)

target_include_directories(test_inspector_editstate PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/include
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
    ${CMAKE_SOURCE_DIR}/src/editor/src/app
    ${CMAKE_SOURCE_DIR}/src/editor/src/panels/inspector
)

target_compile_definitions(test_inspector_editstate PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_inspector_editstate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

Rationale for include dirs: `EditState.h` includes `EditorContext.h` (in `app/`), `ApplicationContext.h` + `ECSCommands.h` + `ECS.h` (common/include), and `RenderStats.h` is pulled by `EditorContext.h`'s transitive deps via `engine/src/rendering` — mirror test_editorprefs's engine include set. `nlohmann_json` is needed because `ECSCommands.h`/`EditorContext.h` transitively include serialization. If the link fails on an undefined symbol from `ApplicationContext`, the test only needs the header-only ring `Push` (templated, header-defined) — no extra link target expected; if a missing symbol appears, report it.

- [ ] **Step 5: Configure + build + run — expect PASS (EditState is already correct)**

```bash
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_inspector_editstate --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_inspector_editstate.exe
```

Expected: `All inspector EditState tests passed.` (This is TDD for the EXTRACTED logic — EditState is written correct-first because its semantics are copied verbatim from the proven monolith scaffold; the test pins them so later edits can't regress. If any test fails, the Begin logic doesn't match the spec's reset/refresh/preserve contract — fix EditState.h.)

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/panels/inspector/IComponentEditor.h src/editor/src/panels/inspector/EditState.h tests/test_inspector_editstate.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): IComponentEditor interface + EditState<T> + tests

Infrastructure for the inspector decomposition (Spec B). IComponentEditor
is the per-component editor interface (Label/Has/AddDefault/Remove/
DrawEditor); EditState<T> encapsulates the entity-switch reset +
live-refresh-while-not-editing + dirty -> ModifyComponent scaffold the
monolith repeated 14 times.

test_inspector_editstate pins EditState::Begin semantics (ImGui-free):
T01 entity-switch reset, T02 live-refresh while not modified, T03
preserve while modified, T04 absent component -> nullptr.

No editors yet + monolith untouched -> editor builds unchanged. Editors
land in the following tasks."
```

---

## Task 2: Migrate Batch A (core 6) + shell registry scaffolding

**Files:**
- Create: `src/editor/src/panels/inspector/{Transform,Lightning,Mesh,Material,Text,SunMarker}Editor.{h,cpp}` (12 files)
- Modify: `src/editor/src/panels/EcsInspectorPanel.h`
- Modify: `src/editor/src/panels/EcsInspectorPanel.cpp`
- Modify: `src/editor/CMakeLists.txt`

This task establishes the registry + the 3 transitional loops, then migrates the first 6 components. After it, the registry renders Transform..SunMarker and the monolith's inline blocks render Player..NavTarget.

- [ ] **Step 1: Add the registry to `EcsInspectorPanel.h` (incremental — keep un-migrated triplets)**

The header changes are INCREMENTAL across the three batch tasks. In Task 2: add the registry vector + ctor, remove `GizmoController m_Gizmo` (migrates into TransformEditor), remove ONLY the Batch-A triplets (Transform/Lightning/Mesh/Material/Text — SunMarker has none), and KEEP the Batch B/C triplets (Player..NavTarget), because their inline blocks still reference them this task. Task 3 removes the Batch-B triplets; Task 4 removes the Batch-C triplets, leaving only `selectedEntity` + `m_Editors`.

Drop the old `#include "GizmoController.h"` from the header (no longer needed). Replace `src/editor/src/panels/EcsInspectorPanel.h` with:

```cpp
#pragma once
#include <memory>
#include <vector>
#include "ECS.h"
#include "inspector/IComponentEditor.h"

struct EditorContext;

class EcsInspectorPanel {
public:
    EcsInspectorPanel();
    void Draw(const EditorContext& ctx);
    void SetSelectedEntity(EntityId e) { selectedEntity = e; }
    EntityId GetSelectedEntity() const { return selectedEntity; }
private:
    EntityId selectedEntity = INVALID_ENTITY;
    std::vector<std::unique_ptr<IComponentEditor>> m_Editors;

    // --- Un-migrated component edit-state (removed as each batch lands) ---
    // Batch B (Task 3):
    PlayerComponent    editPlayer{};        EntityId lastEditedPlayerEntity = INVALID_ENTITY;    bool playerModified = false;
    UIRectComponent    editUIRect{};        EntityId lastEditedUIRectEntity = INVALID_ENTITY;    bool uiRectModified = false;
    StateScopeComponent editScope{};        EntityId lastEditedScopeEntity = INVALID_ENTITY;     bool scopeModified = false;
    MenuButtonComponent editMenuBtn{};      EntityId lastEditedMenuBtnEntity = INVALID_ENTITY;   bool menuBtnModified = false;
    // Batch C (Task 4):
    ColliderComponent  editCollider{};      EntityId lastEditedColliderEntity = INVALID_ENTITY;  bool colliderModified = false;
    NavMeshSourceComponent editNavSource{}; EntityId lastEditedNavSourceEntity = INVALID_ENTITY;  bool navSourceModified = false;
    NavObstacleComponent editNavObstacle{}; EntityId lastEditedNavObstacleEntity = INVALID_ENTITY; bool navObstacleModified = false;
    NavAgentComponent  editNavAgent{};      EntityId lastEditedNavAgentEntity = INVALID_ENTITY;  bool navAgentModified = false;
    NavTargetComponent editNavTarget{};     EntityId lastEditedNavTargetEntity = INVALID_ENTITY; bool navTargetModified = false;
};
```

(Add `#include "ECS.h"` already covers the component types. The header now needs no `GizmoController.h` include — that moves to TransformEditor.h. Keep the migrated-batch members OUT; keep un-migrated members IN. Task 3 deletes the Batch B members; Task 4 deletes the Batch C members, leaving only `selectedEntity` + `m_Editors`.)

- [ ] **Step 2: Create the 6 Batch-A editor files**

For each component, create `<Name>Editor.h` + `<Name>Editor.cpp` in `src/editor/src/panels/inspector/`. The `.h` declares the class; the `.cpp` implements it by moving the monolith's widget body verbatim. Worked examples for the 3 archetypes follow; apply the same recipe to the rest.

**Recipe (apply to every editor):**
1. `Label()` returns the exact header string the monolith used (e.g. `"Transform Component"` — match the `CollapsingHeader` text precisely).
2. `Has(snap,e)` returns `snap.HasComponent<T>(e)`.
3. `AddDefault(ctx,e)` — copy the EXACT construction from the monolith's `// Add component options` block for this type (verbatim field assignments), then `ctx.App->ECSCommandRing.Push(ECSCommand::AddComponent(e, newX))` with the same SM_WARN-on-full.
4. `Remove(ctx,e)` — `ctx.App->ECSCommandRing.Push(ECSCommand::RemoveComponent<T>(e))` with the SM_WARN-on-full (match the monolith's remove block).
5. `DrawEditor(ctx,e)` — move the monolith's `// Edit X Component` body verbatim, with these mechanical rewrites:
   - Replace the entity-switch reset + live-refresh prologue (`if (lastEditedXEntity != selectedEntity) {...} if (!xModified) {...}`) with `const auto* c = m_St.Begin(ctx, e); if (!c) return;`.
   - Replace `editX` → `m_St.edit`, `xModified` → `m_St.modified`, `selectedEntity` → `e`, `ctx.WorldSnapshot->GetComponent<X>(...)` (the raw fetch) → use `c` (or `m_St.edit`).
   - Replace the trailing dirty→command tail (`if (xModified) { xModified=false; Push(ModifyComponent(...)); }`) with `m_St.Commit(ctx, e);`.
   - The `ImGui::CollapsingHeader(...)` wrapper is handled by the registry loop (Step 4) — `DrawEditor` is called only when the header is open. So the editor body starts INSIDE the header (drop the `if (CollapsingHeader)` from the moved body; keep its contents).

**Worked example — trivial (use as the template for simple field editors):**

```cpp
// src/editor/src/panels/inspector/TextEditor.h
#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class TextEditor final : public IComponentEditor {
    EditState<TextComponent> m_St;
public:
    const char* Label() const override { return "Text Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<TextComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
```

```cpp
// src/editor/src/panels/inspector/TextEditor.cpp
#include "TextEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void TextEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    TextComponent newText{};
    newText.Text = "Sample text";
    if (!ctx.App->ECSCommandRing.Push(ECSCommand::AddComponent(e, newText)))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void TextEditor::Remove(const EditorContext& ctx, EntityId e) {
    if (!ctx.App->ECSCommandRing.Push(ECSCommand::RemoveComponent<TextComponent>(e)))
        SM_WARN("ECS command queue full! Remove command dropped.");
}
void TextEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    if (!m_St.Begin(ctx, e)) return;
    // <-- move the monolith's "// Edit Text Component" widget body here verbatim,
    //     rewriting editTextComp -> m_St.edit, textModified -> m_St.modified.
    m_St.Commit(ctx, e);
}
```

**Worked example — tag (SunMarker, no EditState/Commit):**

```cpp
// src/editor/src/panels/inspector/SunMarkerEditor.h
#pragma once
#include "IComponentEditor.h"
class SunMarkerEditor final : public IComponentEditor {
public:
    const char* Label() const override { return "Sun Marker"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<SunMarker>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
```

```cpp
// src/editor/src/panels/inspector/SunMarkerEditor.cpp
#include "SunMarkerEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void SunMarkerEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    if (!ctx.App->ECSCommandRing.Push(ECSCommand::AddComponent(e, SunMarker{})))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void SunMarkerEditor::Remove(const EditorContext& ctx, EntityId e) {
    if (!ctx.App->ECSCommandRing.Push(ECSCommand::RemoveComponent<SunMarker>(e)))
        SM_WARN("ECS command queue full! Remove command dropped.");
}
void SunMarkerEditor::DrawEditor(const EditorContext&, EntityId) {
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Tagged as Sun");
    ImGui::TextDisabled("Day/night cycle drives this entity.");
}
```

**Worked example — complex (Transform owns the gizmo):**

```cpp
// src/editor/src/panels/inspector/TransformEditor.h
#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
#include "GizmoController.h"   // viewport/ is on the editor include path (Spec A)
class TransformEditor final : public IComponentEditor {
    EditState<TransformComponent> m_St;
    GizmoController m_Gizmo;
public:
    const char* Label() const override { return "Transform Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<TransformComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
```

```cpp
// src/editor/src/panels/inspector/TransformEditor.cpp
#include "TransformEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/glm.hpp>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "TransformMath.h"   // ModelMatrix
#include "lib.h"

void TransformEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    TransformComponent newTransform{};
    newTransform.Position = glm::vec3(0.0f);
    newTransform.Rotation = glm::vec3(0.0f);
    newTransform.Scale    = glm::vec3(1.0f);
    if (!ctx.App->ECSCommandRing.Push(ECSCommand::AddComponent(e, newTransform)))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void TransformEditor::Remove(const EditorContext& ctx, EntityId e) {
    if (!ctx.App->ECSCommandRing.Push(ECSCommand::RemoveComponent<TransformComponent>(e)))
        SM_WARN("ECS command queue full! Remove command dropped.");
}
void TransformEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    if (!m_St.Begin(ctx, e)) return;
    // <-- move the monolith's "// Edit Transform Component" body here verbatim:
    //     position/rotation/scale InputFloat3, the gizmo block (uses m_Gizmo,
    //     ctx.EditorCameraActive/EditorCamView/EditorCamProj or WorldCameraComponent,
    //     ImGuizmo decompose), the "* Modified" text. Rewrite editTransform -> m_St.edit,
    //     transformModified -> m_St.modified, selectedEntity -> e, m_Gizmo stays m_Gizmo.
    m_St.Commit(ctx, e);
}
```

**Apply the recipe to the remaining Batch-A editors:** `LightningEditor` (AddDefault sets Type=Directional, Direction=(0,-1,0,0), Color=vec4(1), Intensity=1; body moves the Lightning widget block), `MeshEditor` (AddDefault MeshId=0/Visible=false; body uses `ctx.MeshSys` for the mesh dropdown + `ctx.Preview` for thumbnails — move verbatim), `MaterialEditor` (AddDefault MaterialId=0/BaseColor=vec4(1)/Flags=0; body uses `ctx.MatSys`/`ctx.Preview`). Each `.cpp` includes what its body references (`MeshSystem.h`/`MaterialSystem.h` for Mesh/Material; `MeshPreviewRenderer.h` if the thumbnail call needs the full type).

- [ ] **Step 3: Build the editor registry in the ctor**

Create `src/editor/src/panels/EcsInspectorPanel.cpp` ctor (add near the top, after includes — add the 6 editor includes):

```cpp
#include "inspector/TransformEditor.h"
#include "inspector/LightningEditor.h"
#include "inspector/MeshEditor.h"
#include "inspector/MaterialEditor.h"
#include "inspector/TextEditor.h"
#include "inspector/SunMarkerEditor.h"

EcsInspectorPanel::EcsInspectorPanel() {
    // Registry order == display order. Batches B/C append here in Tasks 3-4.
    m_Editors.push_back(std::make_unique<TransformEditor>());
    m_Editors.push_back(std::make_unique<LightningEditor>());
    m_Editors.push_back(std::make_unique<MeshEditor>());
    m_Editors.push_back(std::make_unique<MaterialEditor>());
    m_Editors.push_back(std::make_unique<TextEditor>());
    m_Editors.push_back(std::make_unique<SunMarkerEditor>());
}
```

- [ ] **Step 4: Wire the 3 transitional registry loops into `Draw()`**

In the context-menu's **"Add component options"** region, insert the registry loop at the TOP (before the remaining inline add-blocks), then DELETE the inline add-blocks for Transform/Lightning/Mesh/Material/Text/SunMarker:

```cpp
                // Add component options
                for (auto& ed : m_Editors) {
                    if (!ed->Has(*ctx.WorldSnapshot, entity)) {
                        char lbl[96]; snprintf(lbl, sizeof(lbl), "Add %s", ed->Label());
                        if (ImGui::MenuItem(lbl)) ed->AddDefault(ctx, entity);
                    }
                }
                // (remaining inline "Add X" blocks for Player..NavTarget stay below, untouched)
```

In the **"Remove component options"** region, same pattern at the TOP, then DELETE the inline remove-blocks for the 6 migrated types:

```cpp
                // Remove component options
                for (auto& ed : m_Editors) {
                    if (ed->Has(*ctx.WorldSnapshot, entity)) {
                        char lbl[96]; snprintf(lbl, sizeof(lbl), "Remove %s", ed->Label());
                        if (ImGui::MenuItem(lbl)) ed->Remove(ctx, entity);
                    }
                }
                // (remaining inline "Remove X" blocks for Player..NavTarget stay below)
```

In the **selected-entity editor section** (after the entity-list loop, where `selectedEntity` is valid), insert the registry loop at the TOP (before the remaining inline `// Edit X Component` blocks), then DELETE the inline editor blocks for the 6 migrated types:

```cpp
        if (selectedEntity != INVALID_ENTITY && ctx.WorldSnapshot->IsValidEntity(selectedEntity)) {
            for (auto& ed : m_Editors) {
                if (ed->Has(*ctx.WorldSnapshot, selectedEntity)) {
                    if (ImGui::CollapsingHeader(ed->Label(), ImGuiTreeNodeFlags_DefaultOpen))
                        ed->DrawEditor(ctx, selectedEntity);
                }
            }
            // (remaining inline "// Edit X Component" blocks for Player..NavTarget stay below)
        }
```

Match the existing guard the monolith used for the editor section (it likely already wraps in `if (selectedEntity != INVALID_ENTITY ...)` — reuse the exact guard; don't introduce a new one). Migrated inline blocks (Transform..SunMarker) are deleted; the un-migrated suffix (Player..NavTarget) stays. Order is preserved because migrated types are the display-order prefix.

Remove now-unused includes/members only when fully unused: `GizmoController.h` include + `m_Gizmo` are gone from the header (Step 1); ensure the .cpp no longer references `m_Gizmo` (it moved to TransformEditor).

- [ ] **Step 5: Add Batch-A sources + include dir to `src/editor/CMakeLists.txt`**

In the `add_executable(editor ...)` list, add under a new comment group:

```cmake
    # Inspector component editors (Spec B)
    src/panels/inspector/TransformEditor.cpp
    src/panels/inspector/LightningEditor.cpp
    src/panels/inspector/MeshEditor.cpp
    src/panels/inspector/MaterialEditor.cpp
    src/panels/inspector/TextEditor.cpp
    src/panels/inspector/SunMarkerEditor.cpp
```

In `target_include_directories(editor PRIVATE ...)`, add:

```cmake
    src/panels/inspector
```

- [ ] **Step 6: Reconfigure + build + test**

```bash
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor test_inspector_editstate --config Debug 2>&1 | tail -15
./out/build/msvc-win64-vs2026-community/bin/Debug/test_inspector_editstate.exe
```

Expected: clean editor build; EditState tests pass. The editor now renders Transform..SunMarker via the registry and Player..NavTarget via inline blocks — identical UX.

- [ ] **Step 7: Commit**

```bash
git add src/editor/src/panels/inspector/*Editor.h src/editor/src/panels/inspector/*Editor.cpp src/editor/src/panels/EcsInspectorPanel.h src/editor/src/panels/EcsInspectorPanel.cpp src/editor/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(editor): migrate core 6 inspector editors to registry (Spec B batch A)

Transform (owns GizmoController), Lightning, Mesh, Material, Text,
SunMarker extracted into src/editor/src/panels/inspector/*Editor.{h,cpp}
implementing IComponentEditor. EcsInspectorPanel gains the registry
vector + ctor + 3 transitional loops (add-menu/remove-menu/editor).

Migrated types render via the registry (display-order prefix);
Player..NavTarget still render via the monolith's inline blocks
(suffix) -> identical UX, green build, no double-render. Widget bodies
moved verbatim; only edit-state plumbing rewritten to EditState::
Begin/Commit. Batches B/C follow."
```

---

## Task 3: Migrate Batch B (gameplay 4)

**Files:**
- Create: `src/editor/src/panels/inspector/{Player,UIRect,StateScope,MenuButton}Editor.{h,cpp}` (8 files)
- Modify: `src/editor/src/panels/EcsInspectorPanel.{h,cpp}`, `src/editor/CMakeLists.txt`

- [ ] **Step 1: Create the 4 editor files** using the recipe from Task 2 Step 2. Notes:
  - `PlayerEditor` — straightforward field editor.
  - `UIRectEditor` — straightforward.
  - `StateScopeEditor` — body has the per-state checkbox loop over `GameStateId` bits; move verbatim (the `kStates[]` table + bitmask logic).
  - `MenuButtonEditor` — body has the action combo (`kActionNames`/`kActionIds`: None/Play/Quit/Back); move verbatim.
  Each `Label()` matches the monolith header ("Player Component", "UI Rect Component", "State Scope Component", "Menu Button Component"); each `AddDefault` copies the monolith's add-block construction verbatim.

- [ ] **Step 2: Append to the ctor registry** (after SunMarker, before nothing yet):

```cpp
#include "inspector/PlayerEditor.h"
#include "inspector/UIRectEditor.h"
#include "inspector/StateScopeEditor.h"
#include "inspector/MenuButtonEditor.h"
// ... in ctor, after SunMarker:
    m_Editors.push_back(std::make_unique<PlayerEditor>());
    m_Editors.push_back(std::make_unique<UIRectEditor>());
    m_Editors.push_back(std::make_unique<StateScopeEditor>());
    m_Editors.push_back(std::make_unique<MenuButtonEditor>());
```

- [ ] **Step 3: Delete the inline blocks** for Player/UIRect/StateScope/MenuButton from all 3 sections (add-menu, remove-menu, editor-section). The registry loops already iterate them. Remove their 4 triplets from `EcsInspectorPanel.h`.

- [ ] **Step 4: Add the 4 sources to `src/editor/CMakeLists.txt`** (under the inspector group).

- [ ] **Step 5: Reconfigure + build + test**

```bash
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor --config Debug 2>&1 | tail -10
```

Expected: clean. Registry now renders Transform..MenuButton; inline renders Collider..NavTarget.

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/panels/inspector/PlayerEditor.* src/editor/src/panels/inspector/UIRectEditor.* src/editor/src/panels/inspector/StateScopeEditor.* src/editor/src/panels/inspector/MenuButtonEditor.* src/editor/src/panels/EcsInspectorPanel.h src/editor/src/panels/EcsInspectorPanel.cpp src/editor/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(editor): migrate gameplay 4 inspector editors to registry (Spec B batch B)

Player, UIRect, StateScope (per-state checkbox bitmask), MenuButton
(action combo) extracted to inspector/*Editor.{h,cpp}; appended to the
registry in display order; inline blocks + header triplets removed.
Collider..NavTarget remain inline. Green; widget bodies verbatim."
```

---

## Task 4: Migrate Batch C (nav 5) + finalize shell

**Files:**
- Create: `src/editor/src/panels/inspector/{Collider,NavMeshSource,NavObstacle,NavAgent,NavTarget}Editor.{h,cpp}` (10 files)
- Modify: `src/editor/src/panels/EcsInspectorPanel.{h,cpp}`, `src/editor/CMakeLists.txt`

- [ ] **Step 1: Create the 5 editor files** using the recipe. Notes:
  - `ColliderEditor` — shape combo (`kShapeNames`/`kShapes`: Box/Sphere/Capsule); move verbatim.
  - `NavMeshSourceEditor` — geometry combo (`-- choose --`/Collider/Mesh, `kGeomNames`/`kGeoms`); move verbatim.
  - `NavObstacleEditor` — shape combo (Cylinder/Box); move verbatim.
  - `NavAgentEditor` — field editor (MoveSpeed/Radius/ReachedEpsilon — only the v1 tunables are editable; do NOT add widgets for the v2 cached-path runtime fields).
  - `NavTargetEditor` — single `InputFloat3("Destination", ...)` (the trivial worked example from the spec).
  Labels match monolith headers; AddDefault copies the monolith add-block verbatim.

- [ ] **Step 2: Append to ctor registry** (after MenuButton):

```cpp
#include "inspector/ColliderEditor.h"
#include "inspector/NavMeshSourceEditor.h"
#include "inspector/NavObstacleEditor.h"
#include "inspector/NavAgentEditor.h"
#include "inspector/NavTargetEditor.h"
// ... in ctor, after MenuButton:
    m_Editors.push_back(std::make_unique<ColliderEditor>());
    m_Editors.push_back(std::make_unique<NavMeshSourceEditor>());
    m_Editors.push_back(std::make_unique<NavObstacleEditor>());
    m_Editors.push_back(std::make_unique<NavAgentEditor>());
    m_Editors.push_back(std::make_unique<NavTargetEditor>());
```

- [ ] **Step 3: Delete the inline blocks** for the 5 nav types from all 3 sections. After this, ALL inline per-component blocks are gone; only the registry loops remain. Remove the 5 nav triplets from `EcsInspectorPanel.h` — the header is now just `selectedEntity` + `m_Editors`.

- [ ] **Step 4: Finalize the shell.** Confirm `EcsInspectorPanel.h` has NO leftover edit-state members:

```cpp
#pragma once
#include <memory>
#include <vector>
#include "ECS.h"
#include "inspector/IComponentEditor.h"
struct EditorContext;
class EcsInspectorPanel {
public:
    EcsInspectorPanel();
    void Draw(const EditorContext& ctx);
    void SetSelectedEntity(EntityId e) { selectedEntity = e; }
    EntityId GetSelectedEntity() const { return selectedEntity; }
private:
    EntityId selectedEntity = INVALID_ENTITY;
    std::vector<std::unique_ptr<IComponentEditor>> m_Editors;
};
```

Confirm `EcsInspectorPanel.cpp` no longer references `editX`/`xModified`/`m_Gizmo`/any removed member, and includes only what the shell + registry need (the entity-list/create code + `<imgui.h>` + the 15 editor headers + the ECS/command headers). Remove now-unused includes (e.g. `ImGuizmo.h`, `MeshSystem.h`, `MaterialSystem.h`, `TransformMath.h` moved into the editor cpps — drop them from the shell if the shell body no longer uses them).

- [ ] **Step 5: Add the 5 sources to `src/editor/CMakeLists.txt`.**

- [ ] **Step 6: Reconfigure + build + full test sweep**

```bash
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor test_inspector_editstate test_editorprefs test_editorcam test_metrichistory test_transientstatus --config Debug 2>&1 | tail -15
for t in test_inspector_editstate test_editorprefs test_editorcam test_metrichistory test_transientstatus; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "Editor-linked tests green."
```

Expected: clean editor build; all 5 tests pass.

- [ ] **Step 7: Verify the shrink + no leftover inline blocks**

```bash
wc -l src/editor/src/panels/EcsInspectorPanel.cpp
# Expected: ~230-280 lines (down from 1119).
grep -c "// Edit .* Component" src/editor/src/panels/EcsInspectorPanel.cpp
# Expected: 0 (all editor blocks migrated).
grep -cE "edit(Transform|Lightning|Mesh|Material|Text|Player|UIRect|Scope|MenuBtn|Collider|NavSource|NavObstacle|NavAgent|NavTarget)" src/editor/src/panels/EcsInspectorPanel.cpp
# Expected: 0 (no leftover edit-state refs).
```

- [ ] **Step 8: Commit**

```bash
git add src/editor/src/panels/inspector/ColliderEditor.* src/editor/src/panels/inspector/NavMeshSourceEditor.* src/editor/src/panels/inspector/NavObstacleEditor.* src/editor/src/panels/inspector/NavAgentEditor.* src/editor/src/panels/inspector/NavTargetEditor.* src/editor/src/panels/EcsInspectorPanel.h src/editor/src/panels/EcsInspectorPanel.cpp src/editor/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(editor): migrate nav 5 editors + finalize inspector shell (Spec B batch C)

Collider (shape combo), NavMeshSource (geometry combo), NavObstacle
(shape combo), NavAgent (v1 tunables), NavTarget extracted to
inspector/*Editor.{h,cpp}; appended to the registry. All inline
per-component blocks now gone; EcsInspectorPanel is a thin shell
(entity list + create/delete/duplicate + 3 registry loops),
~1119 -> ~250 lines. Header holds only selectedEntity + m_Editors.

Full per-component touchpoint consolidation complete: adding a
component now means one *Editor file + one push_back, not 3 edits
across the monolith."
```

---

## Task 5: Final review + GUI smoke handoff

- [ ] **Step 1: Clean tree + full sweep**

```bash
git status -sb
cmake --build out/build/msvc-win64-vs2026-community --target editor runtime game test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_navagent test_followcam test_playermove test_editorprefs test_editorcam test_metrichistory test_transientstatus test_inspector_editstate --config Debug 2>&1 | tail -15
for t in test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_navagent test_followcam test_playermove test_editorprefs test_editorcam test_metrichistory test_transientstatus test_inspector_editstate; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "All green."
```

Expected: editor + runtime + game build clean; all 14 test suites pass.

- [ ] **Step 2: Dispatch final reviewer subagent**

Provide: spec (`93863a0`), this plan, `git diff main..feat/inspector-decomposition`. Reviewer verifies:
- (a) **No widget-logic change** — each editor's `DrawEditor` body matches the corresponding deleted monolith block (spot-check Transform gizmo, StateScope bitmask, MenuButton/Collider/NavSource combos, Mesh/Material dropdowns).
- (b) **Registry order == UX order** (Transform..NavTarget) in the ctor.
- (c) **AddDefault preserves authoring defaults** (Lightning/Mesh/Material/Text non-`T{}` constructions) verbatim, not bare `T{}`.
- (d) **EditState semantics** match the old reset/refresh/command behavior.
- (e) EcsInspectorPanel.cpp ~250 lines, header = selectedEntity + m_Editors only, no leftover `editX`/`m_Gizmo`.
- (f) 15 editor files present + in CMake; `src/panels/inspector` on include path.
- (g) All commits authored `Nuno Silva <nuno.levezinho@live.com.pt>`.

- [ ] **Step 3: User GUI smoke checklist**

1. Editor launches clean (no restart gotcha — no ECS/API change).
2. Select an entity; every present component shows its editor section in the SAME order as before.
3. Edit a field in each component type → `* Modified` shows → change round-trips (1-2 frames) exactly as before.
4. Transform gizmo manipulates + decomposes back into Position/Rotation/Scale.
5. Mesh/Material dropdowns populate from loaded meshes/materials; thumbnails render.
6. StateScope checkboxes toggle bits; MenuButton/Collider/NavSource/NavObstacle combos work.
7. Right-click an entity → Add/Remove menus list all 15 components in order; Add then Remove each round-trips.
8. Day/night cycle: while NOT editing Transform/Lightning, the inspector live-refreshes the moving sun's values; while editing, your in-progress edit is preserved.
9. SunMarker shows the "Tagged as Sun" info text.

---

## Self-review notes

**Spec coverage:**
- ✅ IComponentEditor interface — Task 1.
- ✅ EditState<T> Begin/Commit — Task 1.
- ✅ test_inspector_editstate (T01-T04) — Task 1.
- ✅ 15 editors in panels/inspector/ — Tasks 2-4 (6+4+5).
- ✅ Registry vector + ctor in display order — Tasks 2-4 (append per batch).
- ✅ 3 touchpoints → registry loops — Task 2 Step 4.
- ✅ Transform owns GizmoController — Task 2.
- ✅ Mesh/Material use ctx.MeshSys/MatSys/Preview — Task 2.
- ✅ SunMarker tag editor (no EditState) — Task 2.
- ✅ AddDefault verbatim authoring defaults — Task 2 Step 2 recipe + reviewer check (c).
- ✅ Shell shrink ~1119→~250, header stripped — Task 4 Steps 4,7.
- ✅ CMake sources + include dir — Tasks 2-4.
- ✅ Green-build incremental (registry prefix + inline suffix, no double-render) — front-to-back batch migration.
- ✅ Final review + GUI smoke — Task 5.

No gaps.

**Placeholder scan:** The editor `.cpp` bodies say "move the monolith's block here verbatim" with the exact rewrite recipe + source anchor — this is a faithful instruction for a verbatim move (the 700 lines of widget code aren't reproduced; they're relocated, with the precise mechanical rewrites enumerated). All NEW code (interface, EditState, test, ctor, registry loops, worked editor examples) is shown in full. Not a placeholder — a move instruction with complete scaffolding.

**Type consistency:**
- `IComponentEditor` methods (Label/Has/AddDefault/Remove/DrawEditor) identical across interface, worked examples, and registry loops.
- `EditState<T>` `Begin(ctx,e)`/`Commit(ctx,e)`/`edit`/`modified`/`last` consistent across header, test, and every editor.
- Registry: `std::vector<std::unique_ptr<IComponentEditor>> m_Editors` consistent in header + ctor + 3 loops.
- `ctx.App->ECSCommandRing.Push(ECSCommand::{Add,Remove,Modify}Component(...))` consistent with the monolith's command usage.
- 15 editor class names match between ctor includes, push_backs, file names, and CMake source list.

**Green-build constraint:** every task's build step compiles the editor; migration is front-to-back so migrated (registry) + un-migrated (inline) partition the components with no overlap. Header triplets removed per-batch in lockstep with their inline-block deletion.
