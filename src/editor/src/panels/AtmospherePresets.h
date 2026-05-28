#pragma once

#include <cstddef> // std::size_t

#include "ECS.h" // FogComponent, SkyComponent, DayNightConfigComponent

// A named bundle of atmosphere palette values. Selecting a preset in the Atmosphere panel
// stamps these into the live Fog/Sky/DayNight singleton components (stamp-and-forget): the
// values then serialize into world.json as raw data and no preset name is persisted.
//
// The DayNight payload carries only palette/cycle tunables; the panel keeps the user's
// current Mode / static-sun angle when applying a preset (see MatchPreset, which ignores
// those fields).
struct AtmospherePreset {
    const char*             Name;
    FogComponent            Fog;
    SkyComponent            Sky;
    DayNightConfigComponent DayNight;
};

// The hardcoded preset list and its length.
extern const AtmospherePreset kAtmospherePresets[];
extern const std::size_t      kAtmospherePresetCount;

// Returns the name of the preset whose palette matches the given components (field-by-field
// with a small float epsilon), or nullptr if none match ("Custom"). Mode, StaticSunElevDeg,
// StaticSunAzimuthDeg, and ShowSunDisc are intentionally ignored — they are mode controls,
// not palette.
const char* MatchPreset(const FogComponent& fog,
                        const SkyComponent& sky,
                        const DayNightConfigComponent& dayNight);
