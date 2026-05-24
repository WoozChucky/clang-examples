# Directional Shadow Maps (single map, dynamic) — Design

**Date:** 2026-05-24
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main` (implementation on `directional-shadows`).

## Goal

Add real-time **directional (sun) shadows** via a single shadow map: a depth pass from the light's
point of view writes a depth texture; `MeshRenderPass` samples it (3×3 PCF) to darken surfaces the sun
can't see. Fully **dynamic** — the light-space matrix is recomputed each frame from the day/night sun
direction, and the shadow pass is **gated off when the sun is below the horizon**. Shadows are a real
rendering feature (on by default in editor **and** runtime), not a debug overlay.

## Background (verified)

- **Sun / day-night** (`src/game/src/game.cpp:42-85`, `DayNightSystem`, `SystemPhase::Simulation`):
  each tick sets the `SunMarker` directional light's `LightningComponent::Direction` (a `glm::vec4`) =
  `normalize(0, -cos(theta), sin(theta))` from `DayNightConfigComponent::CycleSeconds`. So the sun
  rotates; **sun is above the horizon when `Direction.y < 0`** (it points downward). `Direction.y >= 0`
  = sun at/below horizon.
- **`MeshRenderPass`** (`src/engine/src/rendering/passes/MeshRenderPass.{h,cpp}`):
  - `PerFrameCB { glm::mat4 P; glm::mat4 VP; DirectionalLight {vec4 Direction; vec4 Color;}; uint32_t PointLightCount; float Ambient; uint32_t _pad[2]; }` (b0) — filled + `writeBuffer` each frame.
  - PS directional term (`MESH_PS_HLSL`): `lighting += max(dot(N,-lightDir),0) * uDirLight.Color.rgb` —
    **the shadow factor multiplies here** (directional only; point lights + ambient untouched). `WorldPos`
    + `Normal` are available in the PS.
  - Binding layout/set today: `b0` PerFrameCB, `b1` PerDrawCB, `t2` material tex, `s3` material sampler,
    `t4` point-light SRV, `t5` instance SRV → **next free: `t6` (shadow SRV), `s4` (comparison sampler)**.
- **Depth texture + framebuffer** (pattern from `SceneViewport.cpp`): `nvrhi::TextureDesc{ format=D32,
  isRenderTarget=true, isShaderResource=true, initialState=DepthWrite, keepInitialState=true }` →
  `createFramebuffer(FramebufferDesc().setDepthAttachment(tex))` (no color = depth-only).
- **Sampler** (pattern from `Renderer.cpp:360`): `SamplerDesc` → for shadows add
  `reductionType = nvrhi::SamplerReductionType::Comparison; comparisonFunc = nvrhi::ComparisonFunc::LessOrEqual;`
  (+ linear filters, clamp) → enables `SampleCmp` hardware PCF.
- **Pass model** (`Renderer.cpp:86-120`): ordered `IRenderPass` list (Primitive→Mesh→Outline→Debug→Ui),
  registered in `Init` + `InitForSwap`; teardown in `Shutdown`/`TeardownForSwap`. Each pass holds
  `Renderer* m_Renderer`; `Renderer::GetActiveCamera()` is the per-frame shared-camera precedent for
  sharing the light VP.
- **Scene bounds**: no scene AABB exists; compute from visible meshes via
  `MeshSystem::GetMeshBounds(meshId)` (`{valid,min,max}`) + `TransformAABB(ModelMatrix(t), min, max, …)`
  (`Frustum.h` / `TransformMath.h`), as `ViewportPicker.cpp` does.
- **Render-settings pattern**: `ENGINE_API` function-local-static globals (`GetCullingSettings()`,
  `GetDebugDrawSettings()`) flipped by `RenderStatsPanel` — the channel for the shadow toggles.
- **Shaders**: inline HLSL via DXC (`vs_6_1`/`ps_6_1`), `b#/t#/s#` registers, Vulkan binding offsets set
  per-pass when `getGraphicsAPI()==VULKAN` (same handling needed for the new SRV/sampler). GLM is RH,
  depth `[0,1]` → use `glm::orthoRH_ZO` + `glm::lookAtRH`.

## Scope

**In scope:**
1. `ShadowMath.h` — pure `ComputeLightViewProj(center, radius, sunDir)` + `IsSunUp(direction)` (unit-tested).
2. Renderer-owned shadow GPU resources (D32 texture + depth-only FB + comparison sampler) + a shared
   `ShadowView { glm::mat4 LightVP; int Enabled; }`, with accessors. Created/torn-down with the backend.
3. `ShadowSettings` global (`Enabled` default true, `Bias`) + `GetShadowSettings()` + Render Stats UI.
4. `ShadowDepthPass` — sun-up + master gate, auto-fit light VP to the visible-mesh AABB, render depth
   of visible meshes into the shadow map (front-face cull). Registered before `MeshRenderPass`.
