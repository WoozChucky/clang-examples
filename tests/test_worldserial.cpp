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
    NavMeshConfigComponent cfg{};
    root["Environment"] = BuildEnvironmentJson(fog, sky, dn, cfg);

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

static void T06_player_roundtrip()
{
    PlayerComponent in; in.MoveSpeed = 13.5f; // non-default
    const nlohmann::json j = in;
    const auto out = j.get<PlayerComponent>();
    EXPECT(near(out.MoveSpeed, in.MoveSpeed));
}

static void T07_uirect_roundtrip()
{
    UIRectComponent in;
    in.Size = glm::vec2(123.0f, 45.0f);
    in.Color = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    const nlohmann::json j = in;
    const auto out = j.get<UIRectComponent>();
    EXPECT(near(out.Size.x, in.Size.x) && near(out.Size.y, in.Size.y));
    EXPECT(veq(glm::vec3(out.Color), glm::vec3(in.Color)) && near(out.Color.a, in.Color.a));
}

static void T08_statescope_roundtrip()
{
    StateScopeComponent in;
    in.StateMask = (1u << 1) | (1u << 4); // MainMenu | Paused
    const nlohmann::json j = in;
    const auto out = j.get<StateScopeComponent>();
    EXPECT(out.StateMask == in.StateMask);
}

static void T09_menubutton_roundtrip()
{
    MenuButtonComponent in;
    in.ActionId = 0x00010002u; // Nav/Quit
    in.Normal = glm::vec4(0.1f, 0.2f, 0.3f, 1.0f);
    in.Hover  = glm::vec4(0.4f, 0.5f, 0.6f, 1.0f);
    in.Press  = glm::vec4(0.7f, 0.8f, 0.9f, 1.0f);
    const nlohmann::json j = in;
    const auto out = j.get<MenuButtonComponent>();
    EXPECT(out.ActionId == in.ActionId);
    EXPECT(veq(glm::vec3(out.Normal), glm::vec3(in.Normal)));
    EXPECT(veq(glm::vec3(out.Hover),  glm::vec3(in.Hover)));
    EXPECT(veq(glm::vec3(out.Press),  glm::vec3(in.Press)));
}

static void T10_collider_roundtrip()
{
    ColliderComponent in;
    in.Shape = ColliderShape::Capsule;
    in.Size = glm::vec3(0.5f, 1.25f, 0.0f);
    in.Offset = glm::vec3(1.0f, 2.0f, 3.0f);
    in.IsTrigger = true;
    in.IsStatic = false;
    in.Layer = 0x12u;
    in.Mask = 0x34u;

    const nlohmann::json j = in;
    const auto out = j.get<ColliderComponent>();
    EXPECT(out.Shape == in.Shape);
    EXPECT(veq(out.Size, in.Size));
    EXPECT(veq(out.Offset, in.Offset));
    EXPECT(out.IsTrigger == in.IsTrigger);
    EXPECT(out.IsStatic == in.IsStatic);
    EXPECT(out.Layer == in.Layer);
    EXPECT(out.Mask == in.Mask);
}

static void T11_collider_backward_compatible_defaults()
{
    const nlohmann::json j = {
        {"Shape", ColliderShape::Sphere},
        {"Size", glm::vec3(2.0f, 0.0f, 0.0f)}
    };

    const auto out = j.get<ColliderComponent>();
    EXPECT(out.Shape == ColliderShape::Sphere);
    EXPECT(veq(out.Size, glm::vec3(2.0f, 0.0f, 0.0f)));
    EXPECT(veq(out.Offset, glm::vec3(0.0f)));
    EXPECT(out.IsTrigger == false);
    EXPECT(out.IsStatic == true);
    EXPECT(out.Layer == 1u);
    EXPECT(out.Mask == 0xffffffffu);
}

static void T_navcfg_multiclass_roundtrip() {
    NavMeshConfigComponent in;
    in.ClassCount = 2;
    in.Classes[0] = NavClassConfig{ 0.3f, 1.8f, 0.4f };
    in.Classes[1] = NavClassConfig{ 1.5f, 2.4f, 0.5f };
    const nlohmann::json j = in;
    const auto out = j.get<NavMeshConfigComponent>();
    EXPECT(out.ClassCount == 2);
    EXPECT(near(out.Classes[0].AgentRadius, 0.3f));
    EXPECT(near(out.Classes[1].AgentRadius, 1.5f));
    EXPECT(near(out.Classes[1].AgentHeight, 2.4f));
}
static void T_navcfg_no_classes_defaults_to_one() {
    nlohmann::json j = {
        {"CellSize", 0.3f}, {"CellHeight", 0.2f},
        {"AgentMaxSlope", 45.0f}, {"TileSize", 32.0f}, {"MaxObstacles", 128}
    };
    const auto out = j.get<NavMeshConfigComponent>();
    EXPECT(out.ClassCount == 1);
    EXPECT(near(out.Classes[0].AgentRadius, 0.5f));
}

static void T_navclass_roundtrip() {
    NavClassComponent in; in.ClassId = 3;
    const nlohmann::json j = in;
    const auto out = j.get<NavClassComponent>();
    EXPECT(out.ClassId == 3);
}

