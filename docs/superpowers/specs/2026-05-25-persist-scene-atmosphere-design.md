# Persist Scene Atmosphere in world.json — Design

**Date:** 2026-05-25
**Status:** Approved (design)

## Problem

Scene atmosphere — fog, sky, and the day/night cycle config — currently lives in
code defaults, not in the scene file. Fog and Sky are `ENGINE_API` function-local-
static globals (`GetFogSettings()` / `GetSkySettings()`), tweakable through the
editor but **RenderThread-only** and never serialized. `DayNightConfigComponent` is
already an ECS singleton but is **not** written by `WorldManager` either (the save
loop only walks active gameplay entities; singletons live on a hidden reserved
entity). Result: every scene looks identical at runtime, there is no per-scene mood,
and editor tweaks are lost on reload/restart.

These three are **scene-authoring data** and belong in `world.json`. Editor/debug
knobs (wireframe, gizmos, culling toggle, shadow bias/PCF) are **view/quality prefs**
and stay out of the scene file.

## Goal

Persist Fog + Sky + DayNight config per scene in `world.json` by migrating Fog and
Sky to **singleton ECS components** alongside `DayNightConfigComponent`, making the
ECS snapshot the single source of truth for scene look. Time-of-day persistence is
**explicitly out of scope** (deferred — see Non-Goals).

## Non-Goals

- **Time-of-day persistence.** The day/night phase is derived from the absolute game
  clock (`fmod(gameTime, cycle)`, game.cpp:51); there is no stored offset. Persisting
  it needs new plumbing (a phase offset + a `DayNightSystem` change). Deferred to a
  follow-up. Scenes reload at whatever phase the clock is at.
- **Persisting editor/debug prefs** (debug draw, culling, shadow tuning). Those are
  view/quality knobs, not scene data; they remain RenderThread globals / editor state.
- No change to the existing per-entity serialization format beyond adding the new
  top-level block.

## Architecture

Fog and Sky become **singleton ECS components** (`FogComponent`, `SkyComponent`),
joining the existing `DayNightConfigComponent`. They are seeded by the game on world
init (defaults from struct initializers), edited through the ECS command ring, read
by the renderer off the immutable snapshot, and serialized by `WorldManager`.

The `GetFogSettings()` / `GetSkySettings()` global statics are **deleted**. This
removes the bug-prone pattern where the editor (RenderThread) mutated render globals
directly; all atmosphere edits now flow GameThread-owned via `ECSCommand`, exactly
like `DayNightConfigComponent` already does.

Confirmed enabling facts:
- `ECS::Clear()` preserves singleton components — it only clears active gameplay
  entities (ecs.cpp:271). So a world load keeps singletons alive and just overwrites
  their values.
- The snapshot copies the singleton entity id (`snap->m_SingletonEntity = ...`,
  ecs.cpp:246), so the RenderThread reads singletons via `snapshot->GetSingleton<T>()`.
  `LightingRenderPass` already does this for `AtmosphereStateComponent`
  (LightingRenderPass.cpp:233).

## Components

### `FogComponent` (`src/common/include/ECS.h`)

Fields carried over verbatim from today's `FogSettings` (same names, same defaults):

```cpp
struct FogComponent {
    bool      Enabled      = true;
    float     DayDensity   = 0.0f;
    float     NightDensity = 0.09f;
    glm::vec3 DayColor     = glm::vec3(0.60f, 0.70f, 0.80f);
    glm::vec3 NightColor   = glm::vec3(0.03f, 0.04f, 0.08f);
};
```

### `SkyComponent` (`src/common/include/ECS.h`)

Fields carried over verbatim from today's `SkySettings`:

```cpp
struct SkyComponent {
    bool      Enabled       = true;
    glm::vec3 DayZenith     = glm::vec3(0.20f, 0.40f, 0.85f);
    glm::vec3 DayHorizon    = glm::vec3(0.70f, 0.80f, 0.95f);
    glm::vec3 NightZenith   = glm::vec3(0.01f, 0.02f, 0.06f);
    glm::vec3 NightHorizon  = glm::vec3(0.04f, 0.05f, 0.12f);
    glm::vec3 SunColor      = glm::vec3(1.00f, 0.95f, 0.80f);
    float     SunRadiusDeg  = 3.0f;
    float     SunGlow       = 64.0f;
    glm::vec3 MoonColor     = glm::vec3(0.80f, 0.85f, 1.00f);
    float     MoonRadiusDeg = 2.5f;
    float     MoonGlow      = 128.0f;
};
```

Both added to the `ECS_FOR_EACH_REGISTERED_COMPONENT(X)` X-macro and registered in
`ECSCommands.h` `ECSCommandProcessor::ApplyComponentCommand`
(`AddComponent`/`ModifyComponent`) and `RemoveComponentByType` (`RemoveComponent`).
Forgetting either branch is silent — commands queue but the world never changes.

`FogFrame` / `ComputeFog` stay as a pure helper but `ComputeFog` is retargeted to
take `const FogComponent&` instead of `const FogSettings&`.

## Seeding (`src/game/src/game.cpp`)

Alongside the existing init-state seeds (game.cpp:213-217):

```cpp
g_GameState->World.SetSingleton(FogComponent{});
g_GameState->World.SetSingleton(SkyComponent{});
```