5. `MeshRenderPass` — `PerFrameCB` += `LightVP` + `ShadowEnabled` + `ShadowBias`; bind shadow SRV/sampler;
   PS 3×3-PCF `SampleCmp`, multiply the directional term by the shadow factor.

**Out of scope / non-goals:** cascaded shadow maps (CSM); shadows for point/spot lights; soft/contact-
hardening shadows beyond fixed 3×3 PCF; instanced/batched shadow draws (v1 draws per visible mesh —
perf optimization deferred); texel-snapping anti-shimmer; shadow-map debug-view panel (optional, not
required); configurable map resolution (fixed 2048²). No `GAME_API_VERSION` bump (no
`GameState`/export/ECS-component change).

## Design

### 1. `ShadowMath.h` (pure, unit-tested) — `src/engine/src/rendering/ShadowMath.h`

```cpp
// Sun is above the horizon (casting) when its travel direction points downward (y < 0).
inline bool IsSunUp(const glm::vec3& sunDir, float eps = 0.02f) { return sunDir.y < -eps; }

// Orthographic light view-projection that frames a sphere (scene bounds) along the sun direction.
// sunDir = direction the light travels (points away from the sun). Uses lookAtRH + orthoRH_ZO (depth [0,1]).
inline glm::mat4 ComputeLightViewProj(const glm::vec3& center, float radius, const glm::vec3& sunDir) {
    glm::vec3 d = glm::normalize(sunDir);
    const float r = (radius > 1e-3f) ? radius : 1.0f;
    glm::vec3 up = (std::abs(d.y) > 0.99f) ? glm::vec3(0,0,1) : glm::vec3(0,1,0);
    const glm::vec3 eye = center - d * (r * 2.0f);           // back off toward the sun
    const glm::mat4 view = glm::lookAtRH(eye, center, up);
    const glm::mat4 proj = glm::orthoRH_ZO(-r, r, -r, r, 0.0f, 4.0f * r);
    return proj * view;
}
```

### 2. Renderer-owned shadow resources + shared view

`Renderer` gains:
```cpp
struct ShadowView { glm::mat4 LightVP{1.0f}; int Enabled = 0; };
// members:
nvrhi::TextureHandle     m_ShadowDepth;     // D32, 2048^2, RT + SRV
nvrhi::FramebufferHandle m_ShadowFb;        // depth-only
nvrhi::SamplerHandle     m_ShadowSampler;   // comparison, LessOrEqual
ShadowView               m_ShadowView;      // written by ShadowDepthPass, read by MeshRenderPass
// accessors:
nvrhi::ITexture*   GetShadowDepthTexture() const;
nvrhi::ISampler*   GetShadowSampler() const;
ShadowView&        GetShadowView();         // mutable: ShadowDepthPass sets it, MeshRenderPass reads
static constexpr uint32_t kShadowMapSize = 2048;
```
Created in `Init` + `InitForSwap` (after the device exists); released in `Shutdown` + `TeardownForSwap`
(so the hot-swap path rebuilds them) — same lifecycle as `MeshSystem`/`MaterialSystem` GPU resources.

### 3. `ShadowSettings` global + UI

`RenderStats.{h,cpp}`:
```cpp
struct ShadowSettings { bool Enabled = true; float Bias = 0.0015f; };
ENGINE_API ShadowSettings& GetShadowSettings(); // function-local static, like GetCullingSettings
```
`RenderStatsPanel`: a "Shadows" separator with `Checkbox("Shadows", &GetShadowSettings().Enabled)` +
`SliderFloat("Shadow bias", &GetShadowSettings().Bias, 0.0f, 0.01f, "%.4f")`.

### 4. `ShadowDepthPass` — `src/engine/src/rendering/passes/ShadowDepthPass.{h,cpp}`

Registered in `Init`/`InitForSwap` **after Primitive, before Mesh** (Primitive→**Shadow**→Mesh→Outline→
Debug→Ui). `Render`:
1. **Gate:** read the sun's `Direction` (`Each<SunMarker, LightningComponent>` or the directional light).
   If `!GetShadowSettings().Enabled` or `!IsSunUp(dir)` → set `Renderer.GetShadowView().Enabled = 0` and
   return (MeshRenderPass then renders unshadowed).
2. **Auto-fit:** iterate visible `Transform+Mesh` → accumulate the world AABB (`GetMeshBounds` +
   `TransformAABB`). `center = (min+max)/2`, `radius = length(max-min)/2`. If no casters → `Enabled=0`,
   return. `LightVP = ComputeLightViewProj(center, radius, dir)`; store into `Renderer.GetShadowView()`
   (`LightVP`, `Enabled=1`).
3. **Render depth:** clear the shadow depth FB; depth-only pipeline (VS only, no PS; `TriangleList`,
   depthTest+write on, **cull FRONT**). For each visible mesh: write its `Model` into a small per-draw CB
   (`cbuffer { float4x4 LightVP; float4x4 Model; }`), bind the mesh VB/IB, `drawIndexed`. (Per-entity
   draws — simple; instanced/batched shadows are a deferred perf item.)

