# Procedural Sky + Sun/Moon Discs — Design

**Date:** 2026-05-25
**Status:** Approved (pending implementation plan)

## Goal

Render a visible sky behind the scene: a day/night-tinted vertical gradient plus
a sun disc and a moon disc that move across the sky with the existing day/night
cycle (the moon opposite the sun). Today the "sky" is just the flat fog/clear
color; this adds an actual sky with celestial bodies.

Builds on the deferred renderer + day/night cycle already shipped. The sun
direction already exists (driven by `DayNightSystem`); this feature only draws
it.

## Scope

In scope: a new full-screen `SkyRenderPass` that paints the gradient + sun disc +
moon disc into sky pixels, plus engine-side `SkySettings` (editor-tunable) and a
Sky section in the editor's Render Stats panel. **Plus a documentation pass**
(see below) bringing `README.md` / `CLAUDE.md` up to date with the deferred
pipeline and the fog / day-night / sky features shipped this cycle.

Out of scope (YAGNI / future): a moon texture or phases; stars/cubemap at night;
clouds; atmospheric scattering (Rayleigh/Mie); lens flare; HDR/bloom on the sun
(the scene is LDR today). Plain procedural discs + glow + gradient only.

## Architecture

### Pass order
```
Shadow -> GBufferFill -> Lighting -> Sky -> Primitive(grid) -> Outline -> Debug -> UI
```
`SkyRenderPass` runs right after `Lighting`. The lighting pass is left untouched
(its `dot(N,N)<0.5 -> fog color` sky branch still runs; the sky pass simply
overwrites the visible sky pixels — so if the sky pass is disabled, the old flat
sky remains as a fallback).

### Sky-only via far-plane depth test (no extra G-buffer read)
The pass is a full-screen triangle emitted at clip-space `z = 1.0` (the far
plane; this engine uses standard depth where the buffer clears to 1.0 = far).
Pipeline: `depthTestEnable = true`, `depthFunc = LessOrEqual`, **`depthWrite =
false`**, `cull = None`, blend off. It renders into the scene framebuffer
(`sceneBuffer`, color + the shared scene depth that `GBufferFillPass` wrote).
- Geometry pixels have depth < 1.0 → incoming `1.0 <= storedDepth` is false →
  rejected (geometry keeps its lit color).
- Sky pixels have depth == 1.0 (cleared, no geometry) → `1.0 <= 1.0` true →
  drawn.
This is the textbook skybox technique; no G-buffer normal/depth SRV needed.

### Pixel shader
1. **View ray** — reconstruct the world-space ray for the pixel from screen UV
   using the inverse view-projection (added to the pass CB) and the camera
   position:
   ```
   float2 ndc = uv * 2 - 1; ndc.y = -ndc.y;          // match the engine's UV/clip convention
   float4 wpos = mul(uInvViewProj, float4(ndc, 1.0, 1.0)); // far plane
   float3 rayDir = normalize(wpos.xyz / wpos.w - uCameraPos.xyz);
   ```
2. **Sun elevation** — `elev = clamp(-uSunDir.y, 0, 1)` (same metric as fog/day-night),
   used to cross-fade the day vs night sky palette.
3. **Gradient** — `t = saturate(rayDir.y)` (horizon→zenith); for day and night
   separately `mix(HorizonColor, ZenithColor, t)`, then `mix(night, day, elev)`.
4. **Sun disc** — `s = dot(rayDir, -uSunDir.xyz)` (−sunDir points toward the sun
   in the sky); `disc = smoothstep(cos(R+soft), cos(R), s)` where `R` =
   `radians(SunAngularRadiusDeg)`; plus a halo `pow(saturate(s), SunGlow)`.
   Add `SunColor * (disc + halo*haloStrength)` to the sky color.
5. **Moon disc** — identical with the direction `+uSunDir.xyz` (opposite the sun)
   and `MoonColor`/`MoonAngularRadiusDeg`/`MoonGlow`.
The sun disc only appears while the sun is above the horizon (its sky direction
points up), the moon while the sun is below — so they rise/set automatically with
the orbit. The disc colors come from `SkySettings` (always bright when visible),
independent of the directional light's faded color.

## Components

- **`src/engine/src/rendering/Sky.h` / `Sky.cpp`** (new) — `SkySettings` struct +
  `ENGINE_API SkySettings& GetSkySettings();` (function-local static, same pattern
  as `Fog.{h,cpp}` / `GetFogSettings`). Fields:
  ```cpp
  struct SkySettings {
      bool      Enabled = true;
      glm::vec3 DayZenith   = {0.20f, 0.40f, 0.85f};
      glm::vec3 DayHorizon  = {0.70f, 0.80f, 0.95f};
      glm::vec3 NightZenith = {0.01f, 0.02f, 0.06f};
      glm::vec3 NightHorizon= {0.04f, 0.05f, 0.12f};
      glm::vec3 SunColor    = {1.00f, 0.95f, 0.80f};
      float     SunRadiusDeg = 3.0f;
      float     SunGlow      = 64.0f;   // halo falloff exponent
      glm::vec3 MoonColor   = {0.80f, 0.85f, 1.00f};
      float     MoonRadiusDeg = 2.5f;
      float     MoonGlow      = 128.0f;
  };
  ```
  Add `Sky.cpp` to `src/engine/CMakeLists.txt`.
