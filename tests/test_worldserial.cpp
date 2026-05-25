#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "ComponentSerialization.h" // inline json (de)serializers for components
#include "Fog.h"                     // ComputeFog(const glm::vec3&, const FogComponent&)

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
static bool veq(const glm::vec3& a, const glm::vec3& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

static void T00_fog_roundtrip()
{
    FogComponent in;
    in.Enabled = false;
    in.DayDensity = 0.012f;
    in.NightDensity = 0.21f;
    in.DayColor = glm::vec3(0.11f, 0.22f, 0.33f);
    in.NightColor = glm::vec3(0.44f, 0.55f, 0.66f);

    const nlohmann::json j = in;
    const auto out = j.get<FogComponent>();

    EXPECT(out.Enabled == in.Enabled);
    EXPECT(near(out.DayDensity, in.DayDensity));
    EXPECT(near(out.NightDensity, in.NightDensity));
    EXPECT(veq(out.DayColor, in.DayColor));
    EXPECT(veq(out.NightColor, in.NightColor));
}

static void T01_sky_roundtrip()
{
    SkyComponent in;
    in.Enabled = false;
    in.DayZenith = glm::vec3(0.1f, 0.2f, 0.3f);
    in.SunColor = glm::vec3(0.9f, 0.8f, 0.7f);
    in.SunRadiusDeg = 7.5f;
    in.SunGlow = 33.0f;
    in.MoonColor = glm::vec3(0.5f, 0.6f, 0.7f);
    in.MoonRadiusDeg = 4.25f;
    in.MoonGlow = 99.0f;
    in.DayHorizon = glm::vec3(0.15f, 0.25f, 0.35f);
    in.NightZenith = glm::vec3(0.45f, 0.55f, 0.65f);
    in.NightHorizon = glm::vec3(0.75f, 0.85f, 0.95f);

    const nlohmann::json j = in;
    const auto out = j.get<SkyComponent>();

    EXPECT(out.Enabled == in.Enabled);
    EXPECT(veq(out.DayZenith, in.DayZenith));
    EXPECT(veq(out.SunColor, in.SunColor));
    EXPECT(near(out.SunRadiusDeg, in.SunRadiusDeg));
    EXPECT(near(out.SunGlow, in.SunGlow));
    EXPECT(veq(out.MoonColor, in.MoonColor));
    EXPECT(near(out.MoonRadiusDeg, in.MoonRadiusDeg));
    EXPECT(near(out.MoonGlow, in.MoonGlow));
    EXPECT(veq(out.DayHorizon, in.DayHorizon));
    EXPECT(veq(out.NightZenith, in.NightZenith));
    EXPECT(veq(out.NightHorizon, in.NightHorizon));
}

static void T02_daynight_roundtrip()
{
    DayNightConfigComponent in;
    in.CycleSeconds = 123.0f;
    in.DayBrightness = 0.7f;
    in.MoonIntensity = 0.25f;
    in.TwilightWidth = 0.4f;
    in.DayAmbient = 0.13f;
    in.MoonColor = glm::vec3(0.2f, 0.3f, 0.4f);

    const nlohmann::json j = in;
    const auto out = j.get<DayNightConfigComponent>();

    EXPECT(near(out.CycleSeconds, in.CycleSeconds));
    EXPECT(near(out.DayBrightness, in.DayBrightness));
    EXPECT(near(out.MoonIntensity, in.MoonIntensity));
    EXPECT(near(out.TwilightWidth, in.TwilightWidth));
    EXPECT(near(out.DayAmbient, in.DayAmbient));
    EXPECT(veq(out.MoonColor, in.MoonColor));
}

static void T03_environment_roundtrip()
{
    FogComponent fog; fog.NightDensity = 0.17f; fog.DayColor = glm::vec3(0.1f, 0.2f, 0.3f);
    SkyComponent sky; sky.SunGlow = 42.0f; sky.MoonColor = glm::vec3(0.4f, 0.5f, 0.6f);
    DayNightConfigComponent dn; dn.CycleSeconds = 77.0f; dn.DayAmbient = 0.2f;

    nlohmann::json root;
    root["Entities"] = nlohmann::json::array();
    root["Environment"] = BuildEnvironmentJson(fog, sky, dn);

    const EnvironmentData env = ParseEnvironmentJson(root);
    EXPECT(env.HasFog && env.HasSky && env.HasDayNight);
    EXPECT(near(env.Fog.NightDensity, fog.NightDensity));
    EXPECT(veq(env.Fog.DayColor, fog.DayColor));
    EXPECT(near(env.Sky.SunGlow, sky.SunGlow));
    EXPECT(veq(env.Sky.MoonColor, sky.MoonColor));
    EXPECT(near(env.DayNight.CycleSeconds, dn.CycleSeconds));
    EXPECT(near(env.DayNight.DayAmbient, dn.DayAmbient));
}

static void T04_environment_absent_is_backward_compatible()
{
    // An old world.json: entities only, no "Environment" key.
    nlohmann::json root;
    root["EntityCount"] = 0;
    root["Entities"] = nlohmann::json::array();

    const EnvironmentData env = ParseEnvironmentJson(root); // must not throw
    EXPECT(!env.HasFog);
    EXPECT(!env.HasSky);
    EXPECT(!env.HasDayNight);
}

static void T05_computefog_day_vs_night()
{
    FogComponent fog; // defaults: DayDensity 0, NightDensity 0.09, day/night colors
    // Sun overhead -> sunDir points down (-y): elevation = 1 -> day endpoints.
    const FogFrame day = ComputeFog(glm::vec3(0, -1, 0), fog);
    // Sun below horizon -> sunDir points up (+y): elevation = 0 -> night endpoints.
    const FogFrame night = ComputeFog(glm::vec3(0, 1, 0), fog);

    EXPECT(near(day.Density, fog.DayDensity));
    EXPECT(veq(day.Color, fog.DayColor));
    EXPECT(near(night.Density, fog.NightDensity));
    EXPECT(veq(night.Color, fog.NightColor));
    EXPECT(night.Density > day.Density); // night is foggier with default values
}

int main()
{
    T00_fog_roundtrip();
    T01_sky_roundtrip();
    T02_daynight_roundtrip();
    T03_environment_roundtrip();
    T04_environment_absent_is_backward_compatible();
    T05_computefog_day_vs_night();

    if (g_Failures == 0) { std::printf("All world-serialization tests passed.\n"); return 0; }
    std::printf("%d world-serialization test(s) FAILED.\n", g_Failures);
    return 1;
}
