# Camera-Frustum-Fit Directional Shadows

**Date:** 2026-05-29
**Status:** Approved, pending implementation plan

## Problem

Directional shadows look blocky/pixelated up close. Root cause (verified): `ShadowDepthPass`
fits the light's orthographic frustum to the **world-space AABB of all visible meshes**
(`ShadowDepthPass.cpp:92-116`) and renders it into a single fixed **2048×2048** depth map. The
ortho half-extent is the scene's bounding-sphere radius, so each shadow texel covers a large
world-space area — and it covers *more* as the scene grows. Up close, one texel spans many
screen pixels → visible stair-stepping. 3×3 PCF (`LightingRenderPass`, `texel = 1/2048`) only
mildly softens it.

## Goal

Concentrate the fixed 2048² shadow map's texels on the region the camera actually sees, so
near-camera shadows gain resolution. Provide live-tunable knobs to evaluate the result and
inform whether CSM (the heavier, "proper" fix) is worth doing next.

## Non-Goals

- No Cascaded Shadow Maps (separate, larger follow-up).
- No change to shadow-map resolution, PCF kernel, bias model, or the depth render itself.
- No new render pass; this modifies the existing `ShadowDepthPass` frustum fit only.
- No lateral off-screen-caster coverage beyond the near-Z extension (a radius-pad knob can be
  added later if missing side-shadows show up).

## Background (verified)

- `ShadowMath.h::ComputeLightViewProj(center, radius, sunDir)` builds the light ortho: looks
  down `sunDir` from `eye = center - d*(2r)`, ortho `[-r,r]×[-r,r]`, depth range `[0, 4r]`. The
  scene sphere sits centered in that depth range. RH + ZO.
- `ShadowDepthPass::Render` currently derives `center` = midpoint and `radius` =
  half-diagonal of the all-visible-meshes AABB, calls `ComputeLightViewProj`, publishes
  `Renderer::ShadowView{ LightVP, Enabled }`, then depth-renders all visible meshes.
- The active camera (`CameraView{ View, Projection, Position }`, trivially-copyable) is
  available via `m_Renderer->GetActiveCamera()` (already used by `SkyRenderPass`).
- `ShadowSettings { bool Enabled; float Bias; }` (`RenderStats.h`) is a RenderThread global
  (`GetShadowSettings()`), edited in the Render Stats panel, and **persisted in
  `editor_preferences.json`** via `EditorPreferences.h` (`PrefsToJson`/`PrefsFromJson`,
  `shadows.enabled`/`shadows.bias`, guarded partial-file-safe reads).

## Design

### 1. Fit the ortho to a capped camera-frustum slice (`ShadowDepthPass::Render`)

Replace the all-meshes AABB fit with:

1. Fetch the active camera (`View`, `Projection`).
2. Compute the **world-space corners of the camera frustum slice from camera-near →
   `ShadowDistance`**: unproject the 8 NDC cube corners with `inverse(Projection*View)`, with the
   far corners capped at view-space depth `ShadowDistance` (reconstruct the capped far corners by
   interpolating each near→far edge to the `ShadowDistance` plane).
3. **Bound the 8 corners with a sphere:** `center` = mean of corners, `radius` = max distance
   from `center` to any corner. With the fixed-angle iso camera (constant pitch/yaw/FOV) the
   radius is stable except on zoom, which keeps shadow density steady and avoids extent-driven
   shimmer.
4. **Texel-snap `center`:** quantize so the ortho moves in whole-texel steps as the camera pans.
   `worldUnitsPerTexel = (2*radius) / SHADOW_MAP_SIZE` (SHADOW_MAP_SIZE = 2048). Snap the center
   in light space: build the light view (no translation) basis, project `center` onto the light's
   right/up axes, `round(coord / worldUnitsPerTexel) * worldUnitsPerTexel`, reconstruct. Removes
   edge crawl during panning.
5. Call `ComputeLightViewProj(center, radius, sunDir, NearExtend)` (new param, §2).

### 2. Near-plane extension toward the sun (`ShadowMath.h`)

Add a defaulted parameter (backward-compatible — existing callers and any test pass `0`):

