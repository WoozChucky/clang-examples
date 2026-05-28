# Atmosphere Simplification: Sky Mode + Presets

**Date:** 2026-05-28
**Status:** Approved, pending implementation plan

## Problem

The editor's **Atmosphere** panel (`DayNightPanel.cpp`) exposes ~21 raw controls spread
across three components (`DayNightConfigComponent`, `FogComponent`, `SkyComponent`). The
sheer count makes tuning hard — every knob is visible at once with no hierarchy. There is
also **no way to disable the day/night cycle**: the simulation always animates the sun, so
there is no "static skybox with a fixed look" option.

## Goals

1. Cut the *default* visible knob count from ~21 to ~5 without permanently removing tuning
   capability.
2. Add a **Static** mode: freeze the day/night cycle at a fixed sun angle, fully lit, with
   shadows still cast.
3. Provide **presets** (Clear Day / Overcast / Sunset / Night) as the primary tuning entry
   point.

## Non-Goals

- No JSON-driven / external preset authoring (presets are hardcoded in editor code for now;
  data-driven can come later).
- No changes to the threading model, command ring, or `world.json` top-level structure.
- No new render passes or shader rewrites (one bool plumbed into the existing sky shader CB).

## Key Insight

The sky gradient, fog, and shadows **all derive from the sun's direction** — the `SunMarker`
directional light, whose direction is written each tick by `DayNightSystem`. Downstream passes
(`SkyRenderPass`, `LightingRenderPass`, `ShadowDepthPass`, `Fog::ComputeFog`) read that
direction; none of them know about "time of day".

Therefore **static mode is just: stop animating the sun direction and pin it at a fixed
angle.** The same elevation→color math that the dynamic cycle uses, evaluated at a fixed
elevation, produces the frozen look. No downstream code changes required for the freeze
itself.

## Design

### 1. Data model — `DayNightConfigComponent` (`src/common/include/ECS.h`)

Add four fields. Defaults preserve current behavior exactly (existing worlds load as
DynamicCycle):

```cpp
enum class SkyMode : int { DynamicCycle = 0, Static = 1 };

struct DayNightConfigComponent {
    // existing fields unchanged ...
    SkyMode Mode                = SkyMode::DynamicCycle;
    float   StaticSunElevDeg    = 50.0f;   // 0 = horizon, 90 = overhead (static only)
    float   StaticSunAzimuthDeg = 30.0f;   // compass angle around Y (static only)
    bool    ShowSunDisc         = true;    // static-mode sun-disc visibility
};
```

`FogComponent` and `SkyComponent` are **unchanged** — preserves `world.json` compatibility and
keeps the palette data exactly where it is.

The component remains trivially-copyable (enum is `int`-backed), so it still crosses the
command ring / snapshot path unchanged.

### 2. System — `DayNightSystem` (`src/game/src/game.cpp`)

Branch on `Mode` at the top of `Update`:

- **DynamicCycle** — unchanged. `gameTime` → `phase` → `theta` → sun `dir`.
- **Static** — compute `dir` from `StaticSunElevDeg` / `StaticSunAzimuthDeg`:
  - `elev = radians(StaticSunElevDeg)`, `az = radians(StaticSunAzimuthDeg)`
  - direction toward the sun, then negated into the engine's existing light-direction
    convention (match the sign used by the dynamic path so shadows/fog agree).

After computing `dir`, **both branches share the same downstream math**: derive `elevation`
from `dir.y`, then the existing sun hue / `sunBright` / ambient computation runs verbatim. The
only difference is where `dir` came from (time vs fixed angle). Factor the shared
"elevation → sun color/brightness/ambient" block so both branches call it.

The static branch ignores `CycleSeconds`/`gameTime` entirely (no time term), so the look is
fully deterministic and frozen.

### 3. Sun disc toggle — `SkyRenderPass` (`src/engine/src/rendering/passes/SkyRenderPass.cpp`)

`SkyRenderPass::Render` already builds `cb.Disc` from `SkyComponent`'s sun/moon radii. Add:

- Read the `DayNightConfigComponent` singleton (alongside the existing `SkyComponent` read).
- If `ShowSunDisc == false`, set the sun-disc cosine thresholds in `cb.Disc` to a value the
  per-pixel test can never exceed (radius 0 → `cos(0) = 1`, the `dot > 1` test never passes) →
  disc hidden. No HLSL change.

This bool only matters visually in Static mode in practice, but applies uniformly (in dynamic
mode it defaults `true`).

### 4. Presets (editor-only, hardcoded, stamp-and-forget)

A static table in editor code (e.g. a new `AtmospherePresets.{h,cpp}` under
`src/editor/src/panels/` — added to `src/editor/CMakeLists.txt`):

```cpp
struct AtmospherePreset {
    const char*             Name;
    FogComponent            Fog;
    SkyComponent            Sky;
    DayNightConfigComponent DayNight;  // palette + cycle tunables (Mode left to the panel)
};
```

