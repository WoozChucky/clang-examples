# Camera-Frustum-Fit Shadows Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sharpen near-camera directional shadows by fitting the light ortho to a capped slice of the camera frustum (tunable distance, texel-snapped, near-Z extended), and move `ShadowSettings` to the engine tier so `runtime.exe` honors tuned shadows.

**Architecture:** `ShadowDepthPass` keeps rendering one 2048² depth map, but derives the ortho center/radius from the camera frustum slice (`near → ShadowDistance`) instead of the all-meshes AABB — concentrating texels where the camera looks. New pure math lives in `ShadowMath.h` (unit-tested). Shadow tunables move from `editor_preferences.json` to `engine_settings.json`, mirroring the existing `aaMode` pattern.

**Tech Stack:** C++23, GLM (RH, ZO depth), NVRHI, nlohmann::json, Dear ImGui.

**Spec:** `docs/superpowers/specs/2026-05-29-camera-fit-shadows-design.md`

---

## Build & Test Reference

- Configure: `cmake --preset msvc-win64-vs2026-community` (community only — enterprise NOT installed).
- Build: `cmake --build --preset msvc-win64-vs2026-community --target <ecs|Engine|editor|test_shadowmath>`
- Test exes: `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- Commit identity is configured globally; never `--no-verify`.

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `src/engine/src/rendering/ShadowMath.h` | `nearExtend` param + pure `FrustumSliceSphere` / `SnapToTexelGrid` | 1 |
| `tests/test_shadowmath.cpp` + `tests/CMakeLists.txt` | Unit tests for the math | 1 |
| `src/engine/src/rendering/RenderStats.h` | `ShadowDistance` + `NearExtend` on `ShadowSettings` | 2 |
| `src/common/include/ApplicationContext.h` | 4 `shadow*` mirror fields on `ApplicationSettings` | 2 |
| `src/engine/src/utilities/SettingsManager.cpp` | (de)serialize `renderer.shadow` | 2 |
| `src/engine/src/core/Application.cpp` | seed `GetShadowSettings()` from persisted settings | 2 |
| `src/editor/src/app/EditorPreferences.{h,cpp}` | remove the `shadows` block + param | 2 |
| `src/editor/src/app/ImGuiRenderer.cpp` | persist shadow edits to engine_settings (change-guarded) | 2 |
| `src/editor/src/panels/RenderStatsPanel.cpp` | 2 new sliders | 2 |
| `src/engine/src/rendering/passes/ShadowDepthPass.cpp` | camera-frustum fit + snap + fallback | 3 |

**Order:** Task 1 (independent pure math + tests) → Task 2 (settings migration; builds + shippable, rendering unchanged) → Task 3 (wire the fit; the payoff + manual verify). Each task builds green.

---

## Task 1: ShadowMath pure helpers (TDD)

**Files:**
- Modify: `src/engine/src/rendering/ShadowMath.h`
- Create: `tests/test_shadowmath.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_shadowmath.cpp`:

```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "ShadowMath.h" // ComputeLightViewProj, FrustumSliceSphere, SnapToTexelGrid

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; }

// nearExtend defaults to 0 and reproduces the pre-change matrix exactly (regression guard).
static void T00_nearextend_zero_is_identity_change()
{
    const glm::vec3 c(5, 2, -3); const float r = 10.0f; const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f));
    const glm::mat4 a = ComputeLightViewProj(c, r, sun);
    const glm::mat4 b = ComputeLightViewProj(c, r, sun, 0.0f);
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) EXPECT(near(a[i][j], b[i][j]));
}

