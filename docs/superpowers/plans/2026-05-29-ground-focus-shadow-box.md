# Ground-Focus Shadow Box Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the camera-frustum-slice shadow fit with a fixed-size box centered on the ground point under the camera's gaze, so shadow density is independent of camera distance (fixing the isometric play-cam).

**Architecture:** Two new pure helpers in `ShadowMath.h` (`CameraForward`, `GroundFocus`); `ShadowDepthPass::Render` swaps `FrustumSliceSphere` for ground-focus + fixed half-extent (`ShadowDistance` repurposed as box coverage); reuses `SnapToTexelGrid` + `ComputeLightViewProj(...,NearExtend)` unchanged. A gated final task renames `ShadowDistance`→`ShadowCoverage` ONLY if the result earns a merge.

**Tech Stack:** C++23, GLM (RH, ZO), NVRHI, Dear ImGui.

**Spec:** `docs/superpowers/specs/2026-05-29-ground-focus-shadow-box-design.md`

**Branch:** `feat/camera-fit-shadows` (exploratory — merge only if shadows look good).

---

## Build & Test Reference

- Configure: `cmake --preset msvc-win64-vs2026-community` (community only).
- Build: `--target <Engine|editor|test_shadowmath>`. Test exe: `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- Commit identity configured globally; never `--no-verify`.

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `src/engine/src/rendering/ShadowMath.h` | `CameraForward`, `GroundFocus` pure helpers | 1 |
| `tests/test_shadowmath.cpp` | 2 new unit cases | 1 |
| `src/engine/src/rendering/passes/ShadowDepthPass.cpp` | swap frustum-fit → ground-focus box | 2 |
| `src/editor/src/panels/RenderStatsPanel.cpp` | relabel slider "Shadow coverage" + range | 2 |
| (gated) RenderStats.h / ApplicationContext.h / SettingsManager.cpp / Application.cpp / ImGuiRenderer.cpp / RenderStatsPanel.cpp | `ShadowDistance`→`ShadowCoverage` rename | 3 |

---

## Task 1: CameraForward + GroundFocus pure helpers (TDD)

**Files:**
- Modify: `src/engine/src/rendering/ShadowMath.h`
- Modify: `tests/test_shadowmath.cpp`

- [ ] **Step 1: Add the failing tests.** In `tests/test_shadowmath.cpp`, add these two functions before `main()`:

```cpp
// CameraForward: a lookAtRH camera at (0,10,20) looking at origin faces toward (0,-?,-?) with a
// normalized direction equal to normalize(target - eye).
static void T08_camera_forward()
{
    const glm::vec3 eye(0, 10, 20), target(0, 0, 0);
    const glm::mat4 view = glm::lookAtRH(eye, target, glm::vec3(0, 1, 0));
    const glm::vec3 fwd = CameraForward(view);
    const glm::vec3 expected = glm::normalize(target - eye);
    EXPECT(near(fwd.x, expected.x));
    EXPECT(near(fwd.y, expected.y));
    EXPECT(near(fwd.z, expected.z));
    EXPECT(near(glm::length(fwd), 1.0f));
}

// GroundFocus: a downward ray hits y=0 at the expected point; an upward/level ray uses the fallback.
static void T09_ground_focus()
{
    // Eye at (2, 10, 2) looking straight down -> hits ground directly below at (2,0,2).
    const glm::vec3 hit = GroundFocus(glm::vec3(2, 10, 2), glm::vec3(0, -1, 0), 99.0f);
    EXPECT(near(hit.x, 2.0f) && near(hit.y, 0.0f) && near(hit.z, 2.0f));
    // Upward ray never hits the ground -> fallback eye + forward*fallbackDist = (0,5,0)+(0,1,0)*7.
    const glm::vec3 up = GroundFocus(glm::vec3(0, 5, 0), glm::vec3(0, 1, 0), 7.0f);
    EXPECT(near(up.x, 0.0f) && near(up.y, 12.0f) && near(up.z, 0.0f)); // 5 + 1*7
}
```

Register them in `main()` after the existing `T07_...();` call:
```cpp
    T08_camera_forward();
    T09_ground_focus();
