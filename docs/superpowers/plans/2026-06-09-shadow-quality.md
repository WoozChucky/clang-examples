# Shadow Quality Bundle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sharper, softer-edged, acne-free single-cascade directional shadows for the modest-zoom isometric ARPG camera — frustum-fit coverage + rotated Poisson PCF + normal-offset bias, no new subsystem.

**Architecture:** Three coordinated changes across the existing shadow path. (1) `ShadowDepthPass` sizes the light ortho box from the camera frustum slice (via the already-written `FrustumSliceSphere`) capped at a `ShadowDistance`, publishing the fit radius on `Renderer::ShadowView`. (2) The `LightingRenderPass` pixel shader replaces its single comparison tap with a 16-tap per-pixel-rotated Poisson kernel. (3) The same shader offsets the sample position along the surface normal, retiring the manual depth-bias slider in favor of a normal-offset knob + a tiny hardcoded constant bias.

**Tech Stack:** C++23, NVRHI (DX12), HLSL (`vs_6_1`/`ps_6_1` via DXC), GLM (RH, depth `[0,1]`), nlohmann::json for settings. Build/test preset `msvc-win64-vs2026-community`. Tests are plain assert-style `main()` executables.

**Spec:** `docs/superpowers/specs/2026-06-09-shadow-quality-design.md`

**Task sequencing note:** Tasks are ordered so the build stays green after every task. `ShadowSettings::Bias` (and its slider/setting/JSON key) is kept untouched through Tasks 1–3 and removed end-to-end only in Task 4, once the shader no longer reads it.

**Commit identity (every commit in this plan):** use `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit ...`. Never `--no-verify`. Stage exact paths (never `git add -A`/`.`).

**Build/run commands used throughout:**
- Build editor (also builds Engine): `cmake --build --preset msvc-win64-vs2026-community --target editor`
- Build a test: `cmake --build --preset msvc-win64-vs2026-community --target test_shadowmath`
- Run it: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_shadowmath.exe` (expected final line: `All shadow-math tests passed.`)

---

### Task 1: Strengthen the `FrustumSliceSphere` containment guard

`FrustumSliceSphere` (`src/engine/src/rendering/ShadowMath.h`) already exists and has a *loose* test (`T02_frustum_slice_sphere` in `tests/test_shadowmath.cpp`). Task 3 will wire it into production as the box-fit primitive, so lock its containment property first with a rigorous test. The function is already implemented, so this test should **PASS on first run** — it is a regression guard, not red→green. If it FAILS, that is a real bug in `FrustumSliceSphere` to investigate before proceeding.

**Files:**
- Test: `tests/test_shadowmath.cpp` (add one test function + its call in `main`)

- [ ] **Step 1: Add a rigorous containment test**

In `tests/test_shadowmath.cpp`, add this function immediately after `T02_frustum_slice_sphere` (after line 56):

```cpp
// FrustumSliceSphere containment: the returned sphere must enclose all 8 frustum-slice corners
// (near plane at z=0, far corners capped at ShadowDistance), reconstructed the same way the
// function does. Guards the box-fit primitive ShadowDepthPass relies on.
static void T10_frustum_slice_sphere_contains_corners()
{
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0, 12, 24), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(50.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const float shadowDistance = 60.0f;
    const ShadowSphere s = FrustumSliceSphere(view, proj, shadowDistance);
    EXPECT(s.radius > 0.0f);

    const glm::mat4 invP = glm::inverse(proj);
    const glm::mat4 invV = glm::inverse(view);
    const glm::vec2 ndc[4] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
    const float eps = 1e-2f;
    for (int i = 0; i < 4; ++i) {
        glm::vec4 vn = invP * glm::vec4(ndc[i].x, ndc[i].y, 0.0f, 1.0f);
        vn /= vn.w;
        const float nearDepth = -vn.z;
        const float dist = glm::max(shadowDistance, nearDepth + 1e-3f);
        const float scale = dist / nearDepth;
        const glm::vec4 vf(vn.x * scale, vn.y * scale, -dist, 1.0f);
        const glm::vec3 cNear = glm::vec3(invV * vn);
        const glm::vec3 cFar  = glm::vec3(invV * vf);
        EXPECT(glm::length(cNear - s.center) <= s.radius + eps);
        EXPECT(glm::length(cFar  - s.center) <= s.radius + eps);
    }
}
```

- [ ] **Step 2: Register the test in `main`**

In `tests/test_shadowmath.cpp`, in `main()`, add the call right after the `T02_frustum_slice_sphere();` line (after line 139):

```cpp
    T10_frustum_slice_sphere_contains_corners();
