#pragma once

// Single home for the json (de)serialization of ECS component types, shared by
// WorldManager (save/load) and unit tests. All free functions are `inline` so the
// header can be included in multiple translation units without ODR violations.

#include <nlohmann/json.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "ECS.h"

namespace nlohmann {
    template<> struct adl_serializer<glm::vec3> {
        static void to_json(json& j, const glm::vec3& v) {
            j = json{{"X", v.x}, {"Y", v.y}, {"Z", v.z}};
        }
        static void from_json(const json& j, glm::vec3& v) {
            if (j.is_array() && j.size() == 3) {
                v.x = j[0].get<float>();
                v.y = j[1].get<float>();
                v.z = j[2].get<float>();
            } else {
                j.at("X").get_to(v.x);
                j.at("Y").get_to(v.y);
                j.at("Z").get_to(v.z);
            }
        }
    };

    template<> struct adl_serializer<glm::vec4> {
        static void to_json(json& j, const glm::vec4& v) {
            j = json{{"X", v.x}, {"Y", v.y}, {"Z", v.z}, {"W", v.w}};
        }
        static void from_json(const json& j, glm::vec4& v) {
            if (j.is_array() && j.size() == 4) {
                v.x = j[0].get<float>();
                v.y = j[1].get<float>();
                v.z = j[2].get<float>();
                v.w = j[3].get<float>();
            } else {
                j.at("X").get_to(v.x);
                j.at("Y").get_to(v.y);
                j.at("Z").get_to(v.z);
                j.at("W").get_to(v.w);
            }
        }
    };
}

inline void to_json(nlohmann::json& j, const glm::vec3& t) {
    j = nlohmann::json{{"X", t.x}, {"Y", t.y}, {"Z", t.z}};
}
inline void from_json(const nlohmann::json& j, glm::vec3& t) {
    j.at("X").get_to(t.x);
    j.at("Y").get_to(t.y);
    j.at("Z").get_to(t.z);
}
inline void to_json(nlohmann::json& j, const glm::vec4& t) {
    j = nlohmann::json{{"R", t.r}, {"G", t.g}, {"B", t.b}, {"A", t.a}};
}
inline void from_json(const nlohmann::json& j, glm::vec4& t) {
    j.at("R").get_to(t.r);
    j.at("G").get_to(t.g);
    j.at("B").get_to(t.b);
    j.at("A").get_to(t.a);
}

inline void to_json(nlohmann::json& j, const TransformComponent& t) {
    j = nlohmann::json{{"Position", t.Position}, {"Rotation", t.Rotation}, {"Scale", t.Scale}};
}
inline void from_json(const nlohmann::json& j, TransformComponent& t) {
    j.at("Position").get_to(t.Position);
    j.at("Rotation").get_to(t.Rotation);
    j.at("Scale").get_to(t.Scale);
}

inline void to_json(nlohmann::json& j, const MeshComponent& t) {
    j = nlohmann::json{{"MeshId", t.MeshId}, {"Visible", t.Visible}};
}
inline void from_json(const nlohmann::json& j, MeshComponent& t) {
    j.at("MeshId").get_to(t.MeshId);
    j.at("Visible").get_to(t.Visible);
}

inline void to_json(nlohmann::json& j, const MaterialComponent& t) {
    j = nlohmann::json{{"MaterialId", t.MaterialId}, {"BaseColor", t.BaseColor}, {"Flags", t.Flags}};
}
inline void from_json(const nlohmann::json& j, MaterialComponent& t) {
    j.at("MaterialId").get_to(t.MaterialId);
    j.at("BaseColor").get_to(t.BaseColor);
    j.at("Flags").get_to(t.Flags);
}

inline void to_json(nlohmann::json& j, const LightningComponent& t) {
    j = nlohmann::json{{"Type", t.Type}, {"Direction", t.Direction}, {"Color", t.Color}, {"Intensity", t.Intensity}, {"Range", t.Range}};
}
inline void from_json(const nlohmann::json& j, LightningComponent& t) {
    j.at("Type").get_to(t.Type);
    j.at("Direction").get_to(t.Direction);
    j.at("Color").get_to(t.Color);
    j.at("Intensity").get_to(t.Intensity);
    j.at("Range").get_to(t.Range);
}

inline void to_json(nlohmann::json& j, const TextComponent& t) {
    j = nlohmann::json{{"Text", t.Text}, {"Color", t.Color}, {"FontSize", t.FontSize}};
}
inline void from_json(const nlohmann::json& j, TextComponent& t) {
    j.at("Text").get_to(t.Text);
    j.at("Color").get_to(t.Color);
    j.at("FontSize").get_to(t.FontSize);
}

inline void to_json(nlohmann::json& j, const SunMarker&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json&, SunMarker&) {}

inline void to_json(nlohmann::json& j, const PlayerComponent& t) {
    j = nlohmann::json{{"MoveSpeed", t.MoveSpeed}};
}
inline void from_json(const nlohmann::json& j, PlayerComponent& t) {
    j.at("MoveSpeed").get_to(t.MoveSpeed);
}

