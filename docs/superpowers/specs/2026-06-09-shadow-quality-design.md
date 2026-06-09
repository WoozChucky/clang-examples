# Shadow Quality Bundle Design

**Status:** Design approved, pre-implementation.
**Branch:** `feat/shadow-quality`
**Depends on:** existing directional shadow path — `ShadowDepthPass`, `LightingRenderPass`, `ShadowMath.h`, single 4096² depth map, back-face render (`cullMode = Front`).

## Goal

Make the single-cascade directional shadow sharper, softer-edged, and acne-free for the modest-zoom isometric ARPG camera — **without** adding a new subsystem (no CSM, PCSS, VSM, GI). Three coordinated changes:

1. **Frustum-fit coverage** — size the light ortho box to what the camera actually sees (capped at a distance) instead of a hardcoded 80-unit box, concentrating the fixed 4096² texels where the player looks.
2. **Poisson-disk rotated PCF** — replace the single comparison tap with a multi-tap rotated Poisson kernel for a soft, grid-free penumbra.
3. **Normal-offset bias** — offset the sample position along the surface normal to kill self-shadow acne without peter-panning; retire the manual depth-bias slider.

Single shadow map only. Visual quality, not a new feature surface.

## Background / current state

- `ShadowDepthPass::Render` (`src/engine/src/rendering/passes/ShadowDepthPass.cpp`) finds the sun, then **centers** the box on the camera's ground-gaze point via `GroundFocus(cam.Position, fwd, coverage)` but **sizes** it with the hardcoded `ShadowSettings::ShadowCoverage` (default 80). It calls `SnapToTexelGrid` then `ComputeLightViewProj(center, radius, sunDir, NearExtend)`. A scene-AABB fallback covers a degenerate camera.
- `ShadowMath.h` already contains a written-but-**unused** `FrustumSliceSphere(camView, camProj, shadowDistance)` that reconstructs the 8 frustum-slice corners (near plane → capped far distance) and bounds them with a world-space sphere (`center`, `radius`). This is exactly the frustum-fit primitive item 1 needs; it is currently never called and has no test.
- `LightingRenderPass` (`src/engine/src/rendering/passes/LightingRenderPass.cpp`) `ShadowFactor` does one `uShadowMap.SampleCmpLevelZero(uShadowSamp, uv, p.z - bias)` → hardware 2×2 bilinear PCF only. `bias = uShadowBias * (1 + (1-ndl)*2)` (slope-scaled). Comment admits edges are "resolution-bound, not filter-bound." Shadow map at `t6`, comparison sampler at `s7`, G-buffer normal at `t2`.
- `cullMode = Front` (renders back faces into the shadow map) → the geometry-thickness gap already substitutes for most depth bias, which is why `ShadowBias ≈ 0` looks best. Thin/flat/single-sided geometry (ground plane, one-quad walls) has no thickness gap and is the only acne risk.

### Settings surface today (all touched by this change)
- `src/common/include/ApplicationContext.h`: `shadowBias=0.0015`, `shadowCoverage=80`, `shadowNearExtend=50`.
- `src/engine/src/utilities/SettingsManager.cpp`: load (`bias`/`coverage`/`nearExtend`) + save (`renderer.shadow.{bias,coverage,nearExtend}`).
- `src/engine/src/core/Application.cpp`: copies `Settings` → `GetShadowSettings()` (`Bias`/`ShadowCoverage`/`NearExtend`).
- `src/engine/src/rendering/RenderStats.h`: `ShadowSettings{ Enabled, Bias, ShadowCoverage, NearExtend }`.
- `src/editor/src/panels/RenderStatsPanel.cpp`: sliders "Shadow bias", "Shadow coverage", "Shadow near-extend".
- `LightingRenderPass.{h,cpp}`: CB field `ShadowBias` / shader `uShadowBias`.

Per the early-dev breaking-changes policy, renaming/removing these fields (and the matching `engine_settings.json` keys) is acceptable — no migration shim.

## Architecture

Three independent changes across the existing shadow path. No new files except a unit test. Data flow unchanged in shape: CPU per-frame fit + settings → shadow CB / lighting CB → shaders.

### 1. Frustum-fit coverage

In `ShadowDepthPass::Render`, replace the `GroundFocus` + fixed-`coverage` sizing block with `FrustumSliceSphere`:

