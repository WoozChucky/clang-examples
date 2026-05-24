# Atmospheric Fog — Design

**Date:** 2026-05-24
**Status:** Approved (pending implementation plan)

## Goal

Exponential distance fog driven by the sun's elevation, synced to the existing
day/night cycle:

- **Noticeable + dark at night**, near-zero during the day.
- **Sky-matched:** the background clear color equals the fog color, so distant
  geometry fades into the horizon with no visible seam.

This is item #6 ("atmospheric fog") from the day/night-improvement discussion,
chosen as the first improvement. It is independent of the brightness/ambient
fixes (#1/#2) and does not depend on them.

## Approach

**Render-side, derived from the sun (no game/ECS changes).** The directional
sun light direction already flows to the renderer each frame (the mesh pass
finds it via `world->Each<TransformComponent, LightningComponent>`). Fog density
and color are computed from that direction's elevation. No new ECS component, no
cross-thread plumbing, no `game.cpp` changes. Matches the existing
`GetShadowSettings()` / `GetCullingSettings()` rendering-settings pattern.

Falloff model: **exponential** — `fogFactor = 1 - exp(-density * dist)`. One
density knob, soft from the camera outward (no fog wall), cheap. Density and
color are animated by the cycle rather than the two distance knobs a linear
model would need.

## Single source of truth

`ComputeFog` is the only place elevation maps to color/density. The Renderer
calls it (for the clear color) and the mesh pass consumes the Renderer's stored
result (for the CB) — no duplicated mapping, no drift.

`elevation = clamp(-sunDir.y, 0, 1)` — same metric `DayNightSystem` already uses
in `game.cpp`, so fog tracks the sun automatically. `elevation = 1` at noon,
`0` at/below the horizon (night).

## Components

### 1. `Fog.h` / `Fog.cpp` (new, `src/engine/src/rendering/`)

```cpp
struct FogSettings {
    bool      Enabled      = true;
    float     DayDensity   = 0.008f;              // barely-there daytime haze
    float     NightDensity = 0.09f;               // noticeable at night
    glm::vec3 DayColor     = {0.60f, 0.70f, 0.80f}; // hazy blue-grey
    glm::vec3 NightColor   = {0.03f, 0.04f, 0.08f}; // dark blue
};
ENGINE_API FogSettings& GetFogSettings();          // single Engine.dll instance

struct FogFrame { glm::vec3 Color; float Density; };
FogFrame ComputeFog(glm::vec3 sunDir, const FogSettings& s); // pure
```

`ComputeFog`:
```
elevation = clamp(-sunDir.y, 0, 1)
Density   = mix(NightDensity, DayDensity, elevation)
Color     = mix(NightColor,   DayColor,   elevation)
```

`GetFogSettings()` follows the `RenderStats.cpp` pattern: a single
`ENGINE_API`-exported function-local static so the mesh pass (Engine.dll) and
the editor panel (editor.exe) share one copy. Touched only on the RenderThread.

### 2. Renderer (`Renderer.{h,cpp}`)

`Renderer::Render` already receives `world`. Before the clear
(`Renderer.cpp:217`):

- Small `world->Each` to grab the directional `LightningComponent.Direction`.
- `m_FrameFog = ComputeFog(sunDir, GetFogSettings())`.
- Clear color = `m_FrameFog.Color` when `GetFogSettings().Enabled`; otherwise
  fall back to the existing behavior.
- Expose `const FogFrame& GetFrameFog() const`.

The junk animated `red/green/blue` clear values currently computed in
`RenderThread.cpp` (lines 192–195) are no longer the scene clear source when fog
is enabled.

### 3. MeshRenderPass (`MeshRenderPass.cpp`)

`PerFrameCB` gains the following, mirrored in **both** the VS and PS `cbuffer
PerFrame : register(b0)` HLSL blocks (identical layout required) and the CPU
struct, kept 16-byte aligned:

```
float4 uCameraPos;   // xyz = camera world pos
float4 uFog;         // rgb = fog color, w = density
int    uFogEnabled;  float3 _padFog;
```

Filled from `m_Renderer->GetFrameFog()` and `cam.Position`. In the pixel shader,
after lighting is resolved — applied to **both** the lit and unlit return paths,
just before returning:

```hlsl
if (uFogEnabled != 0) {
    float dist = length(i.WorldPos - uCameraPos.xyz);
    float f    = 1.0 - exp(-uFog.w * dist);
    finalColor.rgb = lerp(finalColor.rgb, uFog.rgb, f);
}
```

The unlit path currently early-returns; it must be restructured so fog applies
to it too (compute the lit/unlit color into a variable, fog once, return).

### 4. Editor UI (`RenderStatsPanel.cpp`)

Alongside the existing shadow/culling controls: `Enabled` checkbox, sliders for
`DayDensity` and `NightDensity`, color pickers for `DayColor` and `NightColor`,
all bound to `GetFogSettings()`.

## Data flow

```
sunDir (ECS snapshot)
   └─> Renderer::Render
         ├─ ComputeFog(sunDir, GetFogSettings())
         ├─ ① clear color = fog color
         └─ ② m_FrameFog ─> MeshRenderPass ─> PerFrameCB
                                                  └─> PS: lerp(color, fogColor,
                                                          1 - exp(-density*dist))
```

Background clear == fog color ⇒ geometry fades into the horizon seamlessly.

## Error / edge handling

- **No directional light found:** elevation defaults to 0 → night fog. Safe, no
  crash.
- **`Enabled == false`:** `uFogEnabled = 0` (PS skips fog); clear color falls
  back to existing behavior.
- Density is always ≥ 0; `exp` handles camera-inside-fog naturally (no NaN
  guard needed).

## Testing

No unit tests for this feature (per decision). Verification is manual:

- Build + run the `editor`. Scrub the day/night cycle and confirm: fog thick and
  dark at night, near-clear at midday, smooth transition, horizon seam invisible
  (background and distant-geometry fade colors agree).
- `test_ecs` / `test_alloc` are unaffected and must still pass.

## Files touched

- `src/engine/src/rendering/Fog.h` (new)
- `src/engine/src/rendering/Fog.cpp` (new)
- `src/engine/src/rendering/Renderer.{h,cpp}`
- `src/engine/src/rendering/passes/MeshRenderPass.cpp`
- `src/engine/src/threading/RenderThread.cpp` (clear-color source)
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`
- `src/engine/CMakeLists.txt` (add `Fog.cpp`)

## Out of scope (YAGNI)

- Height/ground fog, volumetric fog, light shafts.
- Fog affecting UI/text or ImGui.
- Coupling to the (not-yet-done) ambient/brightness fixes #1/#2.
