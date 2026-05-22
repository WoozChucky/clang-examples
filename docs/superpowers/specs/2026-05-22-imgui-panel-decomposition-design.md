# ImGuiRenderer Panel Decomposition — Design

**Date:** 2026-05-22
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main`.

## Goal

`src/editor/src/rendering/imgui/ImGuiRenderer.cpp` has grown to **1913 lines**; its
`Render()` method (~1300 lines) inlines five large ImGui windows. Extract those inline panels
into focused per-panel **classes** (each owning its own UI state), following — and extending —
the existing `MemoryPanel`/`RenderStatsPanel` dedicated-file pattern, so each piece is
independently digestible. `ImGuiRenderer` becomes a thin host: device/ImGui lifecycle, input,
dock layout, the scene Viewport, and `Render()` orchestration.

This is a **pure, behavior-preserving relocation** — no new features, no renamed window titles
(so the saved dock layout keeps working) — with one cleanup: dropping dead gizmo code. Editor
only; `Engine`/`ecs`/`game`/`runtime` are untouched.

## Background (verified)

`ImGuiRenderer.cpp` structure today:
- `GlfwKeyToImGuiKey` (33), `BuildDefaultDockLayout` anon-namespace helper (144-167),
  `Init` (169), gizmo **file-statics** `m_GizmoOperation/Mode/Snap/UseSnap/UseWindow/CamDistance`
  (234-239), `TransformStart` + the "Gizmo" `ViewManipulate` window (243-331), `TransformEnd`
  (333), `EditTransform` (342-355).
- **`Render()` (356-1658)** inlines: the main menu bar (401-536), the Viewport window
  (538-563), `"Hello, world!"` stats (565-635), `"ECS Inspector & Editor"` (636-1301, ~665
  lines), `"Mesh Manager"` (1302-1525), `"Material Manager"` (1526-~1655).
- Lifecycle/host tail: ctor/dtor (1659), `GetSceneFramebuffer` (1665), `Shutdown` (1681),
  `ShutdownNvrhiOnly` (1698), `InitNvrhiForDevice` (1709), `CreateFontFromFile` (1736),
  `ProcessInputEvents` (1763), `OpenFileDialog` (1822).

State is scattered across three storage kinds: gizmo settings are **file-statics** (234);
`selectedEntity`/`lastEditedEntity` are **function-local statics** in the inspector (676, 861);
mesh-preview camera (`m_MeshPreviewState`) and settings-menu state (`m_PendingBackend`,
`m_PendingBackendInitialized`, `m_SettingsSaveError`) are real **members** (ImGuiRenderer.h).
The decomposition moves each panel's state into that panel's class.

Already-extracted free-function panels (`DrawMemoryPanel`, `DrawRenderStatsPanel`) stay as they
are — called from the host.

The unused gizmo path: `m_GizmoUseWindow` is `false` and `TransformStart`/`TransformEnd` (the
`ViewManipulate` "Gizmo" window) are **never called** (only `EditTransform` is, from the
inspector at 939). That dead code is removed.

## Scope

**In scope:** new `EditorContext.h`; new panel classes `MainMenuBar`, `StatsPanel`,
`MeshManagerPanel`, `MaterialManagerPanel`, `EcsInspectorPanel`, `GizmoController`; extract
`OpenFileDialog` to `EditorFileDialog.{h,cpp}`; slim `ImGuiRenderer.{h,cpp}` to a host;
`src/editor/CMakeLists.txt` additions.

**Out of scope / unchanged:** behavior (pixel-identical UI), window titles, the dock layout,
`MemoryPanel`/`RenderStatsPanel`, the Viewport/`SceneViewport`/`GetSceneFramebuffer` seam,
input handling, `Engine`/`ecs`/`game`/`runtime`, `GAME_API_VERSION`. No new panels or features.
The only intentional removal is the dead `TransformStart`/`TransformEnd`/"Gizmo" window.

## Design

### `EditorContext.h` (new) — shared per-frame dependencies

Built once per `Render()` and passed by `const&` to each panel's `Draw`. Forward-declares the
heavy types; includes only what the struct needs (`<memory>`, `nvrhi` fwd, `ImDrawList` fwd).

```cpp
struct EditorContext {
    ApplicationContext*  App            = nullptr; // ECS command ring, settings, GameThreadConfig
    MeshSystem*          MeshSys        = nullptr;
    MaterialSystem*      MatSys         = nullptr;
    MeshPreviewRenderer* Preview        = nullptr;
    const ECS*           World          = nullptr; // live snapshot (camera singleton)
    std::shared_ptr<ECS> WorldSnapshot;            // editable snapshot (entity/component access, save)
    SimulationSnapshot*  Snapshot       = nullptr;
    float                GpuFrameTimeMs = 0.0f;
    // Gizmo target rect/drawlist — set by the host AFTER the Viewport window draws this frame,
    // BEFORE the inspector's Draw (same ordering the code relies on today).
    ImDrawList*          ViewportDrawList = nullptr;
    float                ViewportMinX = 0.0f, ViewportMinY = 0.0f;
    uint32_t             ViewportW = 0, ViewportH = 0;
};
```
(The exact `WorldSnapshot` type matches whatever `Render()` currently holds — a
`std::shared_ptr<ECS>` from the snapshot; the implementer mirrors the existing local.)

### Panel classes (each `src/editor/src/rendering/imgui/<Name>.{h,cpp}`)

Each is a small class owning its persistent UI state as members, with a single entry point
`void Draw(const EditorContext& ctx);` (signature noted per panel where it differs). `Render()`
holds one instance of each and calls them in the **current order**.

- **`MainMenuBar`** — File/Edit/About/Settings/View. Owns `m_PendingBackend`,
  `m_PendingBackendInitialized`, `m_SettingsSaveError`. `bool Draw(const EditorContext& ctx);`
  returns `true` when **View → Reset Layout** was clicked, so the host rebuilds the dock layout.
  Save uses `ctx.WorldSnapshot`. Backend-swap pushes to `ctx.App->PRCommandRing` (unchanged).
- **`StatsPanel`** — `ImGui::Begin("Hello, world!")`: renderer FPS / GPU ms / game TPS, the
  GameThreadSettings editor (target-TPS radios, spin threshold, frame-time-tracking toggle via
  `ctx.App->GameThreadConfig`), frame-time stats + reset. Title kept verbatim.
- **`MeshManagerPanel`** — `ImGui::Begin("Mesh Manager")`. Owns selected-mesh id + the mesh
  preview camera state (the current `m_MeshPreviewState` struct moves here). Uses `ctx.MeshSys`,
  `ctx.Preview`, `ctx.App`, and `EditorFileDialog::Open` + `MeshLoader` for load-from-file.
- **`MaterialManagerPanel`** — `ImGui::Begin("Material Manager")`. Owns selected-material state.
  Uses `ctx.MatSys`, `ctx.App`, `EditorFileDialog::Open` + `MaterialLoader`.
- **`EcsInspectorPanel`** — `ImGui::Begin("ECS Inspector & Editor")`. Owns `selectedEntity`,
  `lastEditedEntity`, and the per-frame edit working copy (today's function-local statics become
  members). Entity list / create / select / delete, component add/remove menus, and per-component
  editors (Transform, Lightning, Mesh, Material, Text, Sun marker). Pushes `ECSCommand`s to
  `ctx.App->ECSCommandRing` exactly as today. Holds a reference/pointer to a `GizmoController`
  (constructed by the host and shared, or owned by the inspector — implementer's call; owned by
  the inspector is simplest since only it uses the gizmo). Reads the camera from `ctx.World`'s
  `WorldCameraComponent`.
- **`GizmoController`** — owns `m_GizmoOperation`, `m_GizmoMode`, `m_GizmoUseSnap`, `m_GizmoSnap`.
  Exposes the operation/mode radio controls and
  `void EditTransform(float* view, float* proj, float* matrix, const EditorContext& ctx);`
  which sets `ImGuizmo::SetDrawlist(ctx.ViewportDrawList)` + `SetRect(ctx.ViewportMinX/Y,
  ctx.ViewportW/H)` and calls `ImGuizmo::Manipulate` (the logic moved verbatim from the current
  `EditTransform`, lines 342-355, plus the inline gizmo radio buttons at 896-928). The dead
  `TransformStart`/`TransformEnd`/"Gizmo" `ViewManipulate` window is **not** carried over.

### `EditorFileDialog.{h,cpp}` (new)

Move `OpenFileDialog` (the Windows `IFileOpenDialog`/`commdlg` helper, 1822-end) to a free
function `bool EditorFileDialog::Open(char* outPath, size_t outPathSize, const char* filter);`
(namespace or static-class form). Mesh/Material panels call it. Removed from `ImGuiRenderer`.

### `ImGuiRenderer` (host) after refactor

Keeps and owns: `Init`/`InitNvrhiForDevice`/`ShutdownNvrhiOnly`/`Shutdown`/ctor/dtor;
`GetSceneFramebuffer` + `m_SceneViewport`; the Viewport window draw (still captures the image
rect + window draw list — now written into the per-frame `EditorContext`); `ProcessInputEvents`
+ `GlfwKeyToImGuiKey`; `CreateFontFromFile` + `m_fonts`; `BuildDefaultDockLayout` + the
dockspace/reset block; `m_ImGuiNvrhi`, `m_MeshPreviewRenderer`, device/system pointers. Owns one
instance of each panel class + the `GizmoController`.

`Render()` becomes orchestration: ImGui new-frame + font + `ImGuizmo::BeginFrame`; build
`EditorContext ctx` from members + params; `if (m_MenuBar.Draw(ctx)) requestReset = true;`;
dockspace + (reset?) layout build; draw the Viewport window and write its rect/drawlist into
`ctx`; then `m_StatsPanel.Draw(ctx)`, `DrawMemoryPanel(...)`, `DrawRenderStatsPanel(...)`,
`m_EcsInspector.Draw(ctx)`, `m_MeshManager.Draw(ctx)`, `m_MaterialManager.Draw(ctx)` — in the
**same window order as today**; `ImGui::Render()` + `m_ImGuiNvrhi->render(framebuffer)`. Target
size ~600 lines.

## Data flow / ordering invariant

The gizmo needs the Viewport panel rect, which is known only when the Viewport window draws.
Today the Viewport (538) draws before the inspector (636). Preserve this: the host draws the
Viewport window and writes `ViewportDrawList`/`ViewportMin*`/`ViewportW/H` into `ctx` **before**
calling `m_EcsInspector.Draw(ctx)`. The `GizmoController::EditTransform` reads them from `ctx`.

## Phasing (editor builds green at each step)

Incremental extraction, one piece per step, each leaving the editor compiling and behaving
identically:
1. `EditorContext.h` + `EditorFileDialog.{h,cpp}` (extract `OpenFileDialog`); host builds `ctx`,
   Mesh/Material call the new dialog.
2. `StatsPanel` (simplest, mostly read-only).
3. `MaterialManagerPanel`.
4. `MeshManagerPanel` (moves `m_MeshPreviewState`).
5. `GizmoController` (move `EditTransform` + gizmo statics + the inline gizmo radios; **drop**
   `TransformStart`/`TransformEnd`/"Gizmo" window).
6. `EcsInspectorPanel` (uses `GizmoController`; moves selection statics to members).
7. `MainMenuBar` (moves settings-menu members; returns reset-layout request).
8. Final host slim: remove now-dead members/includes from `ImGuiRenderer.{h,cpp}`; confirm
   `Render()` is pure orchestration.

## Build / verification

Build preset `msvc-win64-vs2026-community`. No `GAME_API_VERSION` bump; engine/runtime
untouched. Each new `.cpp` added to `src/editor/CMakeLists.txt` (reconfigure when adding files).
No unit tests (ImGui UI). Verification:
- `editor` builds clean at every step; `runtime` + `test_ecs`/`test_alloc`/`test_frustum` stay
  green (none are touched, but confirm).
- **GUI smoke (user, after all steps):** every panel behaves identically — entity
  select/create/delete, component add/remove, Transform editing **with the gizmo working in the
  Viewport**, Mesh Manager list+preview+load, Material Manager list+preview+load, the stats panel
  + GameThreadSettings controls, the menu bar incl. **View → Reset Layout**, and the Viewport
  itself. `runtime.exe` renders unchanged.

## Risks

- **Missing a dependency in `EditorContext`** → compile error in a panel, caught immediately at
  that step's build; add the field.
- **Gizmo rect/drawlist set after the inspector draws** → gizmo breaks again; mitigated by the
  explicit ordering invariant (Viewport writes `ctx` before the inspector reads it) and the
  post-refactor GUI smoke.
- **Behavior drift during a large move** (e.g. the 665-line inspector) → mitigated by extracting
  verbatim (no logic edits) one panel per step, building + spot-checking each, and keeping window
  titles/IDs identical so layout + `imgui.ini` are unaffected.
- **Dead-code removal assumption wrong** (something does call `TransformStart`) → grep confirms
  only the definition exists; the build (unresolved/unused) and smoke catch any surprise.