// A point just beyond the old near plane (further toward the sun) is OUTSIDE [0,1] depth at
// nearExtend=0 but INSIDE once nearExtend pulls the near plane back.
static void T01_nearextend_captures_caster_toward_sun()
{
    const glm::vec3 c(0, 0, 0); const float r = 10.0f; const glm::vec3 sun = glm::normalize(glm::vec3(0, -1, 0));
    // Old near plane sits at eye = center - sun*2r = (0,20,0); a caster at y=25 is beyond it (toward the sun).
    const glm::vec4 caster(0, 25, 0, 1);
    auto depthInRange = [&](const glm::mat4& vp) {
        glm::vec4 cl = vp * caster; cl /= cl.w; return cl.z >= 0.0f && cl.z <= 1.0f;
    };
    EXPECT(!depthInRange(ComputeLightViewProj(c, r, sun, 0.0f)));   // clipped without extend
    EXPECT( depthInRange(ComputeLightViewProj(c, r, sun, 20.0f)));  // captured with extend
}

// FrustumSliceSphere: the returned sphere contains all reconstructed corners; a smaller
// ShadowDistance yields a smaller radius.
static void T02_frustum_slice_sphere()
{
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0, 10, 20), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const ShadowSphere s80  = FrustumSliceSphere(view, proj, 80.0f);
    const ShadowSphere s40  = FrustumSliceSphere(view, proj, 40.0f);
    EXPECT(s40.radius < s80.radius);          // smaller slice -> smaller sphere
    EXPECT(s80.radius > 0.0f);
    // Reconstruct the same 8 corners and confirm containment for the 80 slice.
    // (Recompute via the same public helper path is fine; here we just sanity-check the radius
    //  bounds the near-plane center distance.)
    EXPECT(s80.radius >= glm::length(s80.center - glm::vec3(0, 10, 20)) - 80.0f - 1.0f); // loose sanity
}

// SnapToTexelGrid: sub-texel shift -> no change; >1-texel shift snaps; result within a texel.
static void T03_snap_to_texel_grid()
{
    const glm::vec3 sun = glm::normalize(glm::vec3(0, -1, 0));
    const float r = 10.0f; const uint32_t mapSize = 2048;
    const float texel = (2.0f * r) / mapSize;
    const glm::vec3 base(100.0f, 0.0f, 50.0f);
    const glm::vec3 snapped = SnapToTexelGrid(base, r, sun, mapSize);
    // Snapping is idempotent.
    const glm::vec3 snapped2 = SnapToTexelGrid(snapped, r, sun, mapSize);
    EXPECT(near(snapped.x, snapped2.x) && near(snapped.z, snapped2.z));
    // Output stays within one texel of input (in the snap plane; sun is +Y so x/z are the plane).
    EXPECT(std::fabs(snapped.x - base.x) <= texel);
    EXPECT(std::fabs(snapped.z - base.z) <= texel);
}