```

- [ ] **Step 3: Build the test**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_shadowmath`
Expected: builds with no errors.

- [ ] **Step 4: Run the test**

Run: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_shadowmath.exe`
Expected: `All shadow-math tests passed.` (exit 0). The new T10 passes because `FrustumSliceSphere` is already correct.

- [ ] **Step 5: Commit**

```bash
git add tests/test_shadowmath.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "test(shadow): rigorous FrustumSliceSphere corner-containment guard"
```

---

### Task 2: Settings data model — rename coverage→distance, add normal-offset + PCF radius knobs

Rename `ShadowCoverage`→`ShadowDistance` everywhere, and add two new tunables (`NormalOffset`, `PcfRadius`) through the full settings trio + editor panel. The new fields are persisted and editable now but not yet consumed by the shader (Task 4) or the fit (Task 3) — that is fine, they sit live. **Leave `Bias`/`shadowBias` untouched in this task** (the lighting shader still reads it, so removing it now would break the build). `NearExtend` is unchanged.

Defaults: `ShadowDistance = 60.0f` (far cap, world units — modest-zoom iso sees roughly this far; replaces the old 80 box-*width*), `NormalOffset = 1.0f` (texels), `PcfRadius = 1.5f` (texels).

**Files:**
- Modify: `src/common/include/ApplicationContext.h:25-28`
- Modify: `src/engine/src/rendering/RenderStats.h:31-36`
- Modify: `src/engine/src/utilities/SettingsManager.cpp:75-81` (load) and `:110-113` (save)
- Modify: `src/engine/src/core/Application.cpp:27-31` (seed)
- Modify: `src/editor/src/panels/RenderStatsPanel.cpp:47-54` (sliders)
- Modify: `src/engine/src/rendering/passes/ShadowDepthPass.cpp:105` (rename the read; still uses GroundFocus this task)

- [ ] **Step 1: Update the persisted settings struct**

In `src/common/include/ApplicationContext.h`, replace lines 26-28:

```cpp
    float       shadowBias       = 0.0015f;
    float       shadowCoverage   = 80.0f;
    float       shadowNearExtend = 50.0f;
```

with (note: `shadowBias` is intentionally KEPT here this task):

```cpp
    float       shadowBias        = 0.0015f;
    float       shadowDistance    = 60.0f;   // frustum-fit far cap (world units); replaces shadowCoverage
    float       shadowNearExtend  = 50.0f;
    float       shadowNormalOffset = 1.0f;   // normal-offset bias (shadow texels)
    float       shadowPcfRadius    = 1.5f;   // Poisson PCF penumbra radius (shadow texels)
