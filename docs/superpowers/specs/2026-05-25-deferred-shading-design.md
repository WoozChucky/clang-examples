# Deferred Shading Foundation — Design

**Date:** 2026-05-25
**Status:** Approved (pending implementation plan)

## Goal

Convert the renderer from **forward** shading (geometry + lighting in one mesh
pass) to **deferred** shading (a G-buffer geometry pass + a full-screen lighting
pass), **reproducing the current visuals 1:1** — no new look. This is the
foundation that lets lighting (sun, point lights, shadows, fog, and later the
day/night ambient moon) live in one dedicated lighting pass instead of bloating
`MeshRenderPass`.

This is **sub-project 1**. The day/night cycle overhaul (capped day, longer/
smoother cycle, ambient moon) is **sub-project 2** and layers onto the lighting
pass afterward, in its own spec/plan.

## Scope

In scope: G-buffer infrastructure, a `GBufferFillPass`, a `LightingRenderPass`
that ports the existing lighting + fog math verbatim, Renderer ownership of the
G-buffer, editor-viewport and runtime-swapchain integration, two-backend (DX12 +
Vulkan) parity.

Out of scope (YAGNI / later): the day/night look changes; a forward path for
transparent/unlit meshes (none exist today — see below); G-buffer optimization
(depth-reconstruction of world pos, normal packing); MSAA; a general
post-process chain.

## Background: current pipeline (forward)

- Pass order each frame: `Primitive(grid) → Shadow → Mesh → Outline → Debug →
  UI` (registered in `Renderer`, executed in `Renderer::Render`).
- `MeshRenderPass` shades geometry directly: gathers the directional sun +
  point lights, samples the shadow map (3×3 PCF), and blends fog — all in its
  pixel shader.
- Scene target: in the **editor**, the `ImGuiOverlay`/`SceneViewport` owns an
  offscreen color (SRGBA8_UNORM) + depth (D32); ImGui samples the color. In the
  **runtime** (no overlay), passes render straight to the swapchain backbuffer +
  its depth.
- Fog (already shipped on this branch) is resolved once per frame in
  `Renderer::Render` from the SunMarker light (`m_FrameFog`) and consumed by the
  mesh pass; it also drives the scene clear color.

## Why no forward "remainder" is needed

Deferred cannot shade **transparent** (alpha-blended, needs multiple overlapping
surfaces per pixel) or **unlit** (bypasses lighting) geometry through a G-buffer.
A forward path would normally be kept for those. **Neither exists today:**

- `MeshRenderPass.cpp` sets instance flags as `flags = (material->Flags & 1u)`
  — only bit 0 (UseTexture). `OPT_UNLIT` (bit 1) is never set by any code.
- `MaterialComponent.BaseColor` defaults to `vec4(1.0)` (alpha 1) and `game.cpp`
  sets no alpha < 1. Pipeline blending is enabled but is a no-op at alpha 1.

