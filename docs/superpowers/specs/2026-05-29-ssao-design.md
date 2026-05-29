# SSAO (Screen-Space Ambient Occlusion)

**Date:** 2026-05-29
**Status:** Approved, pending implementation plan

## Problem / Goal

The deferred renderer has direct shadows (directional) but no ambient occlusion: objects don't
feel "seated," crevices/corners/contact points aren't darkened, and ambient-lit regions read flat.
Add **classic SSAO** — a screen-space pass that darkens the **ambient** lighting term where nearby
geometry occludes it — to add depth/grounding. It's a natural fit: the deferred G-buffer already
carries the world-position + world-normal SSAO needs.

## Non-Goals

- Not HBAO/GTAO (documented as future evolutions behind the same pass; this ships classic SSAO).
- Not half-resolution AO or bilateral blur initially (documented evolutions).
- No change to direct lighting / shadows / SMAA (orthogonal — see Feature Independence).

## Background (verified)

- Deferred pipeline: `GBufferFillPass` writes Albedo (RT0) / world-Normal (RT1, RGBA16F) /
  world-Position (RT2, RGBA16F) + D32 depth. `Renderer` exposes `GetGBufferNormal()`,
  `GetGBufferWorldPos()`, `GetGBufferFramebuffer()`, and `GetActiveCamera()` (`CameraView{View,
  Projection, Position}`).
- `LightingRenderPass` (fullscreen) computes
  `lighting = uAmbientColor.rgb + diffuse*uDir.Color*ShadowFactor(...) + pointLights`, then fog.
  Ambient = `AtmosphereStateComponent.AmbientColor`.
- Renderer-owned-target pattern (shadow map): Renderer owns the texture + framebuffer, a pass
  writes it, `LightingRenderPass` binds it as an SRV. SSAO follows this exactly.
- Engine-tier settings pattern (shadows / aaMode): a RenderThread global in `RenderStats.h` seeded
  at `Application::Init` from an `ApplicationSettings` mirror field, (de)serialized by
  `SettingsManager` into `engine_settings.json`, persisted change-guarded in `ImGuiRenderer`.
- Render passes are Renderer-owned for post/resolve steps (Fxaa/Smaa) — not in `m_RenderPasses`.

## Feature Independence (SSAO vs Shadows vs SMAA)

The three modulate different parts of the pipeline and compose freely — no interactions, no
special-casing:

```
finalLighting = ambient·AO  +  directionalDiffuse·Shadow  +  pointLights      // SSAO -> ambient, Shadows -> directional
then -> [SMAA resolve] on the final color                                      // SMAA -> screen-space post
```

- **SSAO off** → `SsaoRenderPass` skipped; lighting uses AO = 1.0 (no cost, ambient unchanged).
- **Shadows off** → `ShadowFactor` returns 1.0 (existing behavior); directional term unshadowed.
- **SMAA off** → world renders straight to the swapchain (existing AAMode switch).

SSAO depends only on the G-buffer (always produced by the deferred path), **not** on shadows being
enabled. Every on/off combination is valid and behaves sensibly.

## Design

### 1. Pipeline placement & ownership

SSAO runs **after `GBufferFillPass`, before `LightingRenderPass`**.

- **Renderer owns** two R8_UNORM targets — `m_SsaoRaw`, `m_SsaoBlur` — + their framebuffers,
  created at G-buffer resolution and rebuilt on resize. Accessor `GetSsaoTexture()` returns the
  blurred result (or a Renderer-owned 1×1 white texture when SSAO is disabled, so the lighting
  binding layout stays valid).
- New **`SsaoRenderPass`** (Renderer-owned, like `FxaaRenderPass`/`SmaaRenderPass`; not in
  `m_RenderPasses`). Two fullscreen sub-passes: **AO** (writes `m_SsaoRaw`) then **blur** (writes
  `m_SsaoBlur`). Skipped entirely when `SsaoSettings.Enabled == false`.
- `LightingRenderPass` binds `GetSsaoTexture()` as a new SRV (next free slot — t8, sampler reuses
  s4 G-buffer point/clamp; keep slots globally unique for the Vulkan flat binding) and multiplies
  **the ambient term only**: `lighting = uAmbientColor.rgb * ao + ...`.

### 2. AO sub-pass (classic hemisphere, world-space)

Fullscreen triangle. Inputs: G-buffer Normal SRV + WorldPos SRV + a CB carrying camera `ViewProj`,
`Radius`, `Bias`, `Intensity`, `Power`, and the render-target size (for screen-space tap rejection).

Per pixel:
1. Read world pos `P`, world normal `N`. If `dot(N,N) < 0.5` (sky/no geometry) → write AO = 1, return.
2. Build a TBN basis around `N`, rotated per-pixel by an angle from a screen-space hash (no noise
   texture — same approach used by the shadow PCF; the box blur denoises it).
3. For each of `kKernelSize` (16) hemisphere samples (hardcoded cosine-weighted unit-hemisphere
   array in `SsaoMath.h`): `S = P + (TBN * kernel[i]) * Radius`. Project `S` with `ViewProj` →
   clip → screen UV (reject if off-screen). Fetch stored world pos `Pocc` at that UV.
4. Occlusion test: the sample is occluded if `Pocc` is closer to the camera than `S` by more than
   `Bias`, AND within `Radius` of `P` (range check via `smoothstep` on distance — prevents
   haloing from far occluders).