Defaults come from the struct initializers, so a brand-new scene and the `runtime`
boot path reproduce today's look exactly.

## Header changes (`Fog.h` / `Sky.h`)

The settings structs move into `ECS.h` (the X-macro and serialization require the
types to be registered components). `Fog.h` / `Sky.h` shrink to the pure compute
helper declarations (`FogFrame`, `ComputeFog(const glm::vec3& sunDir, const
FogComponent&)`, plus any sky helpers), and the `ENGINE_API GetFogSettings()` /
`GetSkySettings()` accessors are removed. `Fog.cpp` / `Sky.cpp` keep the helper
definitions and lose the function-local statics.

## Renderer reads

- `LightingRenderPass` reads `FogComponent` from the snapshot singleton (replacing
  `GetFogSettings()` at LightingRenderPass.cpp:244); if the singleton is absent it
  falls back to a default-constructed `FogComponent` (defensive — should always exist
  once seeded). Calls `ComputeFog(sunDir, fog)`.
- `SkyRenderPass` reads `SkyComponent` from the snapshot singleton the same way.
- `Renderer`'s per-frame fog (`m_FrameFog`, drives the clear color, ~Renderer.cpp:257)
  reads the same `FogComponent` from the active ECS snapshot. The plan must thread the
  active snapshot to where the clear color is computed so the clear stays consistent
  with the lighting pass.

## Serialization (`src/engine/src/utilities/WorldManager.cpp`)

Add `to_json` / `from_json` for `FogComponent`, `SkyComponent`,
`DayNightConfigComponent` (same pattern as the existing component serializers, using
the `glm::vec3` ADL serializer already present).

**Save** (`SaveWorldSnapshot`): after the `Entities` array, write a top-level
`"Environment"` object, reading each via `world->GetSingleton<T>()`:

```jsonc
"Environment": {
    "Fog":      { ... },
    "Sky":      { ... },
    "DayNight": { ... }
}
```

If a singleton is somehow absent, omit that key (don't write nulls).

**Load** (`LoadWorldSnapshot`): after `world->Clear()` (singletons survive), if the
top-level `"Environment"` object is present, `SetSingleton` each sub-object that
exists. If `"Environment"` is absent (old `world.json` files), leave the seeded
defaults untouched. All reads stay inside the existing try/catch so a corrupt block
logs `SM_WARN` and returns `false` without wiping the live world (matching the current
load-hardening behavior).

## Editor UI

Move the Fog and Sky sections out of `RenderStatsPanel.cpp` (lines 39-50) into
`DayNightPanel.cpp`, and rename the panel to **"Atmosphere"** (Fog + Sky + Day/Night
together). The panel reads current values via `world->GetSingleton<FogComponent>()` /
`<SkyComponent>()` / `<DayNightConfigComponent>()` into local copies, draws the
sliders, and on change pushes `ECSCommand::AddComponent(world->SingletonEntity(),
comp)` into `ctx.App->ECSCommandRing` — the exact pattern the panel already uses for
`DayNightConfigComponent` (DayNightPanel.cpp:29-34), with the same ring-full
`SM_WARN`. `RenderStatsPanel` keeps only Frustum / Debug / Shadows (the editor-pref
knobs). Update the panel's window title / any menu entry that referenced "Day / Night".

## Threading correctness

- Save/load run on the GameThread, which owns the master ECS — singletons are
  GameThread-owned, so reading/writing them there is correct.
- The RenderThread only ever **reads** Fog/Sky/DayNight via the immutable snapshot
  singleton — the same path already proven for `AtmosphereStateComponent`.
- Editor edits no longer mutate RenderThread globals; they go through the ECS command
  ring → GameThread, removing the old cross-thread write pattern.

## Build impact

`ECS.h` changes and new registered component types mean: rebuild `ecs.dll`, `editor`,
and `game`; **bump `GAME_API_VERSION`** (the game now references the new component
types and the registered-component set changed); restart the editor (the running
`editor.exe` has the old layout linked in). Per the project build rules.

## Testing

Pure, no GPU — a new `test_*` target (e.g. `test_worldserial`) printing
`All … passed.`:

1. **Round-trip:** construct `FogComponent` / `SkyComponent` /
   `DayNightConfigComponent` with non-default values, `to_json` then `from_json`,
   assert every field is preserved (floats within epsilon, vec3 component-wise).
2. **Backward-compat:** parse a JSON document with `"Entities"` but no `"Environment"`
   key; assert the load path treats it as "no environment present" (the helper that
   extracts the block reports absent / leaves singletons untouched) and does not throw.
3. **ComputeFog:** `ComputeFog(sunDir, FogComponent)` at noon (sun overhead) vs night
   (sun below horizon) yields the expected density ordering and color endpoints.

The serialization functions and `ComputeFog` must be reachable from a unit-test
target without pulling in the renderer/NVRHI; structure the test around the pure
helpers and the `WorldManager` json (de)serializers.

## Risks / Notes

- **ECSCommands registration** is the classic silent-failure spot — both
  `ApplyComponentCommand` and `RemoveComponentByType` branches must be updated.
- **Renderer clear vs lighting fog consistency** — both must read the same
  `FogComponent` from the same snapshot, or the clear color and fog blend diverge.
- **GAME_API_VERSION bump + editor restart** is mandatory this time (ECS.h layout
  change); a missed restart shows up as stale/garbled component data.
