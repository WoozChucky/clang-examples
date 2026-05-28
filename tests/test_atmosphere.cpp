#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "Atmosphere.h" // SunDirectionFromAngles

#include <nlohmann/json.hpp>
#include <string>
#include "ECS.h"
#include "ComponentSerialization.h"
#include "AtmospherePresets.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// Engine convention: sun light direction points FROM the sun toward the scene,
// so an overhead noon sun points straight down (dir.y == -1).
static void T00_overhead_points_down()
{
    const glm::vec3 d = SunDirectionFromAngles(90.0f, 0.0f);
    EXPECT(near(d.x, 0.0f));
    EXPECT(near(d.y, -1.0f));
    EXPECT(near(d.z, 0.0f));
}

// At the horizon (elevation 0) the vertical component is zero.
static void T01_horizon_is_flat()
{
    const glm::vec3 d = SunDirectionFromAngles(0.0f, 0.0f);
    EXPECT(near(d.y, 0.0f));
    EXPECT(near(glm::length(d), 1.0f)); // always unit length
}

// Azimuth rotates the horizontal component around +Y.
static void T02_azimuth_rotates_horizontal()
{
    const glm::vec3 d = SunDirectionFromAngles(0.0f, 90.0f);
    EXPECT(near(d.x, 1.0f)); // sin(90deg) on x
    EXPECT(near(d.z, 0.0f)); // cos(90deg) on z
}

// Elevation is clamped to [0,90]: out-of-range stays unit and finite.
static void T03_elevation_clamped()
{
    const glm::vec3 d = SunDirectionFromAngles(140.0f, 0.0f);
    EXPECT(near(glm::length(d), 1.0f));
    EXPECT(near(d.y, -1.0f)); // clamped to 90 -> straight down
}

// New fields survive a to_json -> from_json round-trip.
static void T10_daynight_roundtrip()
{
    DayNightConfigComponent in{};
    in.Mode                = SkyMode::Static;
    in.StaticSunElevDeg    = 12.5f;
    in.StaticSunAzimuthDeg = 200.0f;
    in.ShowSunDisc         = false;

    const nlohmann::json j = in;          // to_json
    DayNightConfigComponent out = j.get<DayNightConfigComponent>(); // from_json

    EXPECT(out.Mode == SkyMode::Static);
    EXPECT(near(out.StaticSunElevDeg, 12.5f));
    EXPECT(near(out.StaticSunAzimuthDeg, 200.0f));
    EXPECT(out.ShowSunDisc == false);
}

// Old world.json (new keys absent) loads with the new fields defaulted to DynamicCycle.
static void T11_daynight_missing_keys_default()
{
    // Only the original six keys present (an old file).
    nlohmann::json j = {
        {"CycleSeconds", 60.0f}, {"DayBrightness", 1.0f}, {"MoonIntensity", 0.15f},
        {"TwilightWidth", 0.25f}, {"DayAmbient", 0.08f},
        {"MoonColor", nlohmann::json::array({0.1f, 0.14f, 0.26f})}
    };
    DayNightConfigComponent out = j.get<DayNightConfigComponent>();
    EXPECT(out.Mode == SkyMode::DynamicCycle);
    EXPECT(out.ShowSunDisc == true);
    EXPECT(near(out.StaticSunElevDeg, 50.0f)); // struct default
}

// A preset's own values match itself.
static void T20_preset_matches_itself()
{
    const AtmospherePreset& p = kAtmospherePresets[0];
    const char* name = MatchPreset(p.Fog, p.Sky, p.DayNight);
    EXPECT(name != nullptr);
    EXPECT(std::string(name) == p.Name);
}

// Mode/static-angle/cycle differences do NOT break a palette match: only the
// palette fields (fog + sky colors/densities, day/night tunables) are compared.
static void T21_match_ignores_mode_and_static_angle()
{
    AtmospherePreset p = kAtmospherePresets[0];
    DayNightConfigComponent dn = p.DayNight;
    dn.Mode             = SkyMode::Static;  // mode differs
    dn.StaticSunElevDeg = 5.0f;             // static angle differs
    const char* name = MatchPreset(p.Fog, p.Sky, dn);
    EXPECT(name != nullptr);
    EXPECT(std::string(name) == p.Name);
}

// Edited palette values match no preset -> "Custom" (nullptr).
static void T22_edited_values_are_custom()
{
    AtmospherePreset p = kAtmospherePresets[0];
    SkyComponent sky = p.Sky;
    sky.DayZenith = glm::vec3(0.123f, 0.456f, 0.789f); // not any preset's value
    const char* name = MatchPreset(p.Fog, sky, p.DayNight);
    EXPECT(name == nullptr);
}

int main()
{
    T00_overhead_points_down();
    T01_horizon_is_flat();
    T02_azimuth_rotates_horizontal();
    T03_elevation_clamped();
    T10_daynight_roundtrip();
    T11_daynight_missing_keys_default();
    T20_preset_matches_itself();
    T21_match_ignores_mode_and_static_angle();
    T22_edited_values_are_custom();
    if (g_Failures == 0) { std::printf("All atmosphere tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d atmosphere test(s) failed.\n", g_Failures);
    return 1;
}