inline void to_json(nlohmann::json& j, const UIRectComponent& t) {
    j = nlohmann::json{
        {"Size", {{"X", t.Size.x}, {"Y", t.Size.y}}},
        {"Color", t.Color}   // glm::vec4 round-trips via its registered nlohmann serializer
    };
}
inline void from_json(const nlohmann::json& j, UIRectComponent& t) {
    t.Size.x = j.at("Size").at("X").get<float>();
    t.Size.y = j.at("Size").at("Y").get<float>();
    j.at("Color").get_to(t.Color);
}

inline void to_json(nlohmann::json& j, const StateScopeComponent& t) {
    j = nlohmann::json{{"StateMask", t.StateMask}};
}
inline void from_json(const nlohmann::json& j, StateScopeComponent& t) {
    j.at("StateMask").get_to(t.StateMask);
}

inline void to_json(nlohmann::json& j, const MenuButtonComponent& t) {
    j = nlohmann::json{
        {"ActionId", t.ActionId},
        {"Normal", t.Normal},
        {"Hover",  t.Hover},
        {"Press",  t.Press}
    };
}
inline void from_json(const nlohmann::json& j, MenuButtonComponent& t) {
    j.at("ActionId").get_to(t.ActionId);
    j.at("Normal").get_to(t.Normal);
    j.at("Hover").get_to(t.Hover);
    j.at("Press").get_to(t.Press);
}

inline void to_json(nlohmann::json& j, const ColliderComponent& t) {
    j = nlohmann::json{
        // Shape goes through its underlying int so the JSON format stays the same
        // after switching to enum class (any existing world.json still loads).
        {"Shape", static_cast<uint8_t>(t.Shape)},
        {"Size", t.Size},
        {"Offset", t.Offset},
        {"IsTrigger", t.IsTrigger},
        {"IsStatic", t.IsStatic},
        {"Layer", t.Layer},
        {"Mask", t.Mask}
    };
}
inline void from_json(const nlohmann::json& j, ColliderComponent& t) {
    t.Shape = static_cast<ColliderShape>(j.at("Shape").get<uint8_t>());
    j.at("Size").get_to(t.Size);
    if (j.contains("Offset"))    j.at("Offset").get_to(t.Offset);
    if (j.contains("IsTrigger")) j.at("IsTrigger").get_to(t.IsTrigger);
    if (j.contains("IsStatic"))  j.at("IsStatic").get_to(t.IsStatic);
    if (j.contains("Layer"))     j.at("Layer").get_to(t.Layer);
    if (j.contains("Mask"))      j.at("Mask").get_to(t.Mask);
}

inline void to_json(nlohmann::json& j, const NavMeshSourceComponent& t) {
    j = nlohmann::json{
        {"AreaId",   t.AreaId},
        // Geometry as uint8_t for JSON stability (matches ColliderShape pattern).
        {"Geometry", static_cast<uint8_t>(t.Geometry)}
    };
}
inline void from_json(const nlohmann::json& j, NavMeshSourceComponent& t) {
    if (j.contains("AreaId"))   j.at("AreaId").get_to(t.AreaId);
    if (j.contains("Geometry")) t.Geometry = static_cast<NavMeshGeometrySource>(j.at("Geometry").get<uint8_t>());
}

// ----- Atmosphere components (new) -----

inline void to_json(nlohmann::json& j, const FogComponent& t) {
    j = nlohmann::json{
        {"Enabled", t.Enabled},
        {"DayDensity", t.DayDensity},
        {"NightDensity", t.NightDensity},
        {"DayColor", t.DayColor},
        {"NightColor", t.NightColor}};
}
inline void from_json(const nlohmann::json& j, FogComponent& t) {
    j.at("Enabled").get_to(t.Enabled);
    j.at("DayDensity").get_to(t.DayDensity);
    j.at("NightDensity").get_to(t.NightDensity);
    j.at("DayColor").get_to(t.DayColor);
    j.at("NightColor").get_to(t.NightColor);
}

inline void to_json(nlohmann::json& j, const SkyComponent& t) {
    j = nlohmann::json{
        {"Enabled", t.Enabled},
        {"DayZenith", t.DayZenith},
        {"DayHorizon", t.DayHorizon},
        {"NightZenith", t.NightZenith},
        {"NightHorizon", t.NightHorizon},
        {"SunColor", t.SunColor},
        {"SunRadiusDeg", t.SunRadiusDeg},
        {"SunGlow", t.SunGlow},
        {"MoonColor", t.MoonColor},
        {"MoonRadiusDeg", t.MoonRadiusDeg},
        {"MoonGlow", t.MoonGlow}};
}
inline void from_json(const nlohmann::json& j, SkyComponent& t) {
    j.at("Enabled").get_to(t.Enabled);
    j.at("DayZenith").get_to(t.DayZenith);
    j.at("DayHorizon").get_to(t.DayHorizon);
    j.at("NightZenith").get_to(t.NightZenith);
    j.at("NightHorizon").get_to(t.NightHorizon);
    j.at("SunColor").get_to(t.SunColor);
    j.at("SunRadiusDeg").get_to(t.SunRadiusDeg);
    j.at("SunGlow").get_to(t.SunGlow);
    j.at("MoonColor").get_to(t.MoonColor);
    j.at("MoonRadiusDeg").get_to(t.MoonRadiusDeg);
    j.at("MoonGlow").get_to(t.MoonGlow);
}