Presets: **Clear Day, Overcast, Sunset, Night** (extensible). One shared list; meaning is set
by the active `Mode` (dynamic animates the palette day↔night; static pins it at the fixed sun
angle).

**Stamp-and-forget:** selecting a preset pushes up to three `ECSCommand`s (Fog, Sky,
DayNightConfig) through `ECSCommandRing`, exactly like the existing per-field edits in
`PushSingletonEdit`. Values land in the components and serialize to `world.json` as raw data —
**no serialization or data-model change for presets**, and no preset name is persisted.

**"Custom" detection:** after stamping, any manual knob edit makes the current values stop
matching the chosen preset. The dropdown shows the matching preset name, or **"Custom"** when
current Fog/Sky/DayNight values equal-match no preset. Match is a field-by-field equality check
(with small float epsilon).

### 5. Panel rewrite — `DayNightPanel.cpp`

Restructure into a small default view with an `Advanced` collapsing section:

```
Mode:   [Dynamic Cycle ▼]   (Dynamic Cycle | Static)
Preset: [Clear Day ▼]       (presets… | Custom)

── if Dynamic ──            ── if Static ──
  Cycle seconds               Sun elevation (deg)
  Day brightness              Sun azimuth (deg)
  Moon intensity              Day brightness
                              [✓] Show sun disc

▸ Advanced  (collapsed by default)
    Twilight width, Day ambient, Moon fill color,
    Sun glow + radius, Moon glow + radius,
    Sky gradient colors (day/night zenith+horizon),
    Fog: enabled, day/night density, day/night color
```

Every existing knob remains reachable under **Advanced** — nothing is deleted, only demoted.
Default visible count ~5. Edits continue to flow through `PushSingletonEdit` / the command ring
as today.

### 6. Serialization — `ComponentSerialization.h`

Extend `to_json` / `from_json` for `DayNightConfigComponent` with the four new fields.

**Backward compatibility:** the existing `from_json` uses unguarded `j.at()` (throws on missing
key). The four **new** fields must be read with `if (j.contains(...))` guards so existing
`world.json` files (which lack them) still load and default to `DynamicCycle`. The enum
serializes as its underlying `int`.

## Components & Boundaries

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `DayNightConfigComponent` (+`SkyMode`) | Data: mode, static sun angle, disc flag, existing tunables | none (POD) |
| `DayNightSystem` | Compute sun dir (time-driven *or* fixed) + shared lighting math | config singleton, sun light |
| `SkyRenderPass` disc gate | Hide sun disc when `ShowSunDisc==false` | config singleton |
| `AtmospherePresets` table | Named bundles of Fog/Sky/DayNight values | the three components |
| `DayNightPanel` | UI: mode switch, preset dropdown, Custom detection, Advanced section | presets, command ring |
| serialization | Persist new fields, optional on load | nlohmann::json |

## Data Flow

```
Panel (RenderThread)
  ── mode/preset/knob edit ──> ECSCommand ──> ECSCommandRing
GameThread
  ── drains ring ──> DayNightConfigComponent updated
  ── DayNightSystem ──> writes sun light Direction/Color + AtmosphereStateComponent
RenderThread (next snapshot)
  ── SkyRenderPass / LightingRenderPass / ShadowDepthPass / ComputeFog read sun dir ──> frozen or animated look
```

## Error Handling / Edge Cases

- **Ring full:** reuse existing `SM_WARN` on `Push` failure (per [[feedback_logging_over_silent_skip]]).
- **Missing config singleton:** panel already guards (`TextDisabled("No DayNightConfig singleton")`);
  `DayNightSystem` already falls back to a default-constructed config.
- **Old world.json:** missing new keys → defaults (DynamicCycle), guarded reads.
- **Static elevation at/below horizon:** allowed (produces a night/sunset look); shared math
  already handles `elevation == 0`.

## Testing

- **Unit (`test_ecs`):** factor the pure helpers — (a) sun direction from elevation/azimuth,
  (b) preset→Custom equality match — and test them directly.
- **Serialization round-trip:** `DayNightConfigComponent` with new fields; plus a from_json
  case with the new keys absent → defaults to DynamicCycle.
- **Manual:** switch Dynamic↔Static; confirm shadows render in Static; cycle each preset and
  confirm the look; save/reload `world.json` and confirm persistence; toggle Show sun disc.

## Files Touched

- `src/common/include/ECS.h` — `SkyMode` enum + 4 fields.
- `src/game/src/game.cpp` — `DayNightSystem` mode branch + shared math extraction.
- `src/engine/src/rendering/passes/SkyRenderPass.cpp` — disc gate (read config bool).
- `src/common/include/ComponentSerialization.h` — new fields, guarded reads.
- `src/editor/src/panels/DayNightPanel.cpp` — panel rewrite.
- `src/editor/src/panels/AtmospherePresets.{h,cpp}` — new preset table (+ `src/editor/CMakeLists.txt`).

## Rebuild Note

`ECS.h` changes (new component fields) → per CLAUDE.md, rebuild `ecs.dll`, `editor`, and
`game`, then restart the editor.