So every current mesh is **opaque-lit** and goes through the G-buffer. We skip
the forward remainder. **Guard:** if a mesh ever sets `OPT_UNLIT` or alpha < 1,
that is the trigger to add a forward path (note it in code so it isn't silent).

## Architecture

### New pass order
```
Shadow → GBufferFill → Lighting → Primitive(grid) → Outline → Debug → UI
```
`Shadow` stays first (depth-only, its own framebuffer; lighting needs the shadow
map ready). `Primitive` moves to **after** lighting: it is forward geometry that
must depth-test against the scene and composite over the lit image (if it ran
before `GBufferFill`, its depth would be cleared and its color overwritten by the
lighting pass). `Outline/Debug/UI` stay forward, after lighting, unchanged.

### G-buffer (Renderer-owned, used in both editor and runtime)
| Target | Contents | Format | Rationale |
|--------|----------|--------|-----------|
| RT0 | Albedo (linear) | `RGBA8_UNORM` (linear, **not** sRGB) | resolved texture sample or base color |
| RT1 | World-space normal | `RGBA16_FLOAT` | unit normal, unencoded — 1:1-safe |
| RT2 | World position | `RGBA16_FLOAT` | needed for point-light distance, shadow `LightVP`, fog distance |
| Depth | scene depth | `D32` (the sceneBuffer's depth) | shared; reused by the forward passes after lighting |

### Key design calls (and why)
1. **Store world position explicitly (RT2)** rather than reconstructing it from
   depth + inverse-VP. Heavier bandwidth, but avoids the classic 1:1 footgun of
   NDC/[0,1]-depth/RH/D3D-vs-Vulkan reconstruction differences across the two
   backends. The lighting pass then needs **no depth bound** at all, which also
   sidesteps the depth-as-SRV-then-depth-attachment state-transition mess.
   Optimization (reconstruct from depth) is deliberately deferred.
2. **sRGB handled once, at the end.** Albedo is stored **linear** in RT0; the
   lighting pass computes `albedo * lighting` and writes the **SRGBA8** scene
   color (single sRGB encode on output) — matching the forward path exactly.
   Storing albedo in an sRGB RT would double-encode.
3. **Geometry mask via the normal RT.** `GBufferFill` clears RT1 (normal) to
   `(0,0,0)`. The lighting pass treats `dot(N,N) < 0.5` as "no geometry" (sky)
   and outputs the fog/sky color there; otherwise it lights the surface. This
   gives a sky test without binding depth.

### Components / responsibilities
- **`Renderer`** — owns the three G-buffer color textures + the `GBufferFill`
  framebuffer (G-buffer colors + the sceneBuffer's depth). Creates them lazily,
  **recreates on size change** (covers both editor-viewport resize and runtime
  window resize — checked against the sceneBuffer's framebuffer dimensions each
  frame), and tears them down with the backend (mirror the existing shadow-
  resource lifecycle: `CreateShadowResources`/teardown). Exposes the G-buffer
  textures to the lighting pass (accessors, like `GetShadowDepthTexture`).
- **`GBufferFillPass`** (new) — renders all meshes into the 3 MRTs + shared
  depth. Reuses the current mesh vertex shader and the instance/batch/cull logic
  from `MeshRenderPass`. Its pixel shader writes albedo (resolved texture or base
  color, linear), world normal, and world position — **no lighting**.
- **`LightingRenderPass`** (new) — a full-screen triangle (expanded from
  `SV_VertexID`, the first such pass in the codebase). Binds the G-buffer SRVs +
  the point-light structured buffer + the shadow map/sampler + a per-frame CB
  (camera, directional light, fog, ambient, shadow `LightVP`/bias). Computes the
  **exact current lighting + fog math** and writes the scene color. Fog moves
  here from the mesh PS.
- **`MeshRenderPass`** — removed/retired: its responsibilities split into
  `GBufferFillPass` (geometry) + `LightingRenderPass` (lighting). (Its inline
  HLSL and CB layout knowledge migrate into the two new passes.)
- **`SceneViewport`** (editor) — unchanged ownership of the final color + shared
  depth; on resize it already recreates those, which drives the Renderer's
  per-frame G-buffer size check.

### Data flow
```
Shadow map (ShadowDepthPass) ─────────────┐
meshes ─> GBufferFill ─> [RT0 albedo, RT1 normal, RT2 worldpos] + depth
                                           │
camera + sun + point lights + fog + shadow ┘
        └─> LightingRenderPass (fullscreen): reads G-buffer SRVs,
              per pixel: sky if |N|~0 -> fog color;
              else albedo * (ambient + sun*shadow + Σ point) , fog blend
            -> scene color (SRGBA8)
forward: Primitive grid, Outline, Debug, UI -> scene color + shared depth
```

### Editor vs runtime
- The **final color** + **shared depth** come from the existing `sceneBuffer`
  selection in `Renderer::Render` (editor offscreen `SceneViewport` target, or
  the swapchain). Unchanged.
- The **G-buffer colors** are Renderer-owned intermediates in both modes, sized
  to the sceneBuffer. `GBufferFill` writes G-buffer colors + `sceneBuffer.depth`;
  `Lighting` writes `sceneBuffer.color`; forward passes write
  `sceneBuffer.color` + `sceneBuffer.depth`.

### Two-backend parity
- All targets use widely-supported NVRHI formats (`RGBA8_UNORM`,
  `RGBA16_FLOAT`, `D32`); MRT (3 color attachments) is within both DX12 and
  Vulkan limits. Binding layouts follow the existing model, including the
  `VulkanBindingOffsets` (all bindings in descriptor set 0) that the mesh pass
  already uses. Both backends must be validated.

## Error / edge handling
- **Backend hot-swap** (`SwapBackend`): G-buffer textures + framebuffer + the two
  new pipelines are torn down and recreated, same as the existing shadow/mesh
  resources.
- **Resize** (window or editor viewport): per-frame size check recreates the
  G-buffer to match the sceneBuffer; pipelines that bake framebuffer info rebuild
  (the passes already null+rebuild pipelines on first render / resize).
- **No geometry / sky pixels:** handled by the normal-RT geometry mask → fog
  color.
- **Unlit/transparent mesh appears:** not handled (none today); leave a code note
  marking this as the trigger to add a forward path.

## Testing / verification
No unit tests (consistent with this project's renderer work). Verification is
manual and is the crux of "1:1":
- Build + run the **editor**; compare the scene against the pre-deferred build
  (same camera/scene): lighting, shadows, fog, the grid, outlines, debug draw,
  and UI must look identical. Scrub the day/night cycle (sun motion) and confirm
  fog + shadows track as before.
- Repeat on **both** `--backend=d3d12` and `--backend=vk`; toggle between them
  (or via the in-editor swap) and confirm parity.
- `test_ecs` / `test_alloc` are unaffected but must still pass after the build.

## Files (anticipated)
- `src/engine/src/rendering/Renderer.{h,cpp}` — G-buffer ownership, lifecycle,
  resize, accessors; pass registration order.
- `src/engine/src/rendering/passes/GBufferFillPass.{h,cpp}` — new.
- `src/engine/src/rendering/passes/LightingRenderPass.{h,cpp}` — new.
- `src/engine/src/rendering/passes/MeshRenderPass.{h,cpp}` — removed (or gutted).
- `src/engine/src/threading/RenderThread.cpp` — pass construction/order if done
  there.
- `src/engine/CMakeLists.txt` — add/remove pass sources.
- (Editor) `SceneViewport` / overlay — only if the resize hook needs adjustment.
