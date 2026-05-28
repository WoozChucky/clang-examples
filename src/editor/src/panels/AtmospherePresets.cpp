#include "AtmospherePresets.h"

#include <cmath> // std::fabs

namespace {
    constexpr float kEps = 1e-4f;

    bool feq(float a, float b)              { return std::fabs(a - b) <= kEps; }
    bool veq(const glm::vec3& a, const glm::vec3& b) {
        return feq(a.x, b.x) && feq(a.y, b.y) && feq(a.z, b.z);
    }

    bool FogEq(const FogComponent& a, const FogComponent& b) {
        return a.Enabled == b.Enabled
            && feq(a.DayDensity, b.DayDensity) && feq(a.NightDensity, b.NightDensity)
            && veq(a.DayColor, b.DayColor) && veq(a.NightColor, b.NightColor);
    }
    bool SkyEq(const SkyComponent& a, const SkyComponent& b) {
        return a.Enabled == b.Enabled
            && veq(a.DayZenith, b.DayZenith)   && veq(a.DayHorizon, b.DayHorizon)
            && veq(a.NightZenith, b.NightZenith) && veq(a.NightHorizon, b.NightHorizon)
            && veq(a.SunColor, b.SunColor)     && feq(a.SunRadiusDeg, b.SunRadiusDeg)
            && feq(a.SunGlow, b.SunGlow)
            && veq(a.MoonColor, b.MoonColor)   && feq(a.MoonRadiusDeg, b.MoonRadiusDeg)
            && feq(a.MoonGlow, b.MoonGlow);
    }
    // Palette/cycle tunables only — Mode and static-sun fields are deliberately excluded.
    bool DayNightPaletteEq(const DayNightConfigComponent& a, const DayNightConfigComponent& b) {
        return feq(a.CycleSeconds, b.CycleSeconds) && feq(a.DayBrightness, b.DayBrightness)
            && feq(a.MoonIntensity, b.MoonIntensity) && feq(a.TwilightWidth, b.TwilightWidth)
            && feq(a.DayAmbient, b.DayAmbient) && veq(a.MoonColor, b.MoonColor);
    }
}

// Preset palettes. The first field of each DayNight payload mirrors the struct defaults;
// only values that define the "look" are varied per preset. Mode is left at the default
// (DynamicCycle) and ignored by MatchPreset.
const AtmospherePreset kAtmospherePresets[] = {
    {
        "Clear Day",
        /*Fog*/ { true, 0.0f, 0.06f, glm::vec3(0.60f, 0.70f, 0.80f), glm::vec3(0.03f, 0.04f, 0.08f) },
        /*Sky*/ { true,
                  glm::vec3(0.20f, 0.40f, 0.85f), glm::vec3(0.70f, 0.80f, 0.95f),
                  glm::vec3(0.01f, 0.02f, 0.06f), glm::vec3(0.04f, 0.05f, 0.12f),
                  glm::vec3(1.00f, 0.95f, 0.80f), 3.0f, 64.0f,
                  glm::vec3(0.80f, 0.85f, 1.00f), 2.5f, 128.0f },
        /*DayNight*/ { 60.0f, 1.0f, 0.15f, 0.25f, 0.08f, glm::vec3(0.10f, 0.14f, 0.26f) }
    },
    {
        "Overcast",
        /*Fog*/ { true, 0.015f, 0.10f, glm::vec3(0.72f, 0.74f, 0.78f), glm::vec3(0.10f, 0.11f, 0.14f) },
        /*Sky*/ { true,
                  glm::vec3(0.55f, 0.58f, 0.62f), glm::vec3(0.78f, 0.80f, 0.84f),
                  glm::vec3(0.05f, 0.06f, 0.08f), glm::vec3(0.10f, 0.11f, 0.13f),
                  glm::vec3(0.90f, 0.90f, 0.88f), 3.0f, 32.0f,
                  glm::vec3(0.70f, 0.74f, 0.82f), 2.5f, 96.0f },
        /*DayNight*/ { 60.0f, 0.7f, 0.12f, 0.30f, 0.12f, glm::vec3(0.12f, 0.13f, 0.18f) }
    },
    {
        "Sunset",
        /*Fog*/ { true, 0.02f, 0.09f, glm::vec3(0.85f, 0.55f, 0.40f), glm::vec3(0.05f, 0.04f, 0.10f) },
        /*Sky*/ { true,
                  glm::vec3(0.25f, 0.30f, 0.65f), glm::vec3(0.95f, 0.55f, 0.30f),
                  glm::vec3(0.02f, 0.02f, 0.07f), glm::vec3(0.10f, 0.05f, 0.10f),
                  glm::vec3(1.00f, 0.65f, 0.35f), 4.0f, 48.0f,
                  glm::vec3(0.80f, 0.82f, 1.00f), 2.5f, 128.0f },
        /*DayNight*/ { 60.0f, 0.9f, 0.15f, 0.35f, 0.07f, glm::vec3(0.12f, 0.10f, 0.22f) }
    },
    {
        "Night",
        /*Fog*/ { true, 0.05f, 0.14f, glm::vec3(0.10f, 0.12f, 0.20f), glm::vec3(0.02f, 0.03f, 0.07f) },
        /*Sky*/ { true,
                  glm::vec3(0.02f, 0.03f, 0.10f), glm::vec3(0.05f, 0.07f, 0.16f),
                  glm::vec3(0.01f, 0.01f, 0.04f), glm::vec3(0.02f, 0.03f, 0.08f),
                  glm::vec3(1.00f, 0.95f, 0.80f), 3.0f, 64.0f,
                  glm::vec3(0.85f, 0.88f, 1.00f), 3.0f, 96.0f },
        /*DayNight*/ { 60.0f, 0.4f, 0.30f, 0.25f, 0.04f, glm::vec3(0.10f, 0.14f, 0.30f) }
    },
};
const std::size_t kAtmospherePresetCount = sizeof(kAtmospherePresets) / sizeof(kAtmospherePresets[0]);

const char* MatchPreset(const FogComponent& fog,
                        const SkyComponent& sky,
                        const DayNightConfigComponent& dayNight)
{
    for (std::size_t i = 0; i < kAtmospherePresetCount; ++i) {
        const AtmospherePreset& p = kAtmospherePresets[i];
        if (FogEq(fog, p.Fog) && SkyEq(sky, p.Sky) && DayNightPaletteEq(dayNight, p.DayNight))
            return p.Name;
    }
    return nullptr;
}
