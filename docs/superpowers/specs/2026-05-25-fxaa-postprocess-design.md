# FXAA Post-Process Anti-Aliasing — Design

**Date:** 2026-05-25
**Status:** Approved (design); pending spec review
**Scope:** Engine-only. No ECS.h / GAME_API_VERSION change.

## Problem

The deferred renderer has no anti-aliasing. The fixed isometric game camera makes this
especially visible: most mesh silhouettes are long near-45° edges, which alias hardest.
We want a cheap post-process anti-aliasing option that can be enabled/disabled at runtime
and persists as an engine-wide setting.

## Goal

Add FXAA (Fast Approximate Anti-Aliasing) as a full-screen post-process resolve in the
deferred pipeline, toggleable live, persisted in `engine_settings.json`, applied to the
3D scene only (UI text/quads stay sharp), and behaving identically to today when disabled.

## Constraints / context

- Pipeline is **deferred**. World passes currently render directly into a `sceneBuffer`:
  - **Editor**: an overlay-provided offscreen `SceneViewportColor` texture
    (`isShaderResource = true`), which ImGui samples into the viewport.
  - **Runtime**: the swapchain backbuffer directly (no overlay).
- FXAA needs scene color as a **sampleable SRV** and writes to a **different** target
  (in ≠ out). The editor scene target already is an SRV; the runtime backbuffer is not.
- Pass loop today: `Renderer::Render` iterates `m_RenderPasses` rendering each into the
  single `sceneBuffer` (`Renderer.cpp` ~line 278). Active order: ShadowDepth, GBufferFill,
  Lighting, Sky, Outline, Debug, Ui.
- Vulkan uses **flat binding**; samplers/SRVs/CBs collide unless binding offsets are set.
  `SkyRenderPass` / `LightingRenderPass` already handle this — mirror them.
- Existing toggle precedents in `RenderStats.h`: `GetDebugDrawSettings()`,
  `GetShadowSettings()` — engine-exported globals, touched only on the RenderThread (mesh
  pass / ImGui overlay both run there → no locks). The editor's debug/shadow toggles
  persist in **editor_preferences.json**; FXAA instead persists in **engine_settings.json**
  (engine tier, so `runtime.exe` reads its default too).

## Architecture: the resolve split

Introduce a Renderer-owned offscreen scene-color SRV and a resolve step. Pass-stage
classification decides which passes draw before vs. after the resolve.

```
[FXAA off]  world passes ──────────────────► sceneBuffer ──► Overlay(UI) ──► present
[FXAA on]   world passes ► m_SceneColor ─FXAA─► sceneBuffer ──► Overlay(UI) ──► present
```

### `m_SceneColor` (Renderer-owned intermediate)

- New color texture: `isRenderTarget = true`, `isShaderResource = true`,
  `keepInitialState = true`, `initialState = ShaderResource` (same recipe as
  `SceneViewport.cpp` so NVRHI auto-transitions RT↔SRV across command lists).
- Format matches the scene target's color format.
- Framebuffer = `m_SceneColor` (color) + **the existing shared depth** (the same depth
  texture `EnsureGBuffer` already shares), so GBuffer→Lighting→Sky→Outline→Debug
  depth-testing is unchanged.
- Sized to the scene target; recreated on size change exactly like `m_GBuffer`
  (`EnsureSceneColor(w, h, sharedDepth)` + `ReleaseSceneColor()`), released with the
  backend (Shutdown / TeardownForSwap).
- **Lazily allocated only when FXAA is enabled.** When disabled, never allocated.

### Pass-stage classification

- Add to `IRenderPass`:
  ```cpp
  enum class RenderStage { World, Overlay };
  virtual RenderStage Stage() const { return RenderStage::World; }
  ```
- `UiRenderPass` overrides → `RenderStage::Overlay`. All other passes default to `World`.
- `Renderer::Render` splits the single loop into:
  1. **World target** = (FXAA enabled ? `m_SceneColorFb` : `sceneBuffer`). Scene clear
     goes to the world target. Loop World-stage passes into it.
  2. **Resolve** (only if FXAA enabled): run `FxaaRenderPass` reading `m_SceneColor` SRV,
     writing `sceneBuffer`.
  3. **Overlay**: loop Overlay-stage passes into `sceneBuffer`.

### Zero overhead when disabled

When FXAA is off, the world target *is* `sceneBuffer`, no `m_SceneColor` is allocated, and
no resolve runs — byte-identical to today's path.

## FxaaRenderPass

- New files: `src/engine/src/rendering/passes/FxaaRenderPass.{h,cpp}` + an `.hlsl` shader.
- Full-screen triangle (no vertex buffer), mirroring `SkyRenderPass`'s structure.
- Inputs: `m_SceneColor` SRV + a bilinear **clamp** sampler.
- Constant buffer: `rcpFrame` = `(1/width, 1/height)` + hardcoded FXAA 3.11 quality
  constants (`EDGE_THRESHOLD`, `EDGE_THRESHOLD_MIN`, `SUBPIX`). No exposed knobs (YAGNI).