```cpp
const CameraView& cam = m_Renderer->GetActiveCamera();
const glm::vec3 fwd = CameraForward(cam.View);
if (glm::dot(fwd, fwd) > 0.5f) {                       // valid camera basis
    const float dist = glm::max(shadow.ShadowDistance, 1.0f);
    const ShadowSphere s = FrustumSliceSphere(cam.View, cam.Projection, dist);
    radius = glm::max(s.radius, 1.0f);
    center = SnapToTexelGrid(s.center, radius, sunDir, Renderer::kShadowMapSize);
    haveFit = true;
}
```

- The box now auto-fits the visible frustum slice, trimmed at `ShadowDistance`. Smaller distance → smaller box → smaller texels → sharper.
- Keep `SnapToTexelGrid` (now with the per-frame-varying `radius`) and the all-visible-AABB fallback when the camera basis is degenerate. Keep `ComputeLightViewProj(center, radius, sunDir, NearExtend)` and `NearExtend` unchanged.
- `GroundFocus` may become unused here; leave the helper in `ShadowMath.h` (still small, still tested-adjacent) — do not delete shared math.

**Texel world size** (needed by the shader, item 2/3) = `2*radius / kShadowMapSize`. Because `radius` is now dynamic, the CPU must compute this each frame and pass it in the lighting CB; the shader can no longer assume a constant.

### 2. Poisson-disk rotated PCF

Rewrite `ShadowFactor` in the `LightingRenderPass` pixel shader:

- A fixed compile-time Poisson-disk tap table (16 taps, unit-disk offsets) in the shader source.
- Per-pixel rotation angle from a hash of `SV_POSITION.xy` (interleaved-gradient-noise or a cheap hash → angle); build a 2×2 rotation. Rotating the disk per pixel converts banding into dither.
- Each tap offset scaled by `uPcfRadius` (texels) × `uShadowTexel` (the UV-space size of one shadow texel = `1.0 / kShadowMapSize`), applied in shadow **UV** space, then `SampleCmpLevelZero` and averaged over the 16 taps.
- Result replaces the single-tap return.

New lighting-CB fields (CPU→shader): `uPcfRadius` (texels), `uShadowTexel` (UV-space texel = `1/4096`), `uNormalOffsetWorld` (world units = `NormalOffset * texelWorld`, computed CPU-side from the dynamic radius). Keep the CB 16-byte aligned (pack with existing trailing ints / pad as needed; mirror the existing `int uShadowEnabled; float uShadowBias; ...` packing).

### 3. Normal-offset bias

In `ShadowFactor`, before projecting into light space, offset the world position along the (already-bound, `t2`) surface normal:

```hlsl
float3 biasedWP = worldPos + N * uNormalOffsetWorld;   // N = normalized G-buffer normal
float4 lp = mul(uLightVP, float4(biasedWP, 1.0));
```

- `uNormalOffsetWorld = NormalOffset(texels) * texelWorld(world units)` — computed CPU-side (depends on the dynamic radius), passed in the CB.
- **Remove** the `ShadowBias` slider, the `Bias` field, the `shadowBias` setting + JSON key, and the slope-scaled `uShadowBias` term. Keep a small **hardcoded** constant depth bias in the compare (`p.z - kConstShadowBias`, e.g. `5e-4`) as cheap insurance against residual acne; no UI.

### Settings / data flow changes

| Layer | Change |
| --- | --- |
| `ApplicationContext.h` Settings | remove `shadowBias`; rename `shadowCoverage`→`shadowDistance`; add `shadowNormalOffset` (default ~1.0), `shadowPcfRadius` (default ~1.5); keep `shadowNearExtend` |
| `SettingsManager.cpp` | load/save keys: drop `bias`; `coverage`→`distance`; add `normalOffset`, `pcfRadius`; keep `nearExtend` |
| `Application.cpp` | drop `Bias` copy; `ShadowCoverage`→`ShadowDistance`; add `NormalOffset`, `PcfRadius` copies |
| `RenderStats.h` `ShadowSettings` | drop `Bias`; `ShadowCoverage`→`ShadowDistance`; add `NormalOffset`, `PcfRadius` |
| `RenderStatsPanel.cpp` | drop "Shadow bias"; "Shadow coverage"→"Shadow distance"; add "Shadow normal-offset" + "Shadow PCF radius" sliders; keep "Shadow near-extend" |
| `LightingRenderPass.{h,cpp}` CB | drop `ShadowBias`; add `PcfRadius`, `ShadowTexel`, `NormalOffsetWorld`; populate from `GetShadowSettings()` + the per-frame radius |