inline void to_json(nlohmann::json& j, const DayNightConfigComponent& t) {
    j = nlohmann::json{
        {"CycleSeconds", t.CycleSeconds},
        {"DayBrightness", t.DayBrightness},
        {"MoonIntensity", t.MoonIntensity},
        {"TwilightWidth", t.TwilightWidth},
        {"DayAmbient", t.DayAmbient},
        {"MoonColor", t.MoonColor}};
}
inline void from_json(const nlohmann::json& j, DayNightConfigComponent& t) {
    j.at("CycleSeconds").get_to(t.CycleSeconds);
    j.at("DayBrightness").get_to(t.DayBrightness);
    j.at("MoonIntensity").get_to(t.MoonIntensity);
    j.at("TwilightWidth").get_to(t.TwilightWidth);
    j.at("DayAmbient").get_to(t.DayAmbient);
    j.at("MoonColor").get_to(t.MoonColor);
}

inline void to_json(nlohmann::json& j, const NavMeshConfigComponent& t) {
    j = nlohmann::json{
        {"CellSize",      t.CellSize},
        {"CellHeight",    t.CellHeight},
        {"AgentRadius",   t.AgentRadius},
        {"AgentHeight",   t.AgentHeight},
        {"AgentMaxClimb", t.AgentMaxClimb},
        {"AgentMaxSlope", t.AgentMaxSlope},
        {"TileSize",      t.TileSize},
        {"MaxObstacles",  t.MaxObstacles}
    };
}
inline void from_json(const nlohmann::json& j, NavMeshConfigComponent& t) {
    if (j.contains("CellSize"))      j.at("CellSize").get_to(t.CellSize);
    if (j.contains("CellHeight"))    j.at("CellHeight").get_to(t.CellHeight);
    if (j.contains("AgentRadius"))   j.at("AgentRadius").get_to(t.AgentRadius);
    if (j.contains("AgentHeight"))   j.at("AgentHeight").get_to(t.AgentHeight);
    if (j.contains("AgentMaxClimb")) j.at("AgentMaxClimb").get_to(t.AgentMaxClimb);
    if (j.contains("AgentMaxSlope")) j.at("AgentMaxSlope").get_to(t.AgentMaxSlope);
    if (j.contains("TileSize"))      j.at("TileSize").get_to(t.TileSize);
    if (j.contains("MaxObstacles"))  j.at("MaxObstacles").get_to(t.MaxObstacles);
}

// ----- world.json top-level "Environment" block -----

// Build the "Environment" object value (NOT wrapped — assign to root["Environment"]).
inline nlohmann::json BuildEnvironmentJson(const FogComponent& fog,
                                           const SkyComponent& sky,
                                           const DayNightConfigComponent& dayNight,
                                           const NavMeshConfigComponent& navmesh) {
    return nlohmann::json{
        {"Fog", fog},
        {"Sky", sky},
        {"DayNight", dayNight},
        {"NavMeshConfig", navmesh}};
}

// Parsed result of a root document's "Environment" block. Each Has* flag is true
// only if that sub-object was present; the corresponding value is otherwise left
// at its default. Absent "Environment" (old files) => all flags false.
struct EnvironmentData {
    bool HasFog = false;
    bool HasSky = false;
    bool HasDayNight = false;
    bool HasNavMeshConfig = false;
    FogComponent Fog;
    SkyComponent Sky;
    DayNightConfigComponent DayNight;
    NavMeshConfigComponent NavMeshConfig;
};

inline EnvironmentData ParseEnvironmentJson(const nlohmann::json& root) {
    EnvironmentData e;
    if (!root.contains("Environment")) return e;
    const auto& env = root.at("Environment");
    if (env.contains("Fog"))           { e.Fog           = env.at("Fog").get<FogComponent>();                  e.HasFog = true; }
    if (env.contains("Sky"))           { e.Sky           = env.at("Sky").get<SkyComponent>();                  e.HasSky = true; }
    if (env.contains("DayNight"))      { e.DayNight      = env.at("DayNight").get<DayNightConfigComponent>();  e.HasDayNight = true; }
    if (env.contains("NavMeshConfig")) { e.NavMeshConfig = env.at("NavMeshConfig").get<NavMeshConfigComponent>(); e.HasNavMeshConfig = true; }
    return e;
}