```

- [ ] **Step 2: Run to verify failure.**
```
cmake --build --preset msvc-win64-vs2026-community --target test_shadowmath
```
Expected: FAIL to compile — `CameraForward`/`GroundFocus` undefined.

- [ ] **Step 3: Implement the helpers.** In `src/engine/src/rendering/ShadowMath.h`, after `ComputeLightViewProj` (or near the other helpers), add:

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

- [ ] **Step 4: Run to verify pass.**
```
cmake --build --preset msvc-win64-vs2026-community --target test_shadowmath
./out/build/msvc-win64-vs2026-community/bin/Debug/test_shadowmath.exe
```
Expected: `All shadow-math tests passed.` (now 10 cases).

- [ ] **Step 5: Commit.**
```
git add src/engine/src/rendering/ShadowMath.h tests/test_shadowmath.cpp
git commit -m "feat(render): CameraForward + GroundFocus shadow-fit helpers (+tests)"
```

---

## Task 2: Swap ShadowDepthPass to the ground-focus box + relabel slider

**Files:**
- Modify: `src/engine/src/rendering/passes/ShadowDepthPass.cpp`
- Modify: `src/editor/src/panels/RenderStatsPanel.cpp`

- [ ] **Step 1: Read the current camera-fit block.** In `ShadowDepthPass::Render`, the current block (from the previous task on this branch) reads roughly:
```cpp
    const ShadowSettings& shadow = GetShadowSettings();
    glm::vec3 center(0.0f);
    float radius = 0.0f;
    bool haveFit = false;
    {
        const CameraView& cam = m_Renderer->GetActiveCamera();
        const float shadowDist = glm::max(shadow.ShadowDistance, 1.0f);
        const ShadowSphere sph = FrustumSliceSphere(cam.View, cam.Projection, shadowDist);
        if (sph.radius > 1e-3f) {
            center = SnapToTexelGrid(sph.center, sph.radius, sunDir, /*shadowMapSize=*/2048);
            radius = sph.radius;
            haveFit = true;
        }
    }
    if (!haveFit) { /* AABB fallback ... */ }
    const glm::mat4 lightVP = ComputeLightViewProj(center, radius, sunDir, shadow.NearExtend);
```

- [ ] **Step 2: Replace the camera-fit inner block** (the `{ const CameraView& cam ... haveFit = true; } }` braces) with the ground-focus version. Keep `const ShadowSettings& shadow = GetShadowSettings();`, the `center`/`radius`/`haveFit` declarations, the `if (!haveFit)` AABB fallback, and the final `ComputeLightViewProj(...)` line exactly as they are:
```cpp
    {
        const CameraView& cam = m_Renderer->GetActiveCamera();
        const glm::vec3 fwd  = CameraForward(cam.View);
        const float coverage = glm::max(shadow.ShadowDistance, 1.0f); // box width (world units)
        if (glm::dot(fwd, fwd) > 0.5f) {                              // valid camera basis
            const glm::vec3 focus = GroundFocus(cam.Position, fwd, coverage);
            radius = coverage * 0.5f;                                 // ortho half-extent
            center = SnapToTexelGrid(focus, radius, sunDir, /*shadowMapSize=*/2048);
            haveFit = true;
        }
    }
```
(`FrustumSliceSphere`/`ShadowSphere` are no longer used by this file but stay defined in `ShadowMath.h` — leave them. `CameraForward`/`GroundFocus` come from the same header, already included.)

- [ ] **Step 3: Build Engine + editor.**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target Engine
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: clean (pre-existing NavMeshSystem warnings OK).

- [ ] **Step 4: Relabel the slider.** In `src/editor/src/panels/RenderStatsPanel.cpp`, the current line:
```cpp
    changed |= ImGui::SliderFloat("Shadow distance", &sh.ShadowDistance, 10.0f, 500.0f, "%.0f");
```
becomes (label = coverage; range tightened for box width):
```cpp
    changed |= ImGui::SliderFloat("Shadow coverage", &sh.ShadowDistance, 5.0f, 200.0f, "%.0f");
