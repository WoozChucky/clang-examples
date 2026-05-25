# Day/Night Cycle Overhaul — Design

**Date:** 2026-05-25
**Status:** Approved (pending implementation plan)

## Goal

Improve the day/night cycle (the original goal of this work; the fog and the
deferred-shading conversion were prerequisites done first). Three fixes:

1. **Day no longer blows out** — cap the directional (sun) brightness at ≤1.0.
2. **Cycle is longer and smoother** — longer default period + eased transitions
   (smoothstep twilight) so colors don't snap.
3. **Night gets a varying "ambient moon"** — a cool, omnidirectional fill that
   varies through the night (brightest at deep-midnight), so night isn't a flat
   single color.

Driven by `game.dll`'s `DayNightSystem`, tunable live from the editor via ImGui.
Built on the deferred `LightingRenderPass` (sub-project 1).

## Scope

In scope: the `DayNightSystem` curve rewrite, a new ambient-color channel through
the ECS into the lighting pass, the lighting-pass shader ambient change, and an
editor ImGui panel to tune the cycle.

Out of scope (YAGNI): a real second "moon" directional light with its own shadows
(the user chose the cheaper omnidirectional ambient moon — night geometry is
flat-lit, accepted); per-cloud/weather; god rays; auto-exposure/tonemapping.

## The one channel

`game.dll` links only `ecs` + `common` (not `Engine.dll`), so all data between
the game, the renderer, and the editor flows through the **ECS**. The deferred
`LightingRenderPass` already receives the `world` pointer (it gathers point
lights), so it can read ECS singletons directly — no extra Renderer plumbing.

## Architecture / data flow

```
DayNightConfigComponent (tunables)  <-- ImGui edits via ECSCommand on world.SingletonEntity()
        | read
DayNightSystem (game.dll), each tick:
   - sun LightningComponent.Direction = orbit  (UNCHANGED -> fog + shadows stay correct)
   - sun .Color + brightness: day warm-white, capped <= DayBrightness (<=1.0),
                              faded to ~0 as the sun drops below the horizon
   - moon ambient color: cool (MoonColor * MoonIntensity), scaled by night depth
                         (brightest at deep-midnight); by day a small neutral DayAmbient
        | writes (SetSingleton/ModifySingleton)
AtmosphereStateComponent { glm::vec4 AmbientColor }   (NEW singleton)
        | read by
LightingRenderPass: uploads AmbientColor as uAmbientColor (new CB field);
   pixel shader ambient term becomes uAmbientColor.rgb
   (replaces the old uAmbient * uDir.Color.rgb, which went dark at night)
```

The directional light keeps representing the **true sun orbit** so the fog
(which reads the SunMarker light direction in the Renderer) and the shadow pass
keep working unchanged. Moonlight is omnidirectional ambient, so there is no
"lit from below" artifact when the sun is under the horizon, and no second
directional light is needed.

## Elevation / curve

`elevation = clamp(-sunDir.y, 0, 1)` — 1 at noon, 0 at/below the horizon (same
metric the current `DayNightSystem` and the fog already use).

- **Sun brightness:** ramped by `elevation` with a smoothstep ease, peak
  `DayBrightness` (≤1.0), reaching ~0 below the horizon. No value exceeds 1.0 →
  no white clip.
- **Twilight:** widen the warm dawn/dusk band via `TwilightWidth` + smoothstep so
  the warm→day and day→night color shifts are gradual, not snappy.
- **Cycle length:** `CycleSeconds` default raised from 10 → **60** (tunable).
- **Moon ambient:** `nightDepth = elevation of the "down" direction` (i.e. how
  far the sun is below the horizon, 0 at dusk/dawn → 1 at deep-midnight). Ambient
  at night = `MoonColor * MoonIntensity * f(nightDepth)` (brightest at midnight).
  By day, ambient = a small neutral `DayAmbient` so lit surfaces keep a floor.
  Cross-faded through twilight so the day-ambient → moon-ambient handover is
  smooth.

## Components (ECS.h — triggers `ecs.dll` + editor + game rebuild and an editor restart)

- **Extend `DayNightConfigComponent`** (singleton, already exists with
  `CycleSeconds`): add `float DayBrightness`, `glm::vec3 MoonColor`,
  `float MoonIntensity`, `float TwilightWidth`, `float DayAmbient`. These are the
  ImGui-tunable + game-readable knobs.