- **`src/engine/src/rendering/passes/SkyRenderPass.{h,cpp}`** (new) — models on
  `LightingRenderPass` (full-screen triangle, no input layout, `draw(3)`). A
  `SkyFrameCB` carries `InvViewProj`, `CameraPos`, `SunDir`, and the `SkySettings`
  values (colors/radii/glow + an `Enabled`/blend). The pass reads the sun
  direction from the ECS (`world->Each<...>` for the directional light, as the
  lighting pass does), the camera from `m_Renderer->GetActiveCamera()` (computes
  `InvViewProj = inverse(Projection * View)`), and `GetSkySettings()`. Pipeline
  built against the scene framebuffer with the far-plane depth test above. Binding
  layout = a single constant buffer (b0) — no textures (plain discs). Early-return
  (draw nothing) when `!GetSkySettings().Enabled`.
- **`Renderer`** — register `SkyRenderPass` after `LightingRenderPass` in both
  `Init` and `InitForSwap`.
- **Editor** — add a "Sky" section to `RenderStatsPanel.cpp` (same as the Fog
  section): `Enabled` checkbox, color pickers for the 4 gradient colors + sun/moon
  colors, sliders for sun/moon radius + glow. Bound to `GetSkySettings()`.

## Data flow
```
sun dir (ECS directional light, set by DayNightSystem)
camera (GetActiveCamera -> InvViewProj, CameraPos)
SkySettings (engine global, editor-tuned)
        -> SkyRenderPass builds SkyFrameCB -> full-screen draw @ far depth
        -> sky pixels: gradient(day/night by sun elevation) + sun disc + moon disc
        -> sceneBuffer color (geometry pixels rejected by depth test)
```

## Error / edge handling
- **No directional light:** default sun direction (e.g. straight down) → sky still
  renders a sane gradient.
- **`Enabled == false`:** the pass draws nothing; the lighting pass's flat fog-color
  sky remains.
- **Editor vs runtime camera:** `GetActiveCamera()` already resolves editor-override
  vs game camera; `InvViewProj` is derived from it, so the sky matches whatever
  the scene was rendered with.
- **CB layout:** `SkyFrameCB` must be 16-byte aligned with a `static_assert`
  (mirror the other passes); `glm::vec3` fields padded to vec4 in the CB to match
  HLSL packing.

## Documentation pass

The project docs still describe the **forward** renderer and are stale after this
cycle's deferred conversion + fog/day-night work. As the final step of this
feature (done last, so the docs describe the final state including the sky),
update:

- **`README.md`** — the "Renderer" + "Render passes" sections (currently lines
  ~130-168) list `MeshRenderPass` and a forward flow. Rewrite to describe the
  **deferred** pipeline and the actual pass order:
  `ShadowDepthPass → GBufferFillPass (G-buffer MRT: albedo/normal/world-pos +
  depth) → LightingRenderPass (full-screen: directional + point + shadow + fog,
  reads the G-buffer) → SkyRenderPass (procedural gradient + sun/moon discs) →
  PrimitiveRenderPass (grid) → OutlineRenderPass → DebugRenderPass → UiRenderPass`.
  Mention fog (`Fog.{h,cpp}` / `GetFogSettings`), the day/night cycle
  (`DayNightSystem`, `DayNightConfigComponent`, `AtmosphereStateComponent`), and
  sky (`Sky.{h,cpp}` / `GetSkySettings`). Remove `MeshRenderPass` references.
- **`CLAUDE.md`** — the "Renderer (Engine)" architecture paragraph (~line 96):
  replace the forward/`MeshRenderPass` description with the deferred pipeline +
  the pass list above + the new editor-tunable settings (`FogSettings`,
  `SkySettings`) and the day/night components. **Fix the stale line** stating the
  `ECSCommandProcessor` lives in `ApplicationContext.h` — it is actually in
  `src/common/include/ECSCommands.h` (update both the "ECS command pattern" and
  the "adding a new component type" references that cite `ApplicationContext.h`).
- **`docs/ECS_Threading_Architecture.md`** — only if it makes a now-false claim
  about the renderer passes or the component set; otherwise leave it (the
  snapshot/command threading model is unchanged). Light touch only.

Keep edits accurate to the merged code; do not document features that don't
exist. This is a docs-only change (no code), committed separately.

## Testing / verification
No unit tests (renderer visual work; project norm). Manual:
- Build + run the editor; scrub the day/night cycle and confirm the sun rises,
  crosses, and sets with the moon opposite it; the gradient shifts day↔night; the
  discs are occluded correctly by foreground geometry (far-plane depth test);
  disabling Sky reverts to the flat sky.
- Tune via the Sky panel (colors, sun/moon size + glow) and confirm live changes.
- `test_ecs` unaffected (no ECS change) but should still pass after build.

## Files (anticipated)
- `src/engine/src/rendering/Sky.h`, `Sky.cpp` (new)
- `src/engine/src/rendering/passes/SkyRenderPass.h`, `SkyRenderPass.cpp` (new)
- `src/engine/src/rendering/Renderer.cpp` (pass registration, both Init paths)
- `src/engine/CMakeLists.txt` (add Sky.cpp + SkyRenderPass.cpp)
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` (Sky tuning section)
- `README.md`, `CLAUDE.md` (documentation pass; `docs/ECS_Threading_Architecture.md` only if it has a now-false claim)

## Notes
- No `ECS.h`/`Game.h` change → no `ecs.dll` rebuild/restart beyond a normal build;
  the sun direction is consumed from the existing directional light.
- This is a new feature on top of merged `main`; implement on a fresh branch.
- The far-plane depth-test relies on `GBufferFillPass` being the sole writer of the
  scene depth (true today) and the standard (non-reversed) depth convention
  (buffer clears to 1.0 = far).