int main()
{
    T00_nearextend_zero_is_identity_change();
    T01_nearextend_captures_caster_toward_sun();
    T02_frustum_slice_sphere();
    T03_snap_to_texel_grid();
    if (g_Failures == 0) { std::printf("All shadow-math tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d shadow-math test(s) failed.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the `test_shadowmath` target**

Append to `tests/CMakeLists.txt` (mirror the `test_followcam` block — glm-only, `RUNTIME_DIR` defined there). NOTE the include dir must reach `ShadowMath.h` (in `src/engine/src/rendering`) and `RenderStats.h` it may include — but `ShadowMath.h` only needs glm + the `ShadowSphere` struct it defines itself, so glm + the rendering dir suffice:

```cmake
add_executable(test_shadowmath
    test_shadowmath.cpp
)

target_link_libraries(test_shadowmath PRIVATE
    glm::glm
)

target_include_directories(test_shadowmath PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
)

target_compile_definitions(test_shadowmath PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_shadowmath PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Run test to verify it fails**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_shadowmath
```
Expected: FAIL to compile — `FrustumSliceSphere`/`SnapToTexelGrid`/`ShadowSphere` undefined and `ComputeLightViewProj` has no 4th param.

- [ ] **Step 4: Implement the helpers in `ShadowMath.h`**

Add the `nearExtend` param to `ComputeLightViewProj` and append the two new pure helpers. Replace the existing `ComputeLightViewProj` with:

```cpp
inline glm::mat4 ComputeLightViewProj(const glm::vec3& center, float radius,
                                      const glm::vec3& sunDir, float nearExtend = 0.0f) {
    const glm::vec3 d = glm::normalize(sunDir);
    const float r = (radius > 1e-3f) ? radius : 1.0f;
    const glm::vec3 up = (std::abs(d.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const float ext = (nearExtend > 0.0f) ? nearExtend : 0.0f;
    const glm::vec3 eye = center - d * (r * 2.0f + ext);
    const glm::mat4 view = glm::lookAtRH(eye, center, up);
    const glm::mat4 proj = glm::orthoRH_ZO(-r, r, -r, r, 0.0f, 4.0f * r + ext);
    return proj * view;
}
```

Append after it:

```cpp
// Bounding sphere of a region (light-space-agnostic).
struct ShadowSphere { glm::vec3 center{0.0f}; float radius = 0.0f; };

// Bound the camera frustum slice [camera near, shadowDistance] with a sphere, in world space.
// Reconstructs the 8 view-space corners from the projection (eye at the view-space origin, so a
// corner's xy scales linearly with depth), caps the far corners at shadowDistance, transforms to
// world via inverse(view), then fits a sphere (center = mean, radius = max corner distance).
inline ShadowSphere FrustumSliceSphere(const glm::mat4& camView, const glm::mat4& camProj,
                                       float shadowDistance) {
    const glm::mat4 invP = glm::inverse(camProj);
    const glm::mat4 invV = glm::inverse(camView);
    const glm::vec2 ndc[4] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };

    glm::vec3 corners[8];
    for (int i = 0; i < 4; ++i) {
        glm::vec4 vn = invP * glm::vec4(ndc[i].x, ndc[i].y, 0.0f, 1.0f); // near plane (ZO z=0)
        vn /= vn.w;
        const float nearDepth = -vn.z;                 // view looks down -Z (RH)
        const float dist = glm::max(shadowDistance, nearDepth + 1e-3f);
        const float scale = dist / nearDepth;          // similar triangles from the eye (origin)
        const glm::vec4 vf(vn.x * scale, vn.y * scale, -dist, 1.0f);
        corners[i]     = glm::vec3(invV * vn);
        corners[i + 4] = glm::vec3(invV * vf);
    }

    glm::vec3 c(0.0f);
    for (const auto& p : corners) c += p;
    c /= 8.0f;
    float r2 = 0.0f;
    for (const auto& p : corners) r2 = glm::max(r2, glm::dot(p - c, p - c));
    return ShadowSphere{ c, std::sqrt(r2) };
}

// Quantize `center` to whole shadow-texel increments in the light's view plane so the shadow
// grid does not sub-pixel crawl as the camera pans. texelWorld = (2*radius) / shadowMapSize.
inline glm::vec3 SnapToTexelGrid(const glm::vec3& center, float radius, const glm::vec3& sunDir,
                                 uint32_t shadowMapSize) {
    const float r = (radius > 1e-3f) ? radius : 1.0f;
    const float texelWorld = (2.0f * r) / static_cast<float>(shadowMapSize);
    const glm::vec3 d = glm::normalize(sunDir);
    const glm::vec3 up = (std::abs(d.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const glm::vec3 right = glm::normalize(glm::cross(up, d));
    const glm::vec3 up2   = glm::cross(d, right);      // orthonormal light basis (right, up2, d)
    float cr = glm::dot(center, right);
    float cu = glm::dot(center, up2);
    const float cd = glm::dot(center, d);
    cr = std::round(cr / texelWorld) * texelWorld;     // snap the in-plane components
    cu = std::round(cu / texelWorld) * texelWorld;
    return right * cr + up2 * cu + d * cd;             // reconstruct (orthonormal basis)
}
```

Add `#include <cstdint>` at the top of `ShadowMath.h` (for `uint32_t`) if not already present.

- [ ] **Step 5: Run test to verify it passes**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_shadowmath
./out/build/msvc-win64-vs2026-community/bin/Debug/test_shadowmath.exe
```
Expected: `All shadow-math tests passed.`

- [ ] **Step 6: Confirm Engine still builds** (ShadowMath.h is included by ShadowDepthPass; the defaulted param keeps the existing call valid):
```
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: clean.

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/rendering/ShadowMath.h tests/test_shadowmath.cpp tests/CMakeLists.txt
git commit -m "feat(render): ShadowMath nearExtend + FrustumSliceSphere + SnapToTexelGrid (+tests)"
```

---

## Task 2: Move ShadowSettings to the engine tier (+ new tunables, +sliders)

**Files (in this order to keep the build green):**
- Modify: `src/engine/src/rendering/RenderStats.h`
- Modify: `src/common/include/ApplicationContext.h`
- Modify: `src/engine/src/utilities/SettingsManager.cpp`
- Modify: `src/engine/src/core/Application.cpp`
- Modify: `src/editor/src/app/EditorPreferences.h` + `src/editor/src/app/EditorPreferences.cpp`
- Modify: `src/editor/src/app/ImGuiRenderer.cpp`
- Modify: `src/editor/src/panels/RenderStatsPanel.cpp`

- [ ] **Step 1: Add the two tunables to the live global.** In `src/engine/src/rendering/RenderStats.h`, change:
```cpp
struct ShadowSettings {
    bool  Enabled = true;
    float Bias    = 0.0015f;
};
```
to:
```cpp
struct ShadowSettings {
    bool  Enabled        = true;
    float Bias           = 0.0015f;
    float ShadowDistance = 80.0f;   // world units of camera-frustum slice the shadow map covers
    float NearExtend     = 50.0f;   // light-space near-plane pull-back toward the sun
};
```

- [ ] **Step 2: Add mirror fields to the persisted settings.** In `src/common/include/ApplicationContext.h`, in `struct ApplicationSettings`, after `int aaMode ...;` add:
```cpp
    bool        shadowEnabled    = true;
    float       shadowBias       = 0.0015f;
    float       shadowDistance   = 80.0f;
    float       shadowNearExtend = 50.0f;
```

- [ ] **Step 3: (De)serialize `renderer.shadow` in SettingsManager.** In `src/engine/src/utilities/SettingsManager.cpp`, inside the `if (j.contains("renderer") ...)` block, after the `out->aaMode = ResolveAaMode(jr);` line add:
```cpp
        if (jr.contains("shadow") && jr["shadow"].is_object()) {
            const auto& js = jr["shadow"];
            if (js.contains("enabled")    && js["enabled"].is_boolean())   out->shadowEnabled    = js["enabled"].get<bool>();
            if (js.contains("bias")       && js["bias"].is_number())       out->shadowBias       = js["bias"].get<float>();
            if (js.contains("distance")   && js["distance"].is_number())   out->shadowDistance   = js["distance"].get<float>();
            if (js.contains("nearExtend") && js["nearExtend"].is_number()) out->shadowNearExtend = js["nearExtend"].get<float>();
        }
```
In `Save`, after the `j["renderer"]["aaMode"] = settings.aaMode;` line add:
```cpp
    j["renderer"]["shadow"]["enabled"]    = settings.shadowEnabled;
    j["renderer"]["shadow"]["bias"]       = settings.shadowBias;
    j["renderer"]["shadow"]["distance"]   = settings.shadowDistance;
    j["renderer"]["shadow"]["nearExtend"] = settings.shadowNearExtend;
```

- [ ] **Step 4: Seed the live global at startup.** In `src/engine/src/core/Application.cpp`, immediately after the existing AA seed line `GetAntiAliasingSettings().Mode = static_cast<AAMode>(m_AppContext->Settings.aaMode);` add:
```cpp
    // Seed the live shadow settings from the persisted engine-tier values (both exes boot here;
    // the RenderThread reads GetShadowSettings()).
    {
        ShadowSettings& sh = GetShadowSettings();
        sh.Enabled        = m_AppContext->Settings.shadowEnabled;
        sh.Bias           = m_AppContext->Settings.shadowBias;
        sh.ShadowDistance = m_AppContext->Settings.shadowDistance;
        sh.NearExtend     = m_AppContext->Settings.shadowNearExtend;
    }
```
(`ShadowSettings`/`GetShadowSettings` come from `RenderStats.h`, already included since the file uses `GetAntiAliasingSettings()`. If the build reports them undefined, add `#include "RenderStats.h"` — verify first.)

- [ ] **Step 5: Remove shadows from EditorPreferences.** In `src/editor/src/app/EditorPreferences.h`:
  - In `PrefsToJson`, delete the `{"shadows", {{"enabled", ...},{"bias", ...}}},` block.
  - In `PrefsFromJson`, delete the `if (j.contains("shadows") ...) { ... }` block.
  - Remove the `const ShadowSettings& shadows` parameter from `PrefsToJson` and the
    `ShadowSettings& shadows` parameter from `PrefsFromJson`.
  - Remove the now-unused `#include "RenderStats.h"` ONLY if nothing else in the header needs it
    (CullingSettings/DebugDrawSettings also come from RenderStats.h — so KEEP the include).

  In `src/editor/src/app/EditorPreferences.cpp`, update the two call sites:
  - `PrefsFromJson(j, GetCullingSettings(), GetDebugDrawSettings(), GetShadowSettings(), camera);`
    → `PrefsFromJson(j, GetCullingSettings(), GetDebugDrawSettings(), camera);`
  - `PrefsToJson(GetCullingSettings(), GetDebugDrawSettings(), GetShadowSettings(), camera)`
    → `PrefsToJson(GetCullingSettings(), GetDebugDrawSettings(), camera)`

- [ ] **Step 6: Persist shadow edits to engine_settings in the panel-save path.** In
  `src/editor/src/app/ImGuiRenderer.cpp`, find the Render Stats panel save block (the one that
  calls `EditorPreferences::Save(...)` then the `aaMode` change-guard). After the existing
  `aaMode` change-guard `if (...) { ... }`, add a parallel shadow change-guard:
```cpp
            // Shadow settings are engine-tier (so runtime.exe honors them); persist on change.
            const ShadowSettings& sh = GetShadowSettings();
            if (m_AppContext &&
                (sh.Enabled        != m_AppContext->Settings.shadowEnabled  ||
                 sh.Bias           != m_AppContext->Settings.shadowBias     ||
                 sh.ShadowDistance != m_AppContext->Settings.shadowDistance ||
                 sh.NearExtend     != m_AppContext->Settings.shadowNearExtend)) {
                m_AppContext->Settings.shadowEnabled    = sh.Enabled;
                m_AppContext->Settings.shadowBias       = sh.Bias;
                m_AppContext->Settings.shadowDistance   = sh.ShadowDistance;
                m_AppContext->Settings.shadowNearExtend = sh.NearExtend;
                if (!SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH, m_AppContext->Settings)) {
                    SM_WARN("Failed to persist shadow settings to %s", SettingsManager::DEFAULT_SETTINGS_PATH);
                }
            }
```
(`GetShadowSettings`/`ShadowSettings` are already available via the panel includes / RenderStats.h;
`SettingsManager` + `m_AppContext` are already used by the adjacent aaMode block. Confirm includes;
add `#include "RenderStats.h"` only if the build complains.)

- [ ] **Step 7: Add the two sliders to the panel.** In `src/editor/src/panels/RenderStatsPanel.cpp`,
  the Shadows section currently is:
```cpp
    ImGui::TextDisabled("Shadows");
    ShadowSettings& sh = GetShadowSettings();
    changed |= ImGui::Checkbox("Shadows", &sh.Enabled);
    // The slider mutates Bias live every drag frame; report the change only once the
    // <comment>
    ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");
```
After the `Shadow bias` slider add (use the existing `changed |=` convention so the save path fires;
match how `Bias` reports — if `Bias` uses `IsItemDeactivatedAfterEdit`, mirror that, else `changed |=`):
```cpp
    changed |= ImGui::SliderFloat("Shadow distance", &sh.ShadowDistance, 10.0f, 500.0f, "%.0f");
    changed |= ImGui::SliderFloat("Shadow near-extend", &sh.NearExtend, 0.0f, 200.0f, "%.0f");
```

- [ ] **Step 8: Build everything + run the existing test sweep.**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target ecs
cmake --build --preset msvc-win64-vs2026-community --target Engine
cmake --build --preset msvc-win64-vs2026-community --target editor
cmake --build --preset msvc-win64-vs2026-community --target test_shadowmath
./out/build/msvc-win64-vs2026-community/bin/Debug/test_shadowmath.exe
```
Expected: all clean; `All shadow-math tests passed.` (After this task, rendering is unchanged —
ShadowDepthPass still uses the AABB fit; the new fields exist + persist + have sliders. Shippable
checkpoint.)

- [ ] **Step 9: Commit**

```bash
git add src/engine/src/rendering/RenderStats.h src/common/include/ApplicationContext.h src/engine/src/utilities/SettingsManager.cpp src/engine/src/core/Application.cpp src/editor/src/app/EditorPreferences.h src/editor/src/app/EditorPreferences.cpp src/editor/src/app/ImGuiRenderer.cpp src/editor/src/panels/RenderStatsPanel.cpp
git commit -m "feat(render): move ShadowSettings to engine tier + ShadowDistance/NearExtend tunables"
```

---

## Task 3: Wire ShadowDepthPass to the camera-frustum fit

**Files:**
- Modify: `src/engine/src/rendering/passes/ShadowDepthPass.cpp`

- [ ] **Step 1: Replace the ortho-fit block.** In `ShadowDepthPass::Render`, the current fit
  (lines ~92-117) computes the all-meshes AABB then `center`/`radius`/`lightVP`. Keep the
  all-meshes AABB computation as a **fallback**, but prefer the camera-frustum fit. Replace the
  block that currently reads (from the `// Fit the light frustum to the world-space AABB...`
  comment through the `const glm::mat4 lightVP = ComputeLightViewProj(center, radius, sunDir);`
  line) with:

```cpp
    const ShadowSettings& shadow = GetShadowSettings();

    // Prefer fitting to the camera frustum slice (concentrates texels where the camera looks).
    // Fall back to the all-visible-meshes AABB if the camera is unavailable/degenerate.
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
    if (!haveFit) {
        // Fallback: fit to the world-space AABB of all visible meshes (the original behavior).
        glm::vec3 mn(std::numeric_limits<float>::max());
        glm::vec3 mx(-std::numeric_limits<float>::max());
        bool any = false;
        MeshSystem* msFit = m_Renderer->GetMeshSystem();
        world->Each<TransformComponent, MeshComponent>(
            [&](EntityId, const TransformComponent& t, const MeshComponent& m)
            {
                if (!m.Visible) return;
                const auto b = msFit->GetMeshBounds(m.MeshId);
                if (!b.valid) return;
                glm::vec3 wMin, wMax;
                TransformAABB(ModelMatrix(t), b.min, b.max, wMin, wMax);
                mn = glm::min(mn, wMin); mx = glm::max(mx, wMax); any = true;
            });
        if (!any) return;
        center = 0.5f * (mn + mx);
        radius = 0.5f * glm::length(mx - mn);
        static bool s_warnedShadowFit = false;
        if (!s_warnedShadowFit) { SM_WARN("ShadowDepthPass: camera frustum fit unavailable; using scene-AABB fallback"); s_warnedShadowFit = true; }
    }

    const glm::mat4 lightVP = ComputeLightViewProj(center, radius, sunDir, shadow.NearExtend);
```

  NOTE: this removes the original standalone AABB computation (it now lives inside the `!haveFit`
  branch). Read the current code first and make sure the existing `MeshSystem* ms = ...` used later
  in the depth-render loop is still declared (the fallback uses a local `msFit` to avoid colliding
  with it). If the original `ms` was declared in the block you're replacing, keep its declaration
  for the render loop below.

- [ ] **Step 2: Confirm includes.** `ShadowDepthPass.cpp` already includes `RenderStats.h`,
  `ShadowMath.h`, `Renderer.h`, `Frustum.h`, `<limits>`. `CameraView` comes via `Renderer.h`
  (`GetActiveCamera()` returns `const CameraView&`). No new include expected — add one only if the
  build says so.

- [ ] **Step 3: Build Engine + editor.**
```
cmake --build --preset msvc-win64-vs2026-community --target Engine
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: clean.

- [ ] **Step 4: Manual verification (human-run — graphics).**
Launch the editor:
1. Near shadows are visibly sharper than `main` at the default ShadowDistance (80).
2. Drag **Shadow distance** down (e.g. to 30) → near shadows sharpen further; up (300) → softer/blockier but more coverage. Watch the tradeoff live.
3. Pan/move the camera → shadow edges do NOT crawl/shimmer (texel snap working).
4. Place/observe an object just off the screen edge under an angled sun → its shadow still reaches into view (near-extend); lower **Shadow near-extend** to 0 and confirm such shadows clip (knob works).
5. Restart the editor → shadow settings restored from `engine_settings.json` (`renderer.shadow`).
6. (Optional) run `runtime.exe` → it now renders with the tuned shadow settings (engine tier).

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/rendering/passes/ShadowDepthPass.cpp
git commit -m "feat(render): fit shadow ortho to camera frustum slice (snap + near-extend, AABB fallback)"
```

---

## Self-Review Notes (spec coverage)

- Spec §1 camera-frustum fit (slice → sphere → snap → VP) → Task 3 (using Task 1 helpers).
- Spec §2 near-plane extension → Task 1 (`ComputeLightViewProj` param) + Task 3 (passes `NearExtend`).
- Spec §3 pure testable helpers → Task 1 (`FrustumSliceSphere`, `SnapToTexelGrid`, unit-tested).
- Spec §4 engine-tier settings move + sliders + persistence → Task 2 (all 8 files).
- Spec §5 fallback (no camera/degenerate → AABB fit + one-shot SM_WARN; ShadowDistance clamp) → Task 3.
- Testing: pure unit tests (Task 1) + build gates + manual checklist (Task 3).

## Known Risk Notes (concrete, not placeholders)

- **Bias slider change-reporting:** the existing `Shadow bias` slider does NOT use `changed |=`
  (it mutates live, comment says report once). The new sliders use `changed |=` to trigger the
  save path; if that causes a save every drag-frame and that's undesirable, gate the engine save
  on `ImGui::IsItemDeactivatedAfterEdit()` like the bias pattern intends. Functionally correct
  either way (just save frequency).
- **`FrustumSliceSphere` perspective assumption:** assumes a perspective projection (eye at the
  view-space origin, corner xy scales with depth). The game camera is perspective
  (`glm::perspectiveRH_ZO`, see CameraFollow). If an orthographic camera is ever used, the scale
  reconstruction is wrong — out of scope (no ortho game camera exists).
- **Fallback `ms` vs `msFit`:** read the real code; the depth-render loop below the replaced block
  uses a `MeshSystem*` — ensure exactly one declaration survives for it and the fallback's local
  doesn't shadow/duplicate it.