- **New `AtmosphereStateComponent { glm::vec4 AmbientColor }`** (singleton):
  game-written computed ambient, lighting-pass-read. Add `X(AtmosphereStateComponent)`
  to `ECS_FOR_EACH_REGISTERED_COMPONENT`.
- **Register `DayNightConfigComponent`** in `ECSCommandProcessor::ApplyComponentCommand`
  (add/modify dispatch) and `RemoveComponentByType`, so the editor can modify the
  singleton via an `ECSCommand` (per the project's "adding a component type" rule).

## Code units

- **`src/game/src/game.cpp` `DayNightSystem`** — rewrite: read
  `DayNightConfigComponent`; compute the sun direction (orbit, unchanged), sun
  color + capped/faded brightness, and the moon/day ambient color; write the sun
  `LightningComponent` and `SetSingleton`/`ModifySingleton` the
  `AtmosphereStateComponent`. Initialize `AtmosphereStateComponent` at startup
  next to where `DayNightConfigComponent{}` is set.
- **`LightingRenderPass`** — add `glm::vec4 AmbientColor` to `LightFrameCB` and
  `float4 uAmbientColor` to the HLSL `cbuffer` (re-run the `static_assert` size
  guard). Read `world->GetSingleton<AtmosphereStateComponent>()` (fallback to a
  small neutral if absent) and fill `uAmbientColor`. Change the shader ambient
  term from `uAmbient * uDir.Color.rgb` to `uAmbientColor.rgb`; drop the now-unused
  `uAmbient` scalar (adjust CB layout + static_assert accordingly).
- **Editor** — a "Day/Night" ImGui panel (new, or a section in an existing
  panel): read the current `DayNightConfigComponent` from the render snapshot
  (`world->GetSingleton<DayNightConfigComponent>()`), present sliders/color
  pickers for the tunables, and on change push an `ECSCommand` (Modify) targeting
  `world->SingletonEntity()` with the updated component — mirroring how the
  existing editor pushes component edits through `ECSCommandRing`.

## Error / edge handling

- **No `DayNightConfigComponent`:** `DayNightSystem` falls back to defaults (it
  already does for `CycleSeconds`); extend the same fallback to the new fields.
- **No `AtmosphereStateComponent` yet (first frames / before game writes it):**
  the lighting pass uses a small neutral ambient fallback so nothing is pitch
  black.
- **ImGui edit race:** edits go through the `ECSCommandRing` and are applied on
  the GameThread before `DayNightSystem` runs (the established command ordering);
  `DayNightSystem` then recomputes from the new config. The computed
  `AtmosphereStateComponent` is separate from the edited
  `DayNightConfigComponent`, so an edit never clobbers the computed value.

## Testing / verification

No unit tests (renderer/gameplay-visual work; project norm). Manual:
- Rebuild `ecs` + `editor` + `game`, **restart the editor**. Scrub the cycle and
  confirm: daytime is clean (no white blow-out), transitions are slow and smooth
  (no color snap), night is cool and visibly varies (brighter at deep-midnight,
  dimmer near dawn/dusk), and fog + shadows still track the sun as before.
- Exercise the Day/Night ImGui panel: cycle length, day brightness, moon
  color/intensity, twilight width all change the look live.
- `test_ecs` must still pass after the `ECS.h` change (rebuild + run).

## Files (anticipated)

- `src/common/include/ECS.h` — extend `DayNightConfigComponent`; add
  `AtmosphereStateComponent` + `X(...)`.
- `src/common/include/ECSCommands.h` — register `DayNightConfigComponent` in the
  two `ECSCommandProcessor` dispatch branches.
- `src/game/src/game.cpp` — `DayNightSystem` rewrite + startup init of the new
  singleton.
- `src/engine/src/rendering/passes/LightingRenderPass.{h,cpp}` — CB field, shader
  ambient term, read the new singleton.
- Editor ImGui panel source (new or existing) + its `CMakeLists.txt` entry if new.

## Notes

- `Game.h` struct layout is unchanged → no `GAME_API_VERSION` bump for that, but
  the `ECS.h` change still requires rebuilding `ecs.dll`, `editor`, `game` and
  restarting the editor.
- This builds on the deferred lighting pass; the sun-fade + ambient changes live
  entirely in `DayNightSystem` (game) and the lighting pass shader — the G-buffer
  and the fog are untouched.
