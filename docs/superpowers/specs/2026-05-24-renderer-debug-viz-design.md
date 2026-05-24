# Renderer Debug Visualization (debug-draw + gizmos + wireframe) — Design

**Date:** 2026-05-24
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main` (implementation on `renderer-debug-viz`).

## Goal

Add render-thread debug visualization to the editor:
- a **debug-draw** line API (lines/boxes/spheres/arrows/frustum) drawn by a new `DebugRenderPass`;
- **gizmos** generated from the ECS snapshot: light gizmos, the game-camera frustum, the selected
  entity's AABB;
- a **wireframe** toggle for mesh rendering.

All toggled from the Render Stats panel. Render-thread/editor-facing; no game-thread debug-draw and no
`ApplicationContext`/cross-thread change. (Shader hot-reload + shadows are separate, later cycles.)

## Background (verified)

- **Pass model** (`Renderer.cpp`): `IRenderPass` list run in order (`PrimitiveRenderPass` grid →
  `MeshRenderPass` → `OutlineRenderPass` → `UiRenderPass`); registered in `Renderer::Init` +
  `InitForSwap`. Each `Render(commandList, framebuffer, snapshot, world, deltaTime, frameAllocator)`.
- **`OutlineRenderPass`** is the template for a depth-tested overlay pass: holds `Renderer* m_Renderer`,
  a CB with `VP`, a lazily-created pipeline keyed on the framebuffer info, `depthTestEnable=true` /
  `depthWriteEnable=false`; reads `m_Renderer->GetActiveCamera()` (the editor-or-game camera resolved
  per frame) and `m_Renderer->GetAppContext()->SelectedEntity`.
- **Active camera**: `Renderer::GetActiveCamera()` returns the `CameraView` the world passes use this
  frame — so debug geometry renders through the editor camera in Edit mode, matching the scene.
- **Render settings pattern** (`src/engine/src/rendering/RenderStats.{h,cpp}`): `ENGINE_API`
  free functions return function-local-static structs — `GetRenderStats()` + `GetCullingSettings()`
  (`struct CullingSettings { bool Enabled; }`). The editor `RenderStatsPanel` flips
  `GetCullingSettings().Enabled` via a checkbox and reads `GetRenderStats()` counters. This is the
  established render-thread settings channel (no `ApplicationContext` needed).
- **Lights/camera in the ECS**: `LightningComponent` (Type Directional/Point/Spot, Direction `vec4`,
  Color `vec4`, Intensity, Range) + the entity `TransformComponent` for position; `WorldCameraComponent`
  (View, Projection, Position) is the game camera. `MeshSystem::GetMeshBounds(meshId)` → `{valid,min,max}`.
- **Mesh pipeline** (`MeshRenderPass`): a lazily-created `nvrhi::GraphicsPipeline`; raster state
  currently solid fill. NVRHI supports `nvrhi::RasterFillMode::Wireframe`. Line topology via
  `nvrhi::PrimitiveType::LineList`.
- **`FrameAllocator`**: a per-frame bump arena reset each frame; used by `MeshRenderPass` for many small
  per-frame instance arrays. (Decision below: debug-draw uses a reused `std::vector`, not the arena.)
- **`RenderStatsPanel`** (`src/editor/src/rendering/imgui/RenderStatsPanel.cpp`): free function
  `DrawRenderStatsPanel(bool* open)` — the cull checkbox + counters live here.

## Scope

**In scope:**
1. `DebugDraw.h` — `DebugVertex` + pure geometry builders that append line-list vertices to a
   `std::vector<DebugVertex>&` (unit-tested).
2. `DebugRenderPass` — line-topology, depth-test on / write off; builds gizmos from the snapshot +
   settings, uploads via a reused CPU vector, draws.
3. Light gizmos, game-camera frustum, selected-entity AABB.
4. Wireframe pipeline variant in `MeshRenderPass`.
5. `DebugDrawSettings` + `GetDebugDrawSettings()` (engine global) + Render Stats panel checkboxes.
6. `test_debugdraw` for the pure builders.

**Out of scope / non-goals:** game-thread/cross-thread debug-draw API (render-thread only here);
filled/triangle debug shapes (lines only); always-on-top/x-ray mode (gizmos are depth-tested);
consolidating the existing grid into debug-draw (the grid stays its own pass); text debug labels;
shader hot-reload; shadows. No `GAME_API_VERSION` bump.

## Design

### 1. `DebugDraw.h` (pure, unit-tested) — `src/engine/src/rendering/DebugDraw.h`

```cpp
struct DebugVertex { glm::vec3 Position; glm::vec4 Color; };
```
Free builders (each appends to `std::vector<DebugVertex>& out`; pure, no GPU/global state):
- `DebugAppendLine(out, a, b, color)` — 2 verts.
- `DebugAppendBox(out, min, max, color)` — the 12 edges of the AABB (24 verts).
- `DebugAppendSphere(out, center, radius, color, segments=24)` — 3 axis-aligned circles
  (XY/XZ/YZ), `segments` line-segments each.
- `DebugAppendArrow(out, from, to, color)` — shaft + a small head (a few lines).
- `DebugAppendFrustum(out, invViewProj, color)` — unproject the 8 NDC-cube corners
  (`x,y ∈ {-1,1}`, `z ∈ {0,1}` for ZO depth) by `invViewProj`, perspective-divide, connect the 12
  edges.
Pure GLM math → directly unit-testable (vertex counts + known corner positions).

### 2. `DebugRenderPass` — `src/engine/src/rendering/passes/DebugRenderPass.{h,cpp}`

Mirrors `OutlineRenderPass` structure:
- Members: `Renderer* m_Renderer`, a CB holding `glm::mat4 VP`, a lazily-created line pipeline
  (`primType = LineList`, `depthTestEnable=true`, `depthWriteEnable=false`, `cullMode=None`), an
  input layout (POSITION RGB32_FLOAT + COLOR RGBA32_FLOAT), a GPU vertex buffer (grown when needed),
  and a **reused** `std::vector<DebugVertex> m_Verts`.
- VS multiplies position by `VP`, passes color through; PS outputs the color. Inline HLSL via
  `Renderer::CreateShader` (matching the other passes).
- `Render` each frame:
  1. `const DebugDrawSettings& s = GetDebugDrawSettings();` and `m_Verts.clear();` (retains capacity).
  2. **Light gizmos** (`s.ShowLightGizmos`): for each entity with `LightningComponent` (+
     `TransformComponent`): Point → `DebugAppendSphere(center=transform.Position, radius=range)`;
     Directional → `DebugAppendArrow` along `Direction`; color = the light color.
  3. **Camera frustum** (`s.ShowCameraFrustum`): from `WorldCameraComponent` →
     `DebugAppendFrustum(inverse(Projection * View))`.
  4. **Selected AABB** (`s.ShowSelectedAABB`): `SelectedEntity` atomic → `TransformComponent` +
     `MeshSystem::GetMeshBounds` → world AABB (`TransformAABB`) → `DebugAppendBox`; no mesh bounds →
     a small `DebugAppendBox`/cross at `transform.Position`.
  5. If `m_Verts` non-empty: grow the GPU buffer if `m_Verts.size()` exceeds capacity,
     `writeBuffer(m_Verts.data(), ...)`, set `VP = GetActiveCamera().Projection * .View`, draw a
     single `LineList` of `m_Verts.size()` vertices.
- **Allocation:** the reused `m_Verts` member (cleared per frame, capacity retained) is the CPU
  scratch — contiguous, so `writeBuffer` reads `.data()` directly. (Chosen over the `FrameAllocator`:
  debug-draw is one contiguous list, so a reused vector has zero steady-state heap churn and no
  count-pass; the arena's value is many small per-frame sub-allocations, which this isn't.)

### 3. Wireframe (`MeshRenderPass`)

Add a second lazily-created pipeline identical to the solid one but
`rasterState.fillMode = nvrhi::RasterFillMode::Wireframe`. Select it when
`GetDebugDrawSettings().Wireframe` is set; otherwise the solid pipeline. (Both cached; no per-frame
recreation.)

### 4. Settings + UI

`src/engine/src/rendering/RenderStats.h` / `.cpp` — add alongside `CullingSettings`:
```cpp
struct DebugDrawSettings {
    bool ShowLightGizmos   = false;
    bool ShowCameraFrustum = false;
    bool ShowSelectedAABB  = false;
    bool Wireframe         = false;
};
ENGINE_API DebugDrawSettings& GetDebugDrawSettings(); // function-local static, like GetCullingSettings
```
`RenderStatsPanel.cpp` — under a `"Debug Draw"` separator, four `ImGui::Checkbox`es bound to those
fields (same pattern as the existing cull checkbox).

### 5. Pass registration / order

Register `DebugRenderPass` in `Renderer::Init` + `InitForSwap` **after** `OutlineRenderPass` and
**before** `UiRenderPass` (so gizmos depth-test against the scene + outline but stay under the UI
text). Order: Primitive → Mesh → Outline → **Debug** → Ui.

## Build / verification

Build preset `msvc-win64-vs2026-community`. New engine sources → reconfigure. No `GAME_API_VERSION`
bump.

- **Unit test (`test_debugdraw`)** on the pure builders:
  - `DebugAppendLine` → 2 verts; `DebugAppendBox(min,max)` → 24 verts, all 8 corners present.
  - `DebugAppendFrustum(identity)` → 24 verts; the 8 corners equal the NDC cube corners
    (`x,y ∈ {-1,1}`, `z ∈ {0,1}`) since identity `invViewProj` maps clip=NDC.
  - `DebugAppendSphere(c, r, color, segments=N)` → `3 * N * 2` verts.
  - Color is copied onto every emitted vertex.
  Prints `All debug draw tests passed.`
- `editor` + `runtime` build clean; the other 9 test suites stay green.
- **GUI smoke (user-run):** Render Stats panel shows four new checkboxes. Light gizmos: point lights
  show wireframe spheres sized to range, the sun shows a direction arrow, colored by light color.
  Camera frustum: the game camera's frustum is visible while flying the editor camera. Selected AABB:
  selecting an entity draws its bounding box. Wireframe: meshes render as wireframe; toggling off
  restores solid. Gizmos are hidden behind opaque geometry (depth-tested). `runtime.exe` unaffected
  (defaults all-off; no editor panel).

## Risks

- **Wireframe pipeline state** — needs a separate cached pipeline (NVRHI pipelines are immutable);
  mitigated by lazily creating both variants once and selecting per frame.
- **Vertex-buffer growth** — recreate the GPU buffer when `m_Verts.size()` exceeds its capacity
  (with `keepInitialState`), like the other dynamic buffers; start with a sane reserve.
- **Frustum math (ZO depth)** — corners use `z ∈ {0,1}` (GLM `_ZO`), not `{-1,1}`; pinned by the
  test against an identity `invViewProj`.
- **Empty frame** — when all toggles are off (the default, and always in `runtime`), the pass builds
  no vertices and early-outs before any draw — zero cost.
- **Light position source** — point-light position comes from the entity `TransformComponent`
  (consistent with how meshes/lights are placed); a light with no transform is skipped.