The per-frame `radius` from `FrustumSliceSphere` lives in `ShadowDepthPass`; `LightingRenderPass` needs `texelWorld`/`ShadowTexel`/`NormalOffsetWorld` derived from it. Publish the radius (or the derived texel size) on `Renderer::ShadowView` (next to `LightVP`/`Enabled`) so `LightingRenderPass` reads it the same way it reads `LightVP`. `ShadowTexel` (UV) is constant `1/kShadowMapSize`; `NormalOffsetWorld` and any world-space scaling use the published radius.

## Components / boundaries

- **`ShadowMath.h`** — pure, header-only, no engine deps. `FrustumSliceSphere` already lives here; this change just calls it and adds its first unit test. New constant `kConstShadowBias` can live in the lighting shader source (shader-local), not here.
- **`ShadowDepthPass`** — owns the per-frame light-box fit; gains the radius publish onto `ShadowView`. No new external surface.
- **`LightingRenderPass`** — owns shadow sampling; the shader and CB change; reads the published radius/texel from `ShadowView`.
- **Settings trio** (`ApplicationContext`/`SettingsManager`/`Application`/`RenderStats`/panel) — mechanical field rename + add/remove, following the existing pattern exactly.

## Testing

- **Unit (new):** `FrustumSliceSphere` in the existing `test_*` target (the renderer-math/host-testable suite — place beside other `ShadowMath` coverage). Assert the returned sphere encloses all 8 reconstructed frustum-slice corners (each corner distance from center ≤ radius + eps) for a representative perspective `View`/`Projection` and a finite `shadowDistance`, and that a far `shadowDistance` cap actually reduces the radius vs an uncapped slice. Pure-math, deterministic.
- **Existing suites** stay green (`test_ecs`, `test_alloc`, any renderer-math tests). PCF/normal-offset are shader-side → no unit test, visual smoke only.
- **Smoke (manual GUI, `msvc-win64-vs2026-community`):**
  1. Zoom in → shadow edges razor-sharp (small box, small texels).
  2. Zoom out (modest max) → still clean, no blocky stairstep; edges soft via PCF, not jagged.
  3. Pan the camera → no shadow crawl/swim (texel-snap holds).
  4. Flat ground + thin/one-quad walls → no acne stripes (normal-offset working).
  5. Character feet / object base → contact shadow stays attached, no peter-panning float.
  6. Toggle shadows off (panel) → shadows gone, scene otherwise unchanged.
  7. No NVRHI validation errors; skinned-mesh shadows still deform correctly (compute-skinned VB path untouched).

## Scope

**In:** frustum-fit via `FrustumSliceSphere` + `ShadowDistance` cap; per-frame radius publish on `ShadowView`; 16-tap rotated Poisson PCF with `PcfRadius` knob; normal-offset bias with `NormalOffset` knob; removal of the depth-bias slider/field/key + hardcoded constant bias; settings rename/add across the trio + editor sliders; one `FrustumSliceSphere` unit test.

**Out:** Cascaded Shadow Maps (multi-cascade) — the natural next step if zoom range ever widens; PCSS / contact-hardening variable penumbra; VSM/ESM/moment maps; any GI / indirect fill; shadow map resolution changes (stays 4096); point/spot-light shadows.

## Risks / notes

- **Dynamic radius vs texel-snap:** `radius` now changes per frame with zoom, so `texelWorld` changes → a frame where radius jumps can momentarily shift the snapped grid (tiny one-frame edge shift on zoom). Acceptable for modest zoom; note it, don't over-engineer. Panning at fixed zoom (radius constant) is crawl-free as before.
- **CB packing:** adding three floats + removing one to the lighting `PerFrame` CB must keep 16-byte alignment; mirror the existing trailing-int packing and pad explicitly. A mismatch silently corrupts shadows — verify the C++ struct and HLSL `cbuffer` field-for-field.
- **Normal source:** `ShadowFactor` must use the **normalized** G-buffer normal (`uNormal`, t2) already read in `main_ps`; pass it in rather than re-sampling. Sky/no-geometry pixels (`dot(N,N) < 0.5`) already early-out before lighting, so no offset on those.
- **PCF cost:** 16 taps × `SampleCmpLevelZero` per lit pixel, full-screen. Fine on desktop; if it ever matters, `PcfRadius`/tap-count is the lever. Not optimizing now.
- **Breaking settings:** old `engine_settings.json` files have `coverage`/`bias` keys that no longer load (and lack the new ones → defaults). Expected per early-dev policy; the gitignored local settings just re-default on first run.
- **`FrustumSliceSphere` correctness:** it assumes a perspective camera (the game camera is perspective — confirmed `CameraFollow.h` uses `glm::perspectiveRH_ZO`). The editor fly-camera is also perspective. The AABB fallback still guards the degenerate case.