```cpp
inline glm::mat4 ComputeLightViewProj(const glm::vec3& center, float radius,
                                      const glm::vec3& sunDir, float nearExtend = 0.0f) {
    ...
    const glm::vec3 eye = center - d * (2.0f * r + nearExtend);
    const glm::mat4 proj = glm::orthoRH_ZO(-r, r, -r, r, 0.0f, 4.0f * r + nearExtend);
    ...
}
```

This pulls the near plane `nearExtend` further toward the sun so casters sitting between the
light and the visible slice (e.g. a tall object just out of frame under an angled sun) still
render into the depth map, while X/Y stay tight for maximum density.

### 3. Extract pure, testable helpers (`ShadowMath.h`)

Factor the new math into pure functions so they're unit-testable without a GPU:

- `FrustumSliceSphere(const glm::mat4& invViewProj, /* camera */ ..., float shadowDistance) -> {center, radius}`
  (or a struct) — the corner reconstruction + bounding sphere. Exact signature is a plan detail;
  it must take what it needs to compute the 8 capped corners and return center+radius.
- `SnapToTexelGrid(center, radius, sunDir, shadowMapSize) -> center` — the light-space
  quantization.

`ComputeLightViewProj` stays in `ShadowMath.h` with the new defaulted param.

### 4. Settings + panel + persistence (move ShadowSettings to the engine tier)

`ShadowSettings` (`RenderStats.h`, the RenderThread live global the lighting/shadow passes read)
gains two fields:
```cpp
float ShadowDistance = 80.0f;   // world units of the camera-frustum slice the map covers
float NearExtend     = 50.0f;   // light-space near-plane pull-back toward the sun
```

**Tiering move:** shadows are a runtime rendering feature (`runtime.exe`'s RenderThread reads
`GetShadowSettings()`), but `ShadowSettings` currently persists in `editor_preferences.json`, so
`runtime.exe` never sees tuned values. Move **all** of `ShadowSettings` to the **engine tier**
(`engine_settings.json`), mirroring the `aaMode` pattern, so both exes honor the tuned shadows.

- **`ApplicationSettings`** (`ApplicationContext.h`) gains four mirror fields (flat, matching the
  existing `aaMode` style):
  ```cpp
  bool  shadowEnabled      = true;
  float shadowBias         = 0.0015f;
  float shadowDistance     = 80.0f;
  float shadowNearExtend   = 50.0f;
  ```
- **`SettingsManager`** (`SettingsManager.cpp`): (de)serialize a `renderer.shadow` sub-object
  (`{enabled, bias, distance, nearExtend}`) — guarded reads in load (missing keys keep defaults),
  written in save alongside `renderer.backend`/`renderer.aaMode`.
- **Startup seed** (`Application.cpp`, next to the `aaMode` seed): copy the four persisted values
  into the live global so both exes' RenderThread render with them:
  ```cpp
  ShadowSettings& sh = GetShadowSettings();
  sh.Enabled       = m_AppContext->Settings.shadowEnabled;
  sh.Bias          = m_AppContext->Settings.shadowBias;
  sh.ShadowDistance= m_AppContext->Settings.shadowDistance;
  sh.NearExtend    = m_AppContext->Settings.shadowNearExtend;
  ```
- **Panel** (`RenderStatsPanel.cpp`): the existing Enabled/Bias controls plus two new
  `SliderFloat`s (ShadowDistance ~10–500, NearExtend ~0–200) in the Shadows section. Live tuning.
- **Editor save path** (`ImGuiRenderer.cpp`): the Render Stats panel currently persists shadow
  edits via `EditorPreferences::Save`. After the move, shadow edits persist to
  `engine_settings.json` instead — sync the four `GetShadowSettings()` fields into
  `m_AppContext->Settings` and `SettingsManager::Save` only-on-change, exactly like the existing
  `aaMode` change-guard block.
- **Remove shadows from `EditorPreferences`** (`EditorPreferences.h` + `.cpp`): drop the
  `"shadows"` block from `PrefsToJson`/`PrefsFromJson` and the `ShadowSettings&` parameter; update
  the two call sites in `EditorPreferences.cpp` to stop passing `GetShadowSettings()`.