5. `occlusion = (sum / kKernelSize) * Intensity`; `ao = pow(saturate(1 - occlusion), Power)`. Write R8.

### 3. Blur sub-pass

A small **box blur** (4×4) over `m_SsaoRaw` → `m_SsaoBlur` to denoise the per-pixel-rotation grain
(the classic SSAO pairing). Cheap; reads/writes R8.

### 4. Settings (engine tier)

`RenderStats.h`:
```cpp
struct SsaoSettings {
    bool  Enabled   = true;
    float Radius    = 0.5f;    // world units
    float Intensity = 1.0f;    // occlusion strength multiplier
    float Power     = 2.0f;    // contrast (pow on AO)
    float Bias      = 0.025f;  // depth bias to avoid self-occlusion
};
ENGINE_API SsaoSettings& GetSsaoSettings();
```
Engine-tier persistence mirroring shadows: `ApplicationSettings` mirror fields (`ssaoEnabled`,
`ssaoRadius`, `ssaoIntensity`, `ssaoPower`, `ssaoBias`); `SettingsManager` (de)serialize a
`renderer.ssao.{enabled,radius,intensity,power,bias}` block (guarded reads); seed in
`Application::Init`; persist change-guarded in `ImGuiRenderer`. Panel: a new **"SSAO"** section in
`RenderStatsPanel` — toggle + Radius/Intensity/Power/Bias sliders.

`LightingRenderPass` gets a `uSsaoEnabled` int in its CB (like `uShadowEnabled`); when 0 it uses
AO = 1.0 without sampling, so the bound 1×1-white path is belt-and-suspenders.

### 5. Documented evolutions (keep the same pass I/O + ambient-multiply consumption)

- **HBAO** — replace the kernel loop with horizon-marching along screen directions (better contact
  shadows, more taps). Shader-swap only.
- **GTAO** — physically-based visibility integral (+ optional multi-bounce). Best quality;
  shader-swap + a slightly richer CB.
- **Half-res AO + bilateral upsample** — render AO at half resolution and depth-aware upsample for
  perf; AO is low-frequency so quality loss is small.
- **Bilateral (depth-aware) blur** — replace the box blur to stop AO bleeding across depth edges.

### 6. Error handling

- Missing G-buffer → `SsaoRenderPass` returns (skip), lighting uses AO=1 / 1×1 white.
- `Enabled == false` → AO/blur sub-passes skipped; `GetSsaoTexture()` returns the 1×1 white tex and
  `uSsaoEnabled = 0`.
- Resize → drop sub-pass pipelines + the two AO targets/framebuffers, rebuild at new size.
- Init failure (shaders/targets) → `SM_ERROR`, reset the pass; lighting falls back to AO=1 with a
  one-shot `SM_WARN`.

### 7. Testing

- **Unit (pure, `SsaoMath.h` → `tests/test_ssaomath.cpp`):** factor the kernel generation + the
  project-and-compare occlusion decision into pure functions. Cases: kernel samples lie in the +Z
  hemisphere and are within the unit sphere (cosine-weighted bias toward center); a sample sitting
  behind a nearer occluder (within range) is flagged occluded; an occluder beyond `Radius` is
  rejected (range check); `Bias` prevents self-occlusion of a coplanar sample.
- **Manual:** toggle SSAO on/off → contact darkening at box bases + creases appears/disappears;
  Radius/Intensity/Power sliders behave; no haloing blowups; combine with Shadows on/off and SMAA
  on/off (all combinations valid); `runtime.exe` honors the persisted setting.

## Components & Boundaries

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `SsaoMath.h` | Pure kernel + occlusion-test helpers | glm |
| `SsaoRenderPass` | AO + blur sub-passes → AO target | Renderer (G-buffer SRVs, camera, AO targets), nvrhi |
| Renderer AO targets + `GetSsaoTexture()` | Own/resize targets, run pass between G-buffer and lighting | nvrhi |
| `LightingRenderPass` | Multiply ambient by AO | Renderer (AO SRV) |
| `SsaoSettings` + panel + engine persistence | Toggle + tunables, runtime-honored | nlohmann::json, ImGui |

## Files Touched

- `src/engine/src/rendering/SsaoMath.h` (new) — pure helpers.
- `src/engine/src/rendering/passes/SsaoRenderPass.{h,cpp}` (new) + `src/engine/CMakeLists.txt`.
- `src/engine/src/rendering/Renderer.{h,cpp}` — AO targets, `m_SsaoPass`, `GetSsaoTexture()`, run between G-buffer and lighting; resize/shutdown.
- `src/engine/src/rendering/passes/LightingRenderPass.cpp` — AO SRV bind + ambient multiply + `uSsaoEnabled`.
- `src/engine/src/rendering/RenderStats.h` — `SsaoSettings` + getter.
- `src/common/include/ApplicationContext.h` — `ssao*` mirror fields.
- `src/engine/src/utilities/SettingsManager.cpp` — `renderer.ssao` (de)serialize.
- `src/engine/src/core/Application.cpp` — seed `GetSsaoSettings()`.
- `src/editor/src/app/ImGuiRenderer.cpp` — change-guarded persist.
- `src/editor/src/panels/RenderStatsPanel.cpp` — SSAO section.
- `tests/test_ssaomath.cpp` + `tests/CMakeLists.txt`.