```

- [ ] **Step 2: Update the live `ShadowSettings` struct**

In `src/engine/src/rendering/RenderStats.h`, replace the struct at lines 31-36:

```cpp
struct ShadowSettings {
    bool  Enabled        = true;
    float Bias           = 0.0015f;
    float ShadowCoverage = 80.0f;   // world-space width of the ground-focus shadow box
    float NearExtend     = 50.0f;   // light-space near-plane pull-back toward the sun
};
```

with:

```cpp
struct ShadowSettings {
    bool  Enabled        = true;
    float Bias           = 0.0015f; // (removed in Task 4 once the shader uses normal-offset)
    float ShadowDistance = 60.0f;   // frustum-fit far cap (world units); smaller = sharper + shorter range
    float NearExtend     = 50.0f;   // light-space near-plane pull-back toward the sun
    float NormalOffset   = 1.0f;    // normal-offset bias (shadow texels)
    float PcfRadius      = 1.5f;    // Poisson PCF penumbra radius (shadow texels)
};
```

- [ ] **Step 3: Update settings load**

In `src/engine/src/utilities/SettingsManager.cpp`, replace the shadow load block (lines 77-80):

```cpp
            if (js.contains("enabled")    && js["enabled"].is_boolean())   out->shadowEnabled    = js["enabled"].get<bool>();
            if (js.contains("bias")       && js["bias"].is_number())       out->shadowBias       = js["bias"].get<float>();
            if (js.contains("coverage")   && js["coverage"].is_number())   out->shadowCoverage   = js["coverage"].get<float>();
            if (js.contains("nearExtend") && js["nearExtend"].is_number()) out->shadowNearExtend = js["nearExtend"].get<float>();
```

with:

```cpp
            if (js.contains("enabled")     && js["enabled"].is_boolean())    out->shadowEnabled      = js["enabled"].get<bool>();
            if (js.contains("bias")        && js["bias"].is_number())        out->shadowBias         = js["bias"].get<float>();
            if (js.contains("distance")    && js["distance"].is_number())    out->shadowDistance     = js["distance"].get<float>();
            if (js.contains("nearExtend")  && js["nearExtend"].is_number())  out->shadowNearExtend   = js["nearExtend"].get<float>();
            if (js.contains("normalOffset")&& js["normalOffset"].is_number())out->shadowNormalOffset = js["normalOffset"].get<float>();
            if (js.contains("pcfRadius")   && js["pcfRadius"].is_number())   out->shadowPcfRadius    = js["pcfRadius"].get<float>();
```

- [ ] **Step 4: Update settings save**

In `src/engine/src/utilities/SettingsManager.cpp`, replace the shadow save block (lines 110-113):

```cpp
    j["renderer"]["shadow"]["enabled"]    = settings.shadowEnabled;
    j["renderer"]["shadow"]["bias"]       = settings.shadowBias;
    j["renderer"]["shadow"]["coverage"]   = settings.shadowCoverage;
    j["renderer"]["shadow"]["nearExtend"] = settings.shadowNearExtend;
```

with:

```cpp
    j["renderer"]["shadow"]["enabled"]      = settings.shadowEnabled;
    j["renderer"]["shadow"]["bias"]         = settings.shadowBias;
    j["renderer"]["shadow"]["distance"]     = settings.shadowDistance;
    j["renderer"]["shadow"]["nearExtend"]   = settings.shadowNearExtend;
    j["renderer"]["shadow"]["normalOffset"] = settings.shadowNormalOffset;
    j["renderer"]["shadow"]["pcfRadius"]    = settings.shadowPcfRadius;
```

- [ ] **Step 5: Update the Application seed**

In `src/engine/src/core/Application.cpp`, replace the shadow seed block (lines 27-31):

```cpp
        ShadowSettings& sh = GetShadowSettings();
        sh.Enabled        = m_AppContext->Settings.shadowEnabled;
        sh.Bias           = m_AppContext->Settings.shadowBias;
        sh.ShadowCoverage = m_AppContext->Settings.shadowCoverage;
        sh.NearExtend     = m_AppContext->Settings.shadowNearExtend;
```

with:

```cpp
        ShadowSettings& sh = GetShadowSettings();
        sh.Enabled      = m_AppContext->Settings.shadowEnabled;
        sh.Bias         = m_AppContext->Settings.shadowBias;
        sh.ShadowDistance = m_AppContext->Settings.shadowDistance;
        sh.NearExtend   = m_AppContext->Settings.shadowNearExtend;
        sh.NormalOffset = m_AppContext->Settings.shadowNormalOffset;
        sh.PcfRadius    = m_AppContext->Settings.shadowPcfRadius;
