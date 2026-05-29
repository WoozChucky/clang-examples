# Ground-Focus Shadow Box

**Date:** 2026-05-29
**Status:** Approved, pending implementation plan
**Branch:** `feat/camera-fit-shadows` (continues the camera-fit work; merge ONLY if this produces good shadows)

## Problem

The camera-frustum-slice fit (shipped on this branch) sharpens shadows for a *close* camera (the
editor free-cam) but not for the game's **isometric follow camera**: that camera sits ~22 units
back and sees a wide ground area, so the frustum-slice bounding sphere is large → the 2048² map
spreads thin → jagged shadows. Worse, `ShadowDistance` (depth from the camera near plane) is
unintuitive for a distant camera — a small value puts the shadow slice in the air near the camera
and produces *no* shadows until cranked up, at which point density is poor anyway. Confirmed in
play mode: shadows look terrible regardless of slider values.

## Goal

Make shadow density **independent of camera distance** by fitting the light ortho to a fixed-size
box centered on the ground point the camera is looking at. Density becomes a direct, tunable
function of box size (`2048 / coverage`), giving consistent, sharp shadows on the gameplay area for
the isometric camera.

## Non-Goals

- No CSM (still parked).
- No shadow-filtering change (PCF stays 3×3 — a separate follow-up; this spec is purely the *fit*).
- No new persisted settings field this iteration (repurpose the existing `ShadowDistance`).

## Background (this branch, verified)

- `ShadowDepthPass::Render` currently fits via `FrustumSliceSphere(cam.View, cam.Projection,
  ShadowDistance)` → `SnapToTexelGrid` → `ComputeLightViewProj(center, radius, sunDir, NearExtend)`.
- The active camera is `CameraView{ View, Projection, Position(eye) }` from
  `m_Renderer->GetActiveCamera()`.
- `ShadowSettings` (engine tier now) has `Enabled`, `Bias`, `ShadowDistance`, `NearExtend`.
- `ComputeLightViewProj(center, radius, sunDir, nearExtend)` and `SnapToTexelGrid(center, radius,
  sunDir, mapSize)` are pure helpers in `ShadowMath.h` (unit-tested). The ortho is `[-r, r]` in the
  light's view plane, so `radius` = half the covered width.

## Design

### 1. New pure helpers (`ShadowMath.h`, unit-testable)

```cpp
// World forward direction from a view matrix (camera looks down -Z in view space).
inline glm::vec3 CameraForward(const glm::mat4& view) {
    return -glm::normalize(glm::vec3(view[0][2], view[1][2], view[2][2]));
}

// Ground point under the camera's gaze: intersect the forward ray (from eye) with the plane
// y = groundY. If the ray does not descend (looking up/level), fall back to eye + forward*fallbackDist.
inline glm::vec3 GroundFocus(const glm::vec3& eye, const glm::vec3& forward,
                             float fallbackDist, float groundY = 0.0f) {
    if (forward.y < -1e-4f) {
        const float t = (groundY - eye.y) / forward.y;
        if (t > 0.0f) return eye + forward * t;
    }
    return eye + forward * fallbackDist;
}
```

### 2. Fit change (`ShadowDepthPass::Render`)

Replace the `FrustumSliceSphere` camera-fit block with:

```cpp
const CameraView& cam = m_Renderer->GetActiveCamera();
const glm::vec3 fwd   = CameraForward(cam.View);
const float coverage  = glm::max(shadow.ShadowDistance, 1.0f);   // box width (world units)
if (glm::dot(fwd, fwd) > 0.5f) {                                 // valid camera basis
    const glm::vec3 focus = GroundFocus(cam.Position, fwd, coverage);
    radius = coverage * 0.5f;                                    // ortho half-extent
    center = SnapToTexelGrid(focus, radius, sunDir, /*shadowMapSize=*/2048);
    haveFit = true;
}
// retain the all-visible-meshes AABB fallback (degenerate camera) unchanged
const glm::mat4 lightVP = ComputeLightViewProj(center, radius, sunDir, shadow.NearExtend);
```

`ShadowDistance` now means **box coverage width**; texel density = `2048 / coverage` (e.g.
coverage 30 → ~0.0146 u/texel). The box auto-centers on the ground under the camera, so the iso
cam gets dense, sharp shadows on the play area without "cranking" — and the editor cam centers on
whatever ground point it looks at.

`FrustumSliceSphere` is retained in `ShadowMath.h` (now unused by the pass) — kept for its tests
and possible future CSM use; remove only if it bothers us.

### 3. Settings / panel

- **No struct/persistence change.** Repurpose `ShadowSettings.ShadowDistance` as the box coverage.
- `RenderStatsPanel.cpp`: relabel the slider **"Shadow coverage"**, range ~5–200.
- `NearExtend` unchanged (still pulls the light near plane toward the sun for off-screen casters).

### 4. PRE-MERGE GATE (blocking, required if this earns a merge)

If the ground-focus box produces good shadows and we decide to merge, **rename `ShadowDistance` →
`ShadowCoverage`** everywhere before merging: `RenderStats.h`, `ApplicationContext.h`
(`shadowDistance` → `shadowCoverage`), `SettingsManager.cpp` (JSON key `distance` → `coverage`),
`Application.cpp` seed, `ImGuiRenderer.cpp` persist guard, `RenderStatsPanel.cpp`. This is a
required task in the plan, gated on the merge decision (skip if the branch is scrapped).

## Error Handling

- Degenerate camera (zero forward basis) → all-visible-meshes AABB fallback (unchanged), one-shot
  `SM_WARN`.
- Ray misses ground (camera looking up) → `GroundFocus` falls back to `eye + forward*coverage`.
- `coverage` clamped to ≥ 1.

## Testing

- **Unit (`tests/test_shadowmath.cpp`):** `CameraForward` — a known `glm::lookAtRH` yields the
  expected normalized forward; `GroundFocus` — a downward ray hits the ground at the expected
  point, an upward/level ray returns the `eye + forward*fallback` fallback. Existing 8 cases stay.
- **Manual (the real judge):** in **play mode** (isometric cam), shadows are sharp on the play
  area at a sane coverage (~30) with no cranking; the coverage slider trades sharpness vs area;
  panning the camera shows no edge crawl (texel snap); off-screen casters still cast in (near-extend).

## Files Touched

- `src/engine/src/rendering/ShadowMath.h` — `CameraForward`, `GroundFocus`.
- `src/engine/src/rendering/passes/ShadowDepthPass.cpp` — swap the fit.
- `src/editor/src/panels/RenderStatsPanel.cpp` — slider relabel + range.
- `tests/test_shadowmath.cpp` — 2 new cases.
- (PRE-MERGE GATE only) the 6 files above for the `ShadowCoverage` rename.

## Decision hook

This is the second fit strategy tried on this branch. If the ground-focus box gives good play-mode
shadows → do the rename gate, then merge; the remaining edge jaggedness is a *filtering* issue →
next feature is better PCF (CSM stays parked). If it still looks bad → the branch may be scrapped
and shadow quality rethought (filtering-first, or CSM).