- Shader: standard FXAA 3.11 luma-edge algorithm. Luma computed from RGB in-shader
  (no luma-in-alpha prepass).
- Compiled via DXC like every other pass (`ShaderCompiler`).
- **Vulkan flat binding**: set binding offsets exactly as `SkyRenderPass` /
  `LightingRenderPass` do (CB / SRV / sampler), or samplers and SRVs collide.
- It is a **Renderer-owned resolve step, not an `m_RenderPasses` entry** — it needs both a
  source SRV and a destination framebuffer, which the single-framebuffer
  `IRenderPass::Render` signature does not express. Renderer owns the pass instance and
  invokes it explicitly between the World and Overlay loops.

## Settings & toggle

### Persistence (`engine_settings.json`)

- `ApplicationSettings` (`ApplicationContext.h`) gains `bool fxaaEnabled = true;`.
- `SettingsManager`:
  - Load: read `j["renderer"]["fxaa"]` if present & boolean → `out->fxaaEnabled`;
    missing key leaves the default (`true`). (Matches existing unknown-key tolerance.)
  - Save: write `j["renderer"]["fxaa"] = settings.fxaaEnabled;`.

### Live read (RenderThread)

- New accessor in `RenderStats.h` / `RenderStats.cpp`:
  ```cpp
  struct AntiAliasingSettings { bool FxaaEnabled = true; };
  ENGINE_API AntiAliasingSettings& GetAntiAliasingSettings();
  ```
  Same single-instance-exported pattern as `GetShadowSettings()`. `Renderer::Render`
  reads `GetAntiAliasingSettings().FxaaEnabled` once per frame to choose the path.
- **Seed at startup** from the loaded `ApplicationSettings.fxaaEnabled` in **both**
  `src/editor/src/main.cpp` and `src/runtime/src/main.cpp` (after `SettingsManager::Load`).

### Editor toggle

- Checkbox in `RenderStatsPanel.cpp` alongside the Shadow/Debug-draw toggles, flips
  `GetAntiAliasingSettings().FxaaEnabled` live.
- On change: mirror the value into the editor's `ApplicationSettings.fxaaEnabled` and save
  `engine_settings.json` via the centralized settings-save in `ImGuiRenderer.cpp`.
  (Distinct from the debug toggles, which persist in editor_preferences.json.)

## Testing

- The shader / GPU resolve is not unit-testable here → covered by **GUI smoke**:
  - FXAA on: near-45° mesh edges visibly smoother under the iso camera.
  - FXAA off: frame identical to current behavior (no regression, no new alloc).
  - UI text stays sharp regardless.
  - Verify both **DX12 and Vulkan** backends (flat-binding regression risk).
- **Pure-testable slice**: `SettingsManager` `fxaa` round-trip + missing-key-defaults-true,
  added to the existing settings test pattern (`tests/`). Save with `fxaaEnabled=false`,
  reload, expect `false`; load JSON without the key, expect default `true`.

## Touch list

- `src/engine/src/rendering/IRenderPass.h` — `RenderStage` enum + `Stage()` virtual.
- `src/engine/src/rendering/passes/UiRenderPass.h` — `Stage()` override → `Overlay`.
- `src/engine/src/rendering/Renderer.{h,cpp}` — `m_SceneColor` + `EnsureSceneColor` /
  `ReleaseSceneColor`, owned `FxaaRenderPass`, World/Overlay loop split, resolve invocation,
  per-frame toggle read, release in Shutdown/TeardownForSwap.
- `src/engine/src/rendering/passes/FxaaRenderPass.{h,cpp}` — new pass.
- FXAA `.hlsl` shader — new, in the shaders location used by other passes.
- `src/engine/src/rendering/RenderStats.h` / `RenderStats.cpp` — `AntiAliasingSettings` +
  exported accessor.
- `src/common/include/ApplicationContext.h` — `ApplicationSettings.fxaaEnabled`.
- `src/engine/src/utilities/SettingsManager.cpp` — load/save `renderer.fxaa`.
- `src/editor/src/main.cpp`, `src/runtime/src/main.cpp` — seed `GetAntiAliasingSettings()`.
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` — toggle checkbox.
- `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` — persist on change (centralized save).
- `src/engine/CMakeLists.txt` — add `FxaaRenderPass.cpp` (+ shader if listed).
- `tests/` — settings round-trip test (+ register target in `tests/CMakeLists.txt`).

## Non-goals (YAGNI)

- No SMAA/TAA/MSAA (separate future work; MSAA is a poor fit for deferred anyway).
- No exposed FXAA quality sliders.
- No general post-process stack / ping-pong chain — single fixed resolve only.
- No render-scale / SSAA.