- **Migration:** no cross-file migration. Old `editor_preferences.json` `"shadows"` keys are
  simply ignored (and no longer written); the engine-tier values start at defaults until tuned
  once. Old `engine_settings.json` lacking `renderer.shadow` loads as defaults (guarded reads).

### 5. Error handling / fallback

- **No active camera, or degenerate frustum (radius ≤ epsilon):** fall back to the current
  all-visible-meshes AABB fit (retain that code as the fallback branch), logging once via
  `SM_WARN`. (Per project convention: log on degradation, never silent-skip.)
- **Sun down / no visible meshes:** unchanged early-outs.
- **`ShadowDistance` clamped to ≥ camera near plane** before use.

## Components & boundaries

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `ShadowMath::ComputeLightViewProj` | Light ortho VP (now with near-extend) | glm |
| `ShadowMath::FrustumSliceSphere` | Capped camera-frustum slice → bounding sphere | glm |
| `ShadowMath::SnapToTexelGrid` | Anti-shimmer light-space quantization | glm |
| `ShadowDepthPass::Render` | Orchestrate fit (camera → sphere → snap → VP), fallback, depth render | Renderer (camera, shadow FB), ShadowMath, ShadowSettings |
| `ShadowSettings` (live global) + panel | Tunables the RenderThread reads | — |
| `ApplicationSettings` + SettingsManager + Application seed | Engine-tier persistence + startup seed (both exes) | nlohmann::json |

## Testing

- **Unit (`tests/test_shadowmath.cpp`, new):**
  - `ComputeLightViewProj(..., nearExtend=0)` is bit-for-bit the pre-change matrix (regression
    guard).
  - `nearExtend > 0` moves the eye/near plane by the expected amount (e.g. a point just beyond
    the old near plane toward the sun now projects inside `[0,1]` depth).
  - `FrustumSliceSphere`: the returned sphere contains all 8 reconstructed corners; capping at a
    smaller `ShadowDistance` yields a smaller radius.
  - `SnapToTexelGrid`: a sub-texel center shift produces no change; a >1-texel shift snaps to the
    grid; output stays within a texel of the input.
- **Manual:** drive the panel sliders live — near shadows sharpen as `ShadowDistance` drops; pan
  the camera → no edge crawl (snapping); an object just off-screen still casts into view
  (near-extend); compare against `main`. **Persistence:** edit shadow settings, restart → values
  restored from `engine_settings.json` (`renderer.shadow`); confirm an old `editor_preferences.json`
  with a stale `"shadows"` block no longer drives shadows (engine tier wins) and isn't rewritten.

## Files Touched

Camera-fit core:
- `src/engine/src/rendering/ShadowMath.h` — `nearExtend` param + `FrustumSliceSphere` /
  `SnapToTexelGrid` pure helpers.
- `src/engine/src/rendering/passes/ShadowDepthPass.cpp` — camera-frustum fit + snap + fallback.
- `src/engine/src/rendering/RenderStats.h` — `ShadowDistance`, `NearExtend` on `ShadowSettings`.
- `src/editor/src/panels/RenderStatsPanel.cpp` — two new sliders.

Engine-tier persistence move:
- `src/common/include/ApplicationContext.h` — 4 `shadow*` mirror fields on `ApplicationSettings`.
- `src/engine/src/utilities/SettingsManager.cpp` — (de)serialize `renderer.shadow`.
- `src/engine/src/core/Application.cpp` — seed `GetShadowSettings()` from persisted settings.
- `src/editor/src/app/ImGuiRenderer.cpp` — persist shadow edits to engine_settings (change-guarded).
- `src/editor/src/app/EditorPreferences.h` + `.cpp` — remove the `"shadows"` block + the
  `ShadowSettings&` parameter from `PrefsToJson`/`PrefsFromJson` and their call sites.

Tests:
- `tests/test_shadowmath.cpp` + `tests/CMakeLists.txt` — unit tests.

## Reassessment hook

This is the cheap experiment before committing to CSM. After it lands, evaluate live: if a tuned
`ShadowDistance` gives acceptable near-shadow sharpness across normal zoom, CSM may be unnecessary;
if sharpness still falls short at usable coverage distances, that's the signal to build CSM next.