VS: `o.PosH = mul(LightVP, mul(Model, float4(Position,1)))`. Viewport = the 2048² shadow map.

### 5. `MeshRenderPass` integration

- `PerFrameCB` += `glm::mat4 LightVP; int ShadowEnabled; float ShadowBias; float _pad2[2];` (keep 16-byte
  alignment). Filled each frame from `Renderer.GetShadowView()` (`LightVP`, `Enabled`) +
  `GetShadowSettings().Bias`.
- Binding layout += `Texture_SRV(6)` + `Sampler(4)`; binding set binds
  `Renderer.GetShadowDepthTexture()` + `GetShadowSampler()`. Add the Vulkan binding offsets for the new
  slots alongside the existing ones.
- PS:
  ```hlsl
  Texture2D            uShadowMap     : register(t6);
  SamplerComparisonState uShadowSamp  : register(s4);
  float ShadowFactor(float3 worldPos, float ndl) {
      if (uShadowEnabled == 0) return 1.0;
      float4 lp = mul(uLightVP, float4(worldPos, 1.0));
      float3 p = lp.xyz / lp.w;
      float2 uv = p.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;     // clip -> [0,1], D3D Y-flip
      if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return 1.0; // outside the map = lit
      float bias = uShadowBias * (1.0 + (1.0 - ndl) * 2.0); // slope-scaled
      float d = p.z - bias;
      float sum = 0; float2 texel = 1.0 / 2048.0;
      [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x)
          sum += uShadowMap.SampleCmpLevelZero(uShadowSamp, uv + float2(x,y)*texel, d);
      return sum / 9.0;
  }
  // directional term:  lighting += diffuse * uDirLight.Color.rgb * ShadowFactor(WorldPos, max(dot(N,-L),0));
  ```
  Bias is applied in the PS (subtract from the compared depth, slope-scaled by `N·L`) so the **Bias
  slider is live** (no pipeline recreation). Front-face culling in the depth pass handles most
  peter-panning; the PS bias handles acne.

## Data flow

GameThread `DayNightSystem` rotates the sun (`Direction`) → snapshot. Each frame (render thread):
`ShadowDepthPass` reads the sun + visible meshes → computes `LightVP` + renders the depth map (or gates
off) → publishes `Renderer.ShadowView`. `MeshRenderPass` reads `ShadowView` + binds the map → PS PCF
test darkens the sun term. Sun sets → `Enabled=0` → no shadow pass + unshadowed mesh; sun rises → back on.

## Build / verification

Build preset `msvc-win64-vs2026-community`. New engine sources → reconfigure. No `GAME_API_VERSION` bump.

- **Unit test (`test_shadowmath`)** on the pure helpers:
  - `IsSunUp({0,-1,0})` true; `IsSunUp({0,1,0})`/`IsSunUp({0,0,1})` false (at/below horizon).
  - `ComputeLightViewProj(center, r, sunDir)`: the `center` projects to ~NDC origin (`|x|,|y|<1e-3`, `z∈[0,1]`);
    the 8 corners of the bounding sphere's box (`center ± r` on each axis) all land within `[-1,1]` in x/y and
    `z∈[0,1]` (scene fully inside the light frustum); a straight-down `sunDir` ({0,-1,0}) produces no NaNs
    (alternate up). Prints `All shadow math tests passed.`
- `editor` + `runtime` build clean; the other 10 test suites stay green.
- **GUI smoke (user-run):** with a mesh on the ground, the sun casts a shadow that **moves as the
  day/night cycle rotates the sun**; the shadow disappears at night (sun below horizon) and returns at
  sunrise; the "Shadows" checkbox toggles it; the Bias slider trades acne (too low → speckle) vs
  peter-panning (too high → detached). `runtime.exe` shows shadows too (default on).

## Risks

- **Shadow acne / peter-panning** — the classic tuning pair; mitigated by front-face cull (depth pass) +
  a live slope-scaled PS bias slider. Default `Bias=0.0015`.
- **Whole-scene single map resolution** — auto-fit spreads 2048² over the entire scene AABB; fine for the
  current small scenes, blocky for huge ones (CSM is the future fix; noted non-goal).
- **Long shadows at low sun angles** — the AABB-fit ortho box covers the scene, but very low sun → very
  long shadows can still exceed the box laterally; the sun-up `eps` gate trims the near-horizontal
  degenerate range.
- **Hot-swap** — shadow texture/FB/sampler must be recreated in `InitForSwap` + released in
  `TeardownForSwap` (same as other GPU resources), else a backend swap leaves dangling handles.
- **CB alignment** — growing `PerFrameCB` must stay 16-byte aligned (mat4 + int + float + 2 pad).
- **Vulkan bindings** — the new `t6`/`s4` need the same `VulkanBindingOffsets` treatment as the existing
  slots, or the Vulkan path mis-binds.
