# Selection Outline (mesh silhouette, inverted hull) — Design

**Date:** 2026-05-22
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main`.

## Goal

Draw a colored silhouette around the **selected** entity's mesh in the editor Viewport, so the
user can (a) confirm which mesh a click actually picked (picking is ray-vs-AABB, so the box you
click can differ from the mesh selected) and (b) see the selection at a glance. Editor-only — the
runtime draws no outline.

Technique: **inverted hull** (no stencil). A new render pass draws the selected mesh enlarged
along its vertex normals in a flat outline color **before** the normal mesh pass; the normal mesh
then draws on top, covering the hull's center and leaving a colored rim.

## Background (verified)

- **No stencil buffer.** Both the swapchain depth (`RendererBackend.cpp:27-38`) and the editor
  offscreen scene RT depth (`SceneViewport.cpp:36-46`) are `nvrhi::Format::D32` (depth-only). A
  stencil outline would require a stencil depth format on both, which would break the gameplay
  passes' pipelines (they're built against the framebuffer's format and shared between the editor
  offscreen RT and the runtime swapchain). → inverted hull instead.
- **Pass registration + order:** `Renderer::Init()` (`Renderer.cpp:85-104`) adds passes via
  `AddRenderPass` (push_back) in source order: Primitive → Mesh → Ui. Passes run in that order
  (`Renderer.cpp:195-200`). Registering the outline pass **between Primitive and Mesh** makes it
  draw before the mesh. (Same in the `InitForSwap` path if present — register in both.)
- **MeshRenderPass to mirror** (`MeshRenderPass.{h,cpp}`): `Initialize(device, renderer)`; shaders
  created via `m_Renderer->CreateShader(type, <inline HLSL>, 0, "main_vs"/"main_ps", "vs_6_1"/"ps_6_1")`;
  input layout POSITION(RGB32F)+NORMAL(RGB32F)+TEXCOORD(RG32F) over `MeshVertex` (`px,py,pz,nx,ny,nz,u,v`,
  `ApplicationContext.h:55-60`); pipeline built lazily on first `Render` from
  `frameBuffer->getFramebufferInfo()` via `createGraphicsPipeline(pso, fbi)`; per-frame CB written
  with `commandList->writeBuffer`. `IRenderPass` interface: `Initialize`, `Render(cmd, fb, snapshot,
  world, dt, frameAllocator)`, `OnResize`, `Shutdown` (`IRenderPass.h`).
- **Mesh resources:** `m_Renderer->GetMeshSystem()->GetMeshResources(meshId)` →
  `{ vertexBuffer, indexBuffer, subMeshes (each IndexStart/IndexCount), valid }`
  (`MeshSystem.h:31-38`, used at `MeshRenderPass.cpp:443`).
- **Camera:** `world->GetSingleton<WorldCameraComponent>()` → `View`/`Projection`
  (`MeshRenderPass.cpp:360`).
- **Components:** `world->GetComponent<MeshComponent>(id)` / `<TransformComponent>(id)`;
  `MeshComponent{ uint32_t MeshId; bool Visible; }`. `ModelMatrix(const TransformComponent&)` from
  `TransformMath.h` (built in the picking feature).
- **Selection lives in the editor:** `EcsInspectorPanel::selectedEntity` (`EcsInspectorPanel.h:14`),
  set by the inspector list + the Viewport pick. `EntityId = uint64_t`, `INVALID_ENTITY = 0`
  (`ECS.h:44-45`).
- **Threads:** the editor overlay AND the render passes both run on the **RenderThread** (passes
  first each frame, then the overlay). Cross-thread publish pattern: `ApplicationContext` atomics
  (`SceneViewportSize`, `GameAcceptsMouse`, …) written by the overlay, read elsewhere.

## Scope

**In scope:** `ApplicationContext::SelectedEntity` atomic; `EcsInspectorPanel::GetSelectedEntity`;
the overlay publishing it; a new `OutlineRenderPass` (+ inline outline shaders); its registration
in `Renderer` (before MeshRenderPass) + engine CMake.

**Out of scope / non-goals:** constant screen-space outline width (this uses world/local
normal-extrusion → width grows when closer; a post-process edge-detect is the future upgrade);
hard-edge gap-free silhouettes (inverted hull splits at sharp normals); perfect occlusion (drawn
before other meshes — outline may show through/under later geometry, usually fine for editor
selection); multi-select; outline for non-mesh entities (lights/text have no mesh → nothing to
hull); editor-configurable color/width (hardcode sensible constants now). No `GAME_API_VERSION`
bump (no GameState/ECS-layout change).

## Design

### 1. Publish the selected entity to the engine
- `src/common/include/ApplicationContext.h`: add (near the other editor atomics)
  ```cpp
  // Selected entity for the editor outline pass. RenderThread (overlay) writes, the OutlineRenderPass
  // (also RenderThread, earlier in the frame) reads -> 1-frame lag. INVALID_ENTITY (0) = no outline.
  // The runtime never writes it, so no outline is drawn there.
  std::atomic<uint64_t> SelectedEntity{INVALID_ENTITY};
  ```
- `EcsInspectorPanel.h`: add `EntityId GetSelectedEntity() const { return selectedEntity; }`.
- `ImGuiRenderer::Render`: after `m_EcsInspector.Draw(ctx)`, publish
  `if (m_AppContext) m_AppContext->SelectedEntity.store(m_EcsInspector.GetSelectedEntity(), std::memory_order_relaxed);`.

### 2. `OutlineRenderPass` (`src/engine/src/rendering/passes/OutlineRenderPass.{h,cpp}`)
An `IRenderPass` mirroring MeshRenderPass's setup but simpler (draws one entity, no instancing):
- **Shaders (inline HLSL):** VS reads a CB `{ float4x4 VP; float4x4 Model; float OutlineWidth; float3 _pad; float4 OutlineColor; }`, computes
  `float3 lp = vin.Position + normalize(vin.Normal) * OutlineWidth; PosH = mul(VP, mul(Model, float4(lp,1)));`.
  PS returns `OutlineColor`. Input layout: reuse POSITION+NORMAL+TEXCOORD (TEXCOORD unused but keeps
  one layout). Created via `m_Renderer->CreateShader(...)`.
- **Pipeline (lazy, like MeshRenderPass):** `primType = TriangleList`; depthTestEnable=true,
  depthWriteEnable=true; **`rasterState.cullMode = Front`** (renders the back of the enlarged hull
  → the silhouette rim), `setFrontCounterClockwise(true)` (match the mesh winding); blend disabled
  (solid color); single binding layout with one constant buffer (visibility All).
- **Constant buffer + binding set:** a CB matching the struct above; bound to the pipeline. Updated
  per-draw with `commandList->writeBuffer`.
- **`Render`:** read `EntityId sel = m_Renderer->GetAppContext()->SelectedEntity.load(std::memory_order_relaxed);`.
  `Renderer` already holds the `ApplicationContext*` (`Renderer.h:134`, set in its constructor) but
  exposes no getter — add `ApplicationContext* GetAppContext() const { return m_AppContext; }` to
  `Renderer` (mirrors the existing `GetMeshSystem()`). If `sel == INVALID_ENTITY` → return. Else: `const auto* mc = world->GetComponent<MeshComponent>(sel);`
  `const auto* tc = world->GetComponent<TransformComponent>(sel);` — if either missing or
  `!mc->Visible` → return. `auto res = GetMeshResources(mc->MeshId);` if `!res.valid` → return.
  Build CB: `VP = P*V` (from `WorldCameraComponent`), `Model = ModelMatrix(*tc)`, `OutlineWidth`
  (constant, e.g. `0.03f`), `OutlineColor` (constant, e.g. `(1.0, 0.6, 0.1, 1.0)` orange). Set
  graphics state (pipeline, framebuffer, viewport from `frameBuffer->getFramebufferInfo().getViewport()`,
  vertex/index buffers, binding set) and `drawIndexed` over each `subMesh` (IndexStart/IndexCount),
  exactly as MeshRenderPass does per submesh.
- `OnResize` nulls the pipeline (rebuilt next frame); `Shutdown` releases resources — mirror
  MeshRenderPass.

### 3. Register the pass
`Renderer::Init` (and `InitForSwap` if it also builds the pass list): construct + `Initialize` an
`OutlineRenderPass` and `AddRenderPass` it **between** the Primitive and Mesh registrations, so the
order is Primitive → **Outline** → Mesh → Ui. Add `src/rendering/passes/OutlineRenderPass.cpp` to
`src/engine/CMakeLists.txt`.

## Data flow (per frame, editor, RenderThread)

1. `OutlineRenderPass::Render` (before MeshRenderPass) reads `ApplicationContext::SelectedEntity`
   (set last frame by the overlay). If valid + mesh + transform, draws the normal-extruded hull in
   the outline color (cull FRONT, depth test+write).
2. `MeshRenderPass::Render` draws all meshes normally on top → covers the hull's center, leaving
   the colored rim around the selected mesh.
3. The overlay (`ImGuiRenderer`) draws the panels and republishes `SelectedEntity` from the
   inspector for next frame.

Runtime: no overlay → `SelectedEntity` stays `INVALID_ENTITY` → the outline pass returns
immediately → identical rendering to today.

## Build / verification

Build preset `msvc-win64-vs2026-community`. No `GAME_API_VERSION` bump.
- `editor` + `runtime` build clean; `test_ecs`/`test_alloc`/`test_frustum`/`test_input`/`test_picking`
  stay green (none touched).
- **GUI smoke (user):** select a mesh (click or Inspector list) → a colored silhouette appears
  hugging it; deselect (empty click) → outline gone; pick a different mesh → outline moves; the
  outline confirms the picked mesh (matches what got selected); `runtime.exe` renders identically
  (no outline). Move the gizmo → the outline follows the mesh (it reads the live transform).

## Risks

- **No stencil** → inverted hull chosen (verified D32-only). If a future feature needs stencil,
  that's a separate depth-format change.
- **Width varies with camera distance** (world/local extrusion) → accepted non-goal; constant
  screen-space width is the post-process upgrade.
- **Hard-edge gaps** on meshes with split normals (cubes) → inherent to inverted hull; accepted.
- **`ApplicationContext` access from the pass** — resolved: the pass reads
  `m_Renderer->GetAppContext()->SelectedEntity` via a new `Renderer::GetAppContext()` getter
  (Renderer already stores the `ApplicationContext*`). The selected id is the only state the pass
  needs from outside the ECS snapshot.
- **1-frame selection→outline lag** (overlay writes after passes run) → imperceptible; documented.