```

- [ ] **Step 6: Update the editor panel sliders**

In `src/editor/src/panels/RenderStatsPanel.cpp`, replace lines 51-54:

```cpp
    ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::SliderFloat("Shadow coverage", &sh.ShadowCoverage, 5.0f, 200.0f, "%.0f");
    changed |= ImGui::SliderFloat("Shadow near-extend", &sh.NearExtend, 0.0f, 200.0f, "%.0f");
```

with (keeps the bias slider for now; renames coverage→distance; adds the two new knobs):

```cpp
    ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::SliderFloat("Shadow distance", &sh.ShadowDistance, 10.0f, 150.0f, "%.0f");
    changed |= ImGui::SliderFloat("Shadow near-extend", &sh.NearExtend, 0.0f, 200.0f, "%.0f");
    changed |= ImGui::SliderFloat("Shadow normal-offset", &sh.NormalOffset, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Shadow PCF radius", &sh.PcfRadius, 0.5f, 4.0f, "%.2f");
```

- [ ] **Step 7: Rename the read in ShadowDepthPass (rename only — no behavior change yet)**

In `src/engine/src/rendering/passes/ShadowDepthPass.cpp`, line 105, replace:

```cpp
        const float coverage = glm::max(shadow.ShadowCoverage, 1.0f); // box width (world units)
```

with:

```cpp
        const float coverage = glm::max(shadow.ShadowDistance, 1.0f); // box size (world units); frustum-fit in a later task
```

(The surrounding `GroundFocus`/`SnapToTexelGrid`/`ComputeLightViewProj` logic is unchanged in this task; Task 3 replaces it.)

- [ ] **Step 8: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds with no errors. (`ShadowCoverage` no longer referenced anywhere; `Bias`/`shadowBias` still referenced and still defined → green.)

- [ ] **Step 9: Commit**

```bash
git add src/common/include/ApplicationContext.h src/engine/src/rendering/RenderStats.h src/engine/src/utilities/SettingsManager.cpp src/engine/src/core/Application.cpp src/editor/src/panels/RenderStatsPanel.cpp src/engine/src/rendering/passes/ShadowDepthPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(shadow): rename coverage->distance, add normal-offset + PCF-radius settings"
```

---

### Task 3: Frustum-fit the light box + publish fit radius on `ShadowView`

Replace the `GroundFocus` + fixed-size box in `ShadowDepthPass` with `FrustumSliceSphere(cam.View, cam.Projection, ShadowDistance)`, and publish the fit `radius` on `Renderer::ShadowView` so `LightingRenderPass` can derive shadow-texel world size in Task 4. No unit test (needs a live camera/renderer); validated by the Task-3 smoke. Build stays green because nothing reads `ShadowView::Radius` yet.

**Files:**
- Modify: `src/engine/src/rendering/Renderer.h:133` (`ShadowView` struct)
- Modify: `src/engine/src/rendering/passes/ShadowDepthPass.cpp:99-137`

- [ ] **Step 1: Add `Radius` to `ShadowView`**

In `src/engine/src/rendering/Renderer.h`, line 133, replace:

```cpp
    struct ShadowView { glm::mat4 LightVP{1.0f}; int Enabled = 0; };
```

with:

```cpp
    // Radius = world-space half-extent of the fitted ortho box this frame (0 when disabled).
    // LightingRenderPass derives the shadow-texel world size from it (2*Radius / kShadowMapSize).
    struct ShadowView { glm::mat4 LightVP{1.0f}; int Enabled = 0; float Radius = 0.0f; };
```

- [ ] **Step 2: Replace the box-fit block with the frustum slice**

In `src/engine/src/rendering/passes/ShadowDepthPass.cpp`, replace the fit block at lines 102-112:

```cpp
    {
        const CameraView& cam = m_Renderer->GetActiveCamera();
        const glm::vec3 fwd  = CameraForward(cam.View);
        const float coverage = glm::max(shadow.ShadowDistance, 1.0f); // box size (world units); frustum-fit in a later task
        if (glm::dot(fwd, fwd) > 0.5f) {                              // valid camera basis
            const glm::vec3 focus = GroundFocus(cam.Position, fwd, coverage);
            radius = coverage * 0.5f;                                 // ortho half-extent
            center = SnapToTexelGrid(focus, radius, sunDir, Renderer::kShadowMapSize);
            haveFit = true;
        }
    }
```

with:

```cpp
    {
        const CameraView& cam = m_Renderer->GetActiveCamera();
        const glm::vec3 fwd = CameraForward(cam.View);
        if (glm::dot(fwd, fwd) > 0.5f) {                              // valid camera basis
            // Fit the ortho box to the camera frustum slice [near, ShadowDistance], so the fixed
            // 4096^2 texels concentrate exactly on what the camera sees. Smaller ShadowDistance
            // -> smaller box -> smaller texels -> sharper shadows.
            const float dist = glm::max(shadow.ShadowDistance, 1.0f);
            const ShadowSphere s = FrustumSliceSphere(cam.View, cam.Projection, dist);
            radius = glm::max(s.radius, 1.0f);                        // ortho half-extent
            center = SnapToTexelGrid(s.center, radius, sunDir, Renderer::kShadowMapSize);
            haveFit = true;
        }
    }
```

- [ ] **Step 3: Publish the fit radius**

In `src/engine/src/rendering/passes/ShadowDepthPass.cpp`, find the publish lines (136-137):

```cpp
    sv.LightVP = lightVP;
    sv.Enabled = 1;
```

replace with:

```cpp
    sv.LightVP = lightVP;
    sv.Enabled = 1;
    sv.Radius  = radius;
```

(`radius` here is the value set in the fit block / AABB fallback above — both paths assign it before this point.)

- [ ] **Step 4: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds with no errors. (`FrustumSliceSphere`/`ShadowSphere` come from the already-included `ShadowMath.h`.)

- [ ] **Step 5: Manual smoke (frustum-fit)**

Launch the editor (`./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`), enter Play / look at the scene with shadows enabled:
- Zoom the gameplay camera **in** → shadow edges noticeably sharper than before (box shrank to the visible area, texels smaller).
- Zoom **out** (modest max) → shadows still present across the view, edges coarser but not broken.
- Pan the camera → no shadow crawl/swim (texel-snap holds within a fixed zoom).
- Drag "Shadow distance" down in the Render Stats panel → shadows sharpen and the far cutoff pulls in; up → softer/longer.
- Skinned characters still cast correct (deforming) shadows.
Expected: visibly sharper near shadows, no NVRHI validation errors in the console. (Edges are still hard/jagged — PCF + normal-offset come in Task 4.)

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/rendering/Renderer.h src/engine/src/rendering/passes/ShadowDepthPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(shadow): frustum-fit light box via FrustumSliceSphere + publish fit radius"
```

---

### Task 4: Rotated Poisson PCF + normal-offset bias; retire the depth-bias slider

Rewrite `ShadowFactor` in the `LightingRenderPass` pixel shader: 16-tap per-pixel-rotated Poisson PCF (soft edge) + normal-offset bias (acne fix) + a tiny hardcoded constant bias. Repack the lighting CB (drop `ShadowBias`, add `PcfRadius`, `ShadowTexel`, `NormalOffsetWorld`). Then remove the now-dead depth-bias slider/field/setting/JSON key end-to-end. Validated by the Task-4 smoke (shader-side; no unit test).

**CB layout (new):** the existing trailing block
`uint PointLightCount; int ShadowEnabled; float ShadowBias; int FogEnabled; int SsaoEnabled; int _ssaoPad0..2;`
becomes two clean 16-byte rows:
`uint PointLightCount; int ShadowEnabled; int FogEnabled; int SsaoEnabled;` then
`float PcfRadius; float ShadowTexel; float NormalOffsetWorld; float _pad0;`.
The C++ struct and HLSL `cbuffer` must match field-for-field.

**Files:**
- Modify: `src/engine/src/rendering/passes/LightingRenderPass.h:28-29` (CB struct)
- Modify: `src/engine/src/rendering/passes/LightingRenderPass.cpp` (shader `cbuffer` ~36, `ShadowFactor` 49-60, caller ~72, CB populate 243-244)
- Modify: `src/engine/src/rendering/RenderStats.h` (remove `Bias`)
- Modify: `src/common/include/ApplicationContext.h` (remove `shadowBias`)
- Modify: `src/engine/src/utilities/SettingsManager.cpp` (remove `bias` load + save)
- Modify: `src/engine/src/core/Application.cpp` (remove `Bias` seed)
- Modify: `src/editor/src/panels/RenderStatsPanel.cpp` (remove "Shadow bias" slider)

- [ ] **Step 1: Repack the C++ CB struct**

In `src/engine/src/rendering/passes/LightingRenderPass.h`, replace lines 28-29:

```cpp
        uint32_t  PointLightCount; int ShadowEnabled; float ShadowBias; int FogEnabled;
        int SsaoEnabled; int _ssaoPad0; int _ssaoPad1; int _ssaoPad2;
```

with:

```cpp
        uint32_t  PointLightCount; int ShadowEnabled; int FogEnabled; int SsaoEnabled;
        float PcfRadius; float ShadowTexel; float NormalOffsetWorld; float _pad0;
```

- [ ] **Step 2: Update the HLSL `cbuffer` to match**

In `src/engine/src/rendering/passes/LightingRenderPass.cpp`, replace the two CB tail lines (36-37):

```cpp
    uint uPointLightCount; int uShadowEnabled; float uShadowBias; int uFogEnabled;
    int uSsaoEnabled; int3 _ssaoPad;
```

with:

```cpp
    uint uPointLightCount; int uShadowEnabled; int uFogEnabled; int uSsaoEnabled;
    float uPcfRadius; float uShadowTexel; float uNormalOffsetWorld; float _pad0;
```

- [ ] **Step 3: Rewrite `ShadowFactor` (Poisson PCF + normal-offset)**

In `src/engine/src/rendering/passes/LightingRenderPass.cpp`, replace the entire `ShadowFactor` function (lines 49-60):

```cpp
float ShadowFactor(float3 worldPos, float ndl){
    if (uShadowEnabled == 0) return 1.0;
    float4 lp = mul(uLightVP, float4(worldPos,1.0));
    float3 p = lp.xyz / lp.w;
    float2 uv = p.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    if (uv.x<0.0||uv.x>1.0||uv.y<0.0||uv.y>1.0) return 1.0;
    float bias = uShadowBias * (1.0 + (1.0-ndl)*2.0);
    // Single comparison tap. The shadow sampler is a linear-compare sampler, so the hardware
    // already does 2x2 bilinear PCF here — the sharpest smooth edge the map resolution allows.
    // Edge sharpness is resolution-bound (texel = ShadowCoverage / kShadowMapSize), not filter-bound.
    return uShadowMap.SampleCmpLevelZero(uShadowSamp, uv, p.z - bias);
}
```

with:

```cpp
// 16-tap Poisson disk (unit disk, ~[-1,1]). Rotated per-pixel so PCF banding becomes dither.
static const float2 kPoisson[16] = {
    float2(-0.94201624, -0.39906216), float2( 0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2( 0.34495938,  0.29387760),
    float2(-0.91588581,  0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543,  0.27676845), float2( 0.97484398,  0.75648379),
    float2( 0.44323325, -0.97511554), float2( 0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2( 0.79197514,  0.19090188),
    float2(-0.24188840,  0.99706507), float2(-0.81409955,  0.91437590),
    float2( 0.19984126,  0.78641367), float2( 0.14383161, -0.14100790)
};

// Normal-offset bias replaces the slope-scaled depth bias; this tiny constant covers residual acne.
static const float kConstShadowBias = 5e-4;

float ShadowFactor(float3 worldPos, float3 N, float4 svpos){
    if (uShadowEnabled == 0) return 1.0;
    // Offset the sample position along the surface normal (normal-offset bias) before projecting.
    float3 biasedWP = worldPos + N * uNormalOffsetWorld;
    float4 lp = mul(uLightVP, float4(biasedWP, 1.0));
    float3 p = lp.xyz / lp.w;
    float2 uv = p.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    if (uv.x<0.0||uv.x>1.0||uv.y<0.0||uv.y>1.0) return 1.0;

    // Per-pixel rotation via interleaved gradient noise on screen position.
    float ign = frac(52.9829189 * frac(dot(svpos.xy, float2(0.06711056, 0.00583715))));
    float a = ign * 6.2831853;
    float sa = sin(a), ca = cos(a);
    float2x2 rot = float2x2(ca, -sa, sa, ca);

    float zref = p.z - kConstShadowBias;
    float sum = 0.0;
    [unroll] for (int t = 0; t < 16; ++t){
        float2 off = mul(rot, kPoisson[t]) * uPcfRadius * uShadowTexel; // texels -> UV
        sum += uShadowMap.SampleCmpLevelZero(uShadowSamp, uv + off, zref);
    }
    return sum * (1.0 / 16.0);
}
```

- [ ] **Step 4: Update the `ShadowFactor` call site**

In `src/engine/src/rendering/passes/LightingRenderPass.cpp`, in `main_ps` (line 72), replace:

```cpp
    lighting += diffuse * uDir.Color.rgb * ShadowFactor(wp, diffuse);
```

with (passes the normalized G-buffer normal `N` and the screen position `i.PosH`):

```cpp
    lighting += diffuse * uDir.Color.rgb * ShadowFactor(wp, N, i.PosH);
```

(`N` is already `normalize`d at line 67; `i.PosH` is `SV_POSITION` from `PSIn`.)

- [ ] **Step 5: Populate the new CB fields, drop the old bias write**

In `src/engine/src/rendering/passes/LightingRenderPass.cpp`, replace lines 242-244:

```cpp
    cb.LightVP = sv.LightVP;
    cb.ShadowEnabled = sv.Enabled;
    cb.ShadowBias = GetShadowSettings().Bias;
```

with:

```cpp
    cb.LightVP = sv.LightVP;
    cb.ShadowEnabled = sv.Enabled;
    {
        const ShadowSettings& shset = GetShadowSettings();
        const float texelWorld = (sv.Radius > 0.0f)
            ? (2.0f * sv.Radius / static_cast<float>(Renderer::kShadowMapSize)) : 0.0f;
        cb.PcfRadius         = shset.PcfRadius;
        cb.ShadowTexel       = 1.0f / static_cast<float>(Renderer::kShadowMapSize); // UV-space texel
        cb.NormalOffsetWorld = shset.NormalOffset * texelWorld;                     // texels -> world
    }
```

- [ ] **Step 6: Remove `Bias` from the live `ShadowSettings`**

In `src/engine/src/rendering/RenderStats.h`, delete this line from `ShadowSettings`:

```cpp
    float Bias           = 0.0015f; // (removed in Task 4 once the shader uses normal-offset)
```

- [ ] **Step 7: Remove `shadowBias` from the persisted settings**

In `src/common/include/ApplicationContext.h`, delete this line:

```cpp
    float       shadowBias        = 0.0015f;
```

- [ ] **Step 8: Remove the `bias` load + save**

In `src/engine/src/utilities/SettingsManager.cpp`, delete the load line:

```cpp
            if (js.contains("bias")        && js["bias"].is_number())        out->shadowBias         = js["bias"].get<float>();
```

and the save line:

```cpp
    j["renderer"]["shadow"]["bias"]         = settings.shadowBias;
```

- [ ] **Step 9: Remove the `Bias` seed**

In `src/engine/src/core/Application.cpp`, delete this line from the shadow seed block:

```cpp
        sh.Bias         = m_AppContext->Settings.shadowBias;
```

- [ ] **Step 10: Remove the "Shadow bias" slider**

In `src/editor/src/panels/RenderStatsPanel.cpp`, delete these two lines:

```cpp
    ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
```

- [ ] **Step 11: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds with no errors. (No remaining references to `Bias`/`shadowBias`/`uShadowBias`.)

- [ ] **Step 12: Run the math test (regression guard)**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_shadowmath && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_shadowmath.exe`
Expected: `All shadow-math tests passed.` (Unchanged — guards the fit math the renderer relies on.)

- [ ] **Step 13: Manual smoke (PCF + normal-offset)**

Launch the editor, shadows enabled, some sun + ambient:
1. Zoom in → edges **soft** (penumbra), not stair-stepped/jagged.
2. Zoom out (modest max) → still clean, no blocky stairstep.
3. Pan → no crawl.
4. Flat ground + thin/one-quad walls → **no acne** stripes. If any acne, raise "Shadow normal-offset"; if shadows detach/float (peter-panning), lower it.
5. Character feet / object bases → contact shadow stays attached.
6. Drag "Shadow PCF radius" up → softer/wider penumbra; down → tighter. Drag "Shadow normal-offset" → acne vs peter-pan tradeoff visible.
7. Toggle "Shadows" off → shadows gone, scene otherwise unchanged.
8. No NVRHI validation errors; skinned shadows still deform.
Expected: soft, crisp, acne-free shadows; the bias slider is gone, replaced by normal-offset + PCF radius.

- [ ] **Step 14: Commit**

```bash
git add src/engine/src/rendering/passes/LightingRenderPass.h src/engine/src/rendering/passes/LightingRenderPass.cpp src/engine/src/rendering/RenderStats.h src/common/include/ApplicationContext.h src/engine/src/utilities/SettingsManager.cpp src/engine/src/core/Application.cpp src/editor/src/panels/RenderStatsPanel.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(shadow): rotated Poisson PCF + normal-offset bias; retire depth-bias slider"
```

---

## Final review (after all tasks)

Dispatch a whole-branch code review (spec compliance + quality) per subagent-driven-development, then hand off to `superpowers:finishing-a-development-branch` for the FF-merge to `main`. Confirm before pushing.

**Whole-branch smoke checklist (user-run):** all of Task 3 Step 5 + Task 4 Step 13, plus a fresh-launch run with a pre-existing `engine_settings.json` (old `coverage`/`bias` keys ignored, new `distance`/`normalOffset`/`pcfRadius` default in cleanly), and `test_shadowmath` green.

## Notes / gotchas (carried from the spec)

- **CB alignment:** the C++ `LightFrameCB` tail and the HLSL `cbuffer` tail must match field-for-field (two 16-byte rows). A mismatch silently corrupts shadows — verify both edits landed (Steps 1-2) before building.
- **Dynamic radius:** `sv.Radius` changes per frame with zoom, so `NormalOffsetWorld` (world units) tracks it; `ShadowTexel` (UV) is constant `1/4096`. One-frame edge shift on a hard zoom is acceptable for modest zoom; do not over-engineer.
- **Normal source:** `ShadowFactor` uses the already-normalized G-buffer `N`; sky/no-geometry pixels early-out (`dot(N,N) < 0.5`) before lighting, so no offset is applied there.
- **Breaking settings:** old `engine_settings.json` files lose `coverage`/`bias` and gain defaults for the new keys — expected per the early-dev breaking-changes policy; the gitignored local file re-defaults on first run.
- **Build-green ordering:** never remove `Bias`/`shadowBias` before Task 4 — Tasks 1-3 leave them in place so the shader keeps compiling.
```
