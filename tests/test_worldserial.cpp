#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "ComponentSerialization.h" // inline json (de)serializers for components

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

int main()
{
    T00_fog_roundtrip();
    T01_sky_roundtrip();
    T02_daynight_roundtrip();

    if (g_Failures == 0) { std::printf("All world-serialization tests passed.\n"); return 0; }
    std::printf("%d world-serialization test(s) FAILED.\n", g_Failures);
    return 1;
}