```
Leave the "Shadow near-extend" slider as-is. Rebuild editor:
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: clean.

- [ ] **Step 5: Commit.**
```
git add src/engine/src/rendering/passes/ShadowDepthPass.cpp src/editor/src/panels/RenderStatsPanel.cpp
git commit -m "feat(render): ground-focus shadow box fit (camera-distance-independent)"
```

- [ ] **Step 6: Manual verification (human-run — the decision gate).**
Launch the editor, enter **play mode** (isometric camera):
1. At "Shadow coverage" ~30, shadows on the play area are sharp WITHOUT cranking (the prior frustum-fit needed cranking and still looked bad).
2. Lower coverage → sharper but smaller covered area; raise → softer but more area. Find a good value.
3. Move the player/camera → shadows stay put on the ground (box follows the gaze point), no edge crawl.
4. Compare editor free-cam too — still reasonable (box centers on the looked-at ground point).
5. Decide: good enough → proceed to Task 3 (rename) then merge; still bad → STOP, report back (branch may be scrapped / rethink toward PCF or CSM).

---

## Task 3 (GATED — only if Task 2 manual verification is GOOD and we're merging): rename ShadowDistance → ShadowCoverage

Do NOT run this task unless the human confirmed the ground-focus result is good and wants to merge. This is the blocking pre-merge rename from the spec. Skip entirely if the branch is scrapped.

**Files & edits (mechanical rename, keep semantics):**
- `src/engine/src/rendering/RenderStats.h`: `float ShadowDistance` → `float ShadowCoverage` (keep default).
- `src/common/include/ApplicationContext.h`: `float shadowDistance` → `float shadowCoverage`.
- `src/engine/src/utilities/SettingsManager.cpp`: JSON key `"distance"` → `"coverage"` in BOTH the load (`js.contains("coverage") ... out->shadowCoverage = ...`) and save (`j["renderer"]["shadow"]["coverage"] = settings.shadowCoverage;`).
- `src/engine/src/core/Application.cpp`: seed `sh.ShadowCoverage = m_AppContext->Settings.shadowCoverage;`.
- `src/editor/src/app/ImGuiRenderer.cpp`: the shadow change-guard `sh.ShadowCoverage` ↔ `Settings.shadowCoverage` (both the compare and the assign).
- `src/editor/src/panels/RenderStatsPanel.cpp`: slider binds `&sh.ShadowCoverage`.
- `src/engine/src/rendering/passes/ShadowDepthPass.cpp`: `shadow.ShadowDistance` → `shadow.ShadowCoverage`.

- [ ] **Step 1:** Grep to find every reference: `grep -rn "ShadowDistance\|shadowDistance" src` — update each to the new name. Confirm none remain after.
- [ ] **Step 2: Build + test.**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target ecs
cmake --build --preset msvc-win64-vs2026-community --target Engine
cmake --build --preset msvc-win64-vs2026-community --target editor
cmake --build --preset msvc-win64-vs2026-community --target test_shadowmath
./out/build/msvc-win64-vs2026-community/bin/Debug/test_shadowmath.exe
```
Expected: all clean; tests pass. (Old `engine_settings.json` with `renderer.shadow.distance` will fall back to the default coverage on load — acceptable; the user re-tunes once.)
- [ ] **Step 3: Commit.**
```
git add -A
git commit -m "refactor(render): rename ShadowDistance -> ShadowCoverage (box-coverage semantics)"
```

---

## Self-Review Notes (spec coverage)

- Spec §1 CameraForward/GroundFocus helpers → Task 1 (TDD).
- Spec §2 fit swap (ground-focus + coverage*0.5 radius + snap + near-extend) → Task 2.
- Spec §3 slider relabel "Shadow coverage" → Task 2 Step 4.
- Spec §4 PRE-MERGE GATE rename → Task 3 (gated on the merge decision).
- Testing: pure unit cases (Task 1) + build + the manual decision gate (Task 2 Step 6).

## Notes

- `FrustumSliceSphere` + its tests stay (unused by the pass now) — harmless, reusable for CSM later.
- The shadow-map size `2048` is still a literal (matches the existing PCF `1/2048` in the lighting shader); not changed here.