// ---------- T20-T22: navigation serialization ----------
static void Test_Navigation() {
    using json = nlohmann::json;

    // T20: NavMeshConfigComponent round-trip with custom values
    {
        FogComponent fog{};                              // default — irrelevant here
        SkyComponent sky{};                              // default — irrelevant here
        DayNightConfigComponent dn{};                    // default — irrelevant here
        NavMeshConfigComponent cfg{};
        cfg.CellSize            = 0.42f;
        cfg.CellHeight          = 0.17f;
        cfg.AgentMaxSlope       = 50.0f;
        cfg.TileSize            = 48.0f;
        cfg.MaxObstacles        = 256;
        cfg.Classes[0].AgentRadius   = 0.66f;
        cfg.Classes[0].AgentHeight   = 2.10f;
        cfg.Classes[0].AgentMaxClimb = 0.55f;

        json root;
        root["Environment"] = BuildEnvironmentJson(fog, sky, dn, cfg);
        const EnvironmentData parsed = ParseEnvironmentJson(root);
        EXPECT(parsed.HasNavMeshConfig);
        EXPECT(near(parsed.NavMeshConfig.CellSize,      0.42f));
        EXPECT(near(parsed.NavMeshConfig.CellHeight,    0.17f));
        EXPECT(near(parsed.NavMeshConfig.Classes[0].AgentRadius,   0.66f));
        EXPECT(near(parsed.NavMeshConfig.Classes[0].AgentHeight,   2.10f));
        EXPECT(near(parsed.NavMeshConfig.Classes[0].AgentMaxClimb, 0.55f));
        EXPECT(near(parsed.NavMeshConfig.AgentMaxSlope, 50.0f));
        EXPECT(near(parsed.NavMeshConfig.TileSize,      48.0f));
        EXPECT(parsed.NavMeshConfig.MaxObstacles == 256);
    }

    // T21: NavMeshSourceComponent per-entity round-trip
    {
        NavMeshSourceComponent src{};
        src.AreaId   = 12;
        src.Geometry = NavMeshGeometrySource::Mesh;
        json j = src;
        NavMeshSourceComponent back = j.get<NavMeshSourceComponent>();
        EXPECT(back.AreaId == 12);
        EXPECT(back.Geometry == NavMeshGeometrySource::Mesh);
    }

    // T22: backward-compat — old world.json without NavMeshConfig still loads, fields default
    {
        json root;
        // Build an Environment with ONLY Fog/Sky/DayNight (no NavMeshConfig key).
        FogComponent fog{};  SkyComponent sky{};  DayNightConfigComponent dn{};
        root["Environment"] = nlohmann::json{
            {"Fog", fog}, {"Sky", sky}, {"DayNight", dn}
        };
        const EnvironmentData parsed = ParseEnvironmentJson(root);
        EXPECT(!parsed.HasNavMeshConfig);   // absent
        // parsed.NavMeshConfig was default-initialized; verify a sentinel default survived.
        EXPECT(near(parsed.NavMeshConfig.CellSize, 0.3f));
    }

    // T23: NavObstacleComponent per-entity round-trip
    {
        NavObstacleComponent obs{};
        obs.Shape  = NavObstacleShape::Box;
        obs.Size   = glm::vec3(2.5f, 1.0f, 0.8f);
        obs.Offset = glm::vec3(0.1f, 0.0f, -0.2f);
        json j = obs;
        NavObstacleComponent back = j.get<NavObstacleComponent>();
        EXPECT(back.Shape == NavObstacleShape::Box);
        EXPECT(near(back.Size.x, 2.5f));
        EXPECT(near(back.Size.y, 1.0f));
        EXPECT(near(back.Size.z, 0.8f));
        EXPECT(near(back.Offset.x,  0.1f));
        EXPECT(near(back.Offset.y,  0.0f));
        EXPECT(near(back.Offset.z, -0.2f));
    }

    // T24: NavAgentComponent per-entity round-trip
    {
        NavAgentComponent agent{};
        agent.MoveSpeed      = 4.25f;
        agent.Radius         = 0.75f;
        agent.ReachedEpsilon = 0.20f;
        json j = agent;
        NavAgentComponent back = j.get<NavAgentComponent>();
        EXPECT(near(back.MoveSpeed,      4.25f));
        EXPECT(near(back.Radius,         0.75f));
        EXPECT(near(back.ReachedEpsilon, 0.20f));
    }

    // T25: NavTargetComponent per-entity round-trip
    {
        NavTargetComponent target{};
        target.Destination = glm::vec3(12.5f, 0.0f, -7.25f);
        json j = target;
        NavTargetComponent back = j.get<NavTargetComponent>();
        EXPECT(near(back.Destination.x,  12.5f));
        EXPECT(near(back.Destination.y,   0.0f));
        EXPECT(near(back.Destination.z, -7.25f));
    }
}

int main()
{
    T00_fog_roundtrip();
    T01_sky_roundtrip();
    T02_daynight_roundtrip();
    T03_environment_roundtrip();
    T04_environment_absent_is_backward_compatible();
    T05_computefog_day_vs_night();
    T06_player_roundtrip();
    T07_uirect_roundtrip();
    T08_statescope_roundtrip();
    T09_menubutton_roundtrip();
    T10_collider_roundtrip();
    T11_collider_backward_compatible_defaults();
    Test_Navigation();
    T_navcfg_multiclass_roundtrip();
    T_navcfg_no_classes_defaults_to_one();
    T_navclass_roundtrip();

    if (g_Failures == 0) { std::printf("All world-serialization tests passed.\n"); return 0; }
    std::printf("%d world-serialization test(s) FAILED.\n", g_Failures);
    return 1;
}
